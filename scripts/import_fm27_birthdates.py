#!/usr/bin/env python3
"""Fill unresolved SWOS 2026/27 ages from an FM27 player export."""
import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import date
from pathlib import Path

from extend_modern_ages import UNRESOLVED, club_keys, rows
from import_modern_ages import key, player_age, roster_players, words

COLUMNS = {'uid', 'name', 'full_name', 'common_name', 'birth_date', 'nation_id',
           'club_name', 'club_short_name'}


@dataclass(frozen=True)
class Player:
    uid: str
    name: str
    dob: date
    nation: str
    club: tuple
    names: frozenset


def read_fm(path):
    seen = set()
    for row in rows(path):
        if not COLUMNS <= row.keys():
            raise ValueError(f'Missing FM27 columns: {sorted(COLUMNS - row.keys())}')
        uid = row['uid']
        if not uid.isdecimal() or uid in seen:
            raise ValueError(f'Missing or duplicate FM player ID: {uid!r}')
        seen.add(uid)
        try:
            dob = date.fromisoformat(row['birth_date'])
        except ValueError:
            continue
        if not 14 <= player_age(dob) <= 55 or not row['name'].strip():
            continue
        names = frozenset(tuple(words(row[col])) for col in ('name', 'full_name', 'common_name'))
        club = (row['club_name'].strip(), row['club_short_name'].strip())
        yield Player(uid, row['name'], dob, row['nation_id'], club, names - {()})


def index_players(fm, wanted):
    exact, shortened = defaultdict(set), defaultdict(set)
    labels = defaultdict(set)
    for player in fm:
        if any(player.club):
            for name in player.club:
                for alias in club_keys(name):
                    labels[alias].add(player.club)
        for tokens in player.names:
            normal = ''.join(tokens)
            if normal in wanted:
                exact[normal].add(player)
            if len(tokens) < 2:
                continue
            aliases = {tokens[0][0] + ''.join(tokens[1:]), tokens[0][0] + tokens[-1],
                       tokens[0] + tokens[-1], normal[:22]}
            for alias in (aliases & wanted) - {normal}:
                if len(alias) >= 7:
                    shortened[alias].add(player)
    return exact, shortened, labels


def independent_dates(path, wanted, nationality):
    matches = defaultdict(set)
    for row in rows(path):
        if row['type'] != 'player':
            continue
        nat = nationality.get(key(row['nationality']))
        if nat is None:
            continue
        try:
            birth = date.fromisoformat(row['date_of_birth'][:10])
        except ValueError:
            continue
        if not 14 <= player_age(birth) <= 55:
            continue
        for name in {key(row['name']), key(row['full_name'])} & wanted:
            matches[name, nat].add(birth)
    return matches


def club_links(group, exact, labels, counts, fm_nations):
    label_options = set().union(*(labels[alias] for alias in club_keys(group[0][2])))
    support = Counter()
    for _, _, _, name, nat, _, generated in group:
        tokens, nm = words(name), key(name)
        if generated or len(tokens) < 2 or counts[nm, nat] != 1:
            continue
        options = exact[nm]
        viable = {p.club for p in options if fm_nations.get(p.nation) == nat and any(p.club)}
        if len(viable) == 1:
            support.update(viable)
    ranked = support.most_common(2)
    if ranked:
        club, hits = ranked[0]
        runner = ranked[1][1] if len(ranked) > 1 else 0
        if hits >= 3 and hits >= runner + 2:
            return club, hits
        if hits >= 2 and label_options == {club} and hits > runner:
            return club, hits
    if len(label_options) == 1:
        return next(iter(label_options)), 0
    return None, 0


def match(roster, audit, fm, reep_path):
    players = list(roster_players(roster))
    if len(players) != len(audit) or any(uid not in audit or int(audit[uid]['age']) != roster[off]
                                        for off, uid, *_ in players):
        raise ValueError('The audit and roster do not match.')
    counts = Counter((key(name), nat) for _, _, _, name, nat, _, _ in players)
    wanted = {nm for nm, _ in counts}
    exact, shortened, labels = index_players(fm, wanted)
    nat_evidence = defaultdict(Counter)
    known = {}
    for _, uid, _, name, nat, _, generated in players:
        nm, old = key(name), audit[uid]
        if generated or len(words(name)) < 2 or counts[nm, nat] != 1 or not old['date_of_birth']:
            continue
        options = {p for p in exact[nm] if p.dob.isoformat() == old['date_of_birth']}
        if len(options) == 1:
            p = next(iter(options))
            known.setdefault(p.uid, set()).add(uid)
            nat_evidence[p.nation][nat] += 1
    fm_nations = {}
    for code, evidence in nat_evidence.items():
        nat, count = evidence.most_common(1)[0]
        if count >= 4 and count / sum(evidence.values()) >= 0.85:
            fm_nations[code] = nat
    nation_path = Path(__file__).with_name('swos2627_age_nationalities.json')
    nationalities = defaultdict(set)
    for nat, names in json.loads(nation_path.read_text(encoding='utf-8')).items():
        for name in names:
            nationalities[key(name)].add(int(nat))
    nationality = {k: next(iter(v)) for k, v in nationalities.items() if len(v) == 1}
    reep = independent_dates(reep_path, wanted, nationality)
    groups = []
    for entry in players:
        if not groups or entry[0] != groups[-1][-1][0] + 77:
            groups.append([])
        groups[-1].append(entry)
    proposals = []
    for group in groups:
        label = group[0][2]
        if re.fullmatch(r'SPARE \d+', label):
            continue
        club, strength = club_links(group, exact, labels, counts, fm_nations)
        squad_names = Counter(key(entry[3]) for entry in group)
        for off, uid, _, name, nat, _, generated in group:
            nm, tokens = key(name), words(name)
            if generated or audit[uid]['status'] not in UNRESOLVED or not nm or squad_names[nm] != 1:
                continue
            if re.fullmatch(r'(RESERVE|RES|TRIAL)\s*\d+', name):
                continue
            full = len(tokens) >= 2 and min(map(len, tokens)) >= 2
            long_name = full and len(nm) >= 10
            candidates = defaultdict(set)
            for p in exact[nm]:
                if p.uid in known:
                    continue
                same_nat = fm_nations.get(p.nation) == nat
                if club and p.club == club:
                    if same_nat and (full or strength >= 3 and len(nm) >= 4):
                        candidates['fm27_club'].add(p)
                    elif long_name and strength >= 3 and len(exact[nm]) == 1:
                        candidates['fm27_club'].add(p)
                if long_name and same_nat and counts[nm, nat] == 1 and reep[nm, nat] == {p.dob}:
                    candidates['fm27_corroborated'].add(p)
            if club and strength >= 2 and len(tokens) >= 2:
                for p in shortened[nm] - exact[nm]:
                    if p.uid not in known and p.club == club and fm_nations.get(p.nation) == nat:
                        candidates['fm27_club_alias'].add(p)
            viable = set().union(*candidates.values())
            if len(viable) != 1:
                continue
            p = next(iter(viable))
            status = next(k for k in ('fm27_club', 'fm27_club_alias', 'fm27_corroborated') if p in candidates[k])
            proposals.append((off, uid, p, status))
    duplicates = Counter(p.uid for _, _, p, _ in proposals)
    stats = Counter()
    for off, uid, p, status in proposals:
        if duplicates[p.uid] != 1:
            continue
        age = player_age(p.dob)
        roster[off] = age
        audit[uid].update(age=age, status=status, source='FM27 player export', source_id=p.uid,
                          source_name=p.name, date_of_birth=p.dob.isoformat())
        stats[status] += 1
    print(f'{len(fm_nations)} FM nationality IDs mapped using previously matched birth dates.')
    print(dict(stats), sum(stats.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('roster', 'audit', 'fm27', 'reep', 'out', 'report'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    if args.out.exists() or args.report.exists() or args.out.resolve() == args.report.resolve():
        parser.error('Choose two distinct new output paths.')
    roster = bytearray(args.roster.read_bytes())
    audit = {r['player_id']: r for r in rows(args.audit)}
    match(roster, audit, read_fm(args.fm27), args.reep)
    with args.out.open('xb') as stream:
        stream.write(roster)
    with args.report.open('x', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(next(iter(audit.values()))))
        writer.writeheader()
        writer.writerows(audit.values())


if __name__ == '__main__':
    main()
