#!/usr/bin/env python3
"""Add birth dates to unresolved World 2026/27 roster records."""
import argparse
import csv
import datetime as dt
import io
import json
import re
import zipfile
from collections import Counter, defaultdict
from pathlib import Path

from extend_modern_ages import club_keys, load_sources, make_index, rows, UNRESOLVED
from import_modern_ages import key, player_age, roster_players, same_role, words

SOURCE_KEYS = {'Transfermarkt': 'TM', 'Transfermarkt profiles': 'TM',
               '2026 World Cup squads': 'TM', 'SoFIFA': 'EA', 'FC26': 'EA', 'FC27': 'EA'}
GUIDE_CLUBS = {'CAROLINE SPRINGS GEORGE CROSS FC': 'CAROLINE SPRINGS',
               'MELBOURNE CITY FC': 'MELBOURNE CITY 2',
               'MELBOURNE VICTORY FC': 'MELBOURNE VICTORY 2',
               'ST ALBANS SAINTS SC': 'ST ALBANS SAINTS'}


def guide_records(path):
    active = False
    for line in path.read_text(encoding='utf-8').splitlines():
        heading = re.match(r'^\s*(.*?)\s*[–-]\s+(2025|2026) NPL (SENIOR|UNDER 23) SQUAD\s*$', line)
        if heading:
            label, year, team = heading.groups()
            club = GUIDE_CLUBS.get(label, re.sub(r' (?:SC|FC)$', '', label))
            active = True
        if 'OFFICIAL SEASON GUIDE' in line or 'Birthplace not necessarily' in line:
            active = False
        if not active:
            continue
        parts = re.split(r'\s{2,}', line.strip())
        index = 4 if team == 'SENIOR' else 1
        if len(parts) <= index or not re.fullmatch(r'\d{2}-[A-Za-z]{3}-\d{4}', parts[index]):
            continue
        name = parts[1] if team == 'SENIOR' else parts[0]
        dob = dt.datetime.strptime(parts[index], '%d-%b-%Y').date()
        yield club, name, dob, year


def register_records(path):
    with zipfile.ZipFile(path) as archive:
        for filename in archive.namelist():
            match = re.fullmatch(r'[^/]+/[^/]+/[^/]+/([a-z]{2})\.players\.txt', filename)
            if not match:
                continue
            country = match[1]
            if country == 'gb':
                continue
            for line in archive.read(filename).decode('utf-8-sig').splitlines():
                row = re.match(r'^([^,]+),\s*(GK|G|DF|D|MF|M|FW|F),.*?\bb\.\s+(\d{1,2} [A-Z][a-z]{2} \d{4})', line)
                if row:
                    yield country, row[1].strip(), {'F': 'A'}.get(row[2][0], row[2][0]), dt.datetime.strptime(row[3], '%d %b %Y').date()


def historical_clubs(archive_path, tm):
    associations = defaultdict(set)
    aliases = defaultdict(set)
    with zipfile.ZipFile(archive_path) as archive:
        def read(name):
            with io.TextIOWrapper(archive.open(f'{name}/{name}.csv'), encoding='utf-8-sig', newline='') as stream:
                yield from csv.DictReader(stream)

        for row in read('team_details'):
            aliases[row['club_id']].update(club_keys(row['club_slug']))
            aliases[row['club_id']].update(club_keys(re.sub(r' \(\d+\)$', '', row['club_name'])))
        for row in read('transfer_history'):
            if row['transfer_date'] < '2023-07-01' or row['player_id'] not in tm:
                continue
            for side in ('from', 'to'):
                club = row[f'{side}_team_id']
                if club in aliases:
                    associations[row['player_id']].add('#' + club)
        for row in read('player_performances'):
            season = row['season_name'].split('/')[0]
            club = row['team_id']
            if season in {'22', '23', '24', '25', '26', '2023', '2024', '2025', '2026'} and club in aliases:
                associations[row['player_id']].add('#' + club)
    for pid, profiles in tm.items():
        for profile in profiles:
            for club in associations[pid]:
                profile.clubs.setdefault(club, 1)
            for club, weight in list(profile.clubs.items()):
                if club.startswith('#'):
                    for alias in aliases.get(club[1:], ()):
                        profile.clubs[alias] = max(weight, profile.clubs.get(alias, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roster', type=Path)
    parser.add_argument('previous_audit', type=Path)
    for arg in ('players', 'profiles', 'appearances', 'clubs', 'fm23', 'sofifa', 'fc26', 'fc27', 'reep',
                'world-cup', 'history', 'reep-aliases', 'register', 'out', 'report'):
        parser.add_argument('--' + arg, type=Path, required=True)
    parser.add_argument('--guide', type=Path, action='append', required=True)
    args = parser.parse_args()
    if args.out.exists() or args.report.exists() or args.out.resolve() == args.report.resolve():
        parser.error('Choose two distinct new output paths.')
    roster = bytearray(args.roster.read_bytes())
    players = list(roster_players(roster))
    audit = {r['player_id']: r for r in rows(args.previous_audit)}
    if len(players) != len(audit) or any(uid not in audit or int(audit[uid]['age']) != roster[off]
                                        for off, uid, *_ in players):
        raise ValueError('The audit and roster do not match.')
    mapping = Path(__file__).with_name('swos2627_age_second_pass_nationalities.json')
    nations = {int(k): set(map(key, v)) for k, v in json.loads(mapping.read_text(encoding='utf-8')).items()}
    nat_file = Path(__file__).with_name('swos2627_age_register_countries.json')
    iso = json.loads(nat_file.read_text(encoding='utf-8'))
    profiles, tm = load_sources(args)
    historical_clubs(args.history, tm)
    reep = {identity: profile for (source, identity, _), profile in profiles.items() if source == 'Reep'}
    for row in rows(args.reep_aliases):
        if row['reep_id'] in reep and key(row['alias']):
            reep[row['reep_id']].names.add(tuple(words(row['alias'])))
    for row in rows(args.reep):
        profile = reep.get(row['reep_id'])
        if not profile or not row['key_transfermarkt']:
            continue
        for tm_profile in tm.get(row['key_transfermarkt'], ()):
            if tm_profile.birth == profile.birth and (tm_profile.names & profile.names):
                tm_profile.names.update(profile.names)
    idx = make_index(profiles)
    guide = defaultdict(list)
    for path in args.guide:
        for club, name, dob, year in guide_records(path):
            tokens = words(name)
            if len(tokens) >= 2:
                for alias in {key(name), tokens[0][0] + ''.join(tokens[1:])}:
                    guide[club, alias].append((name, dob, year))
    register = defaultdict(list)
    for country, name, role, dob in register_records(args.register):
        register[key(name), country].append((name, role, dob))
    claimed = {(SOURCE_KEYS.get(r['source'], r['source']), r['source_id']) for r in audit.values() if r['source_id']}
    counts = Counter((key(name), nat) for _, _, _, name, nat, _, _ in players)
    club_counts = Counter((club, key(name)) for _, _, club, name, _, _, _ in players)
    groups = []
    for entry in players:
        if not groups or entry[0] != groups[-1][-1][0] + 77:
            groups.append([])
        groups[-1].append(entry)
    proposals = []
    for group in groups:
        club = group[0][2]
        if re.fullmatch(r'SPARE \d+', club):
            continue
        aliases = club_keys(club)
        anchor_scores = Counter()
        for _, uid, _, _, _, _, _ in group:
            row = audit[uid]
            if SOURCE_KEYS.get(row['source']) != 'TM':
                continue
            for profile in tm.get(row['source_id'], ()):
                if profile.birth.isoformat() == row['date_of_birth']:
                    for alias, weight in profile.clubs.items():
                        if alias.startswith('#'):
                            anchor_scores[alias] += weight
        anchors = anchor_scores.most_common(2)
        if anchors and anchors[0][1] >= 6 and (len(anchors) == 1 or anchors[0][1] >= anchors[1][1] + 4):
            aliases.add(anchors[0][0])
        for off, uid, _, name, nat, role, generated in group:
            if generated or audit[uid]['status'] not in UNRESOLVED or re.fullmatch(r'(RESERVE|RES|TRIAL) \d+', name):
                continue
            nm, tokens = key(name), words(name)
            if club_counts[club, nm] != 1:
                continue
            options = []
            complete = len(tokens) >= 2 and min(map(len, tokens)) >= 2
            nationalities = nations.get(nat, set())
            entries = idx.get(nm, {})
            for match_key, exact in entries.items():
                p = profiles[match_key]
                if match_key[:2] in claimed or match_key[0] != 'TM':
                    continue
                country_match = bool(p.nations & nationalities)
                position_match = any(same_role(role, r) for r in p.roles)
                club_match = max((p.clubs.get(alias, 0) for alias in aliases), default=0)
                same_keeper = (role == 0 and 'G' in p.roles) or (role != 0 and bool(p.roles - {'G'}))
                if not club_match or not same_keeper:
                    continue
                if country_match and position_match and (exact or len(nm) >= 9 and len(tokens) >= 2):
                    rank = 4 + club_match
                elif complete and exact and len(nm) >= 10 and (country_match or position_match):
                    rank = 3 + club_match
                else:
                    continue
                options.append((rank, 'additional_transfer_history', 'Transfermarkt history', match_key[:2], p.name, p.birth))
            for full, dob, year in guide.get((club, nm), ()):
                if len(tokens) >= 2 and 14 <= player_age(dob) <= 55:
                    options.append((10 + (year == '2026'), 'additional_official_squad',
                                    f'Football Victoria {year}', ('FV', f'{club}/{key(full)}'), full, dob))
            if complete and len(nm) >= 10 and counts[nm, nat] == 1:
                for full, player_role, dob in register.get((nm, iso.get(str(nat), '')), ()):
                    if not same_role(role, player_role) or not 14 <= player_age(dob) <= 37:
                        continue
                    # Exact DOB agreement with another profile protects against old namesakes.
                    if any(profiles[k].birth == dob and nationalities & profiles[k].nations
                           for k, exact in entries.items() if exact and k[:2] not in claimed):
                        options.append((3, 'additional_register_corroboration', 'openfootball players', ('OF', f'{nat}/{nm}'), full, dob))
            if not options or len({x[-1] for x in options}) != 1:
                continue
            proposals.append((off, uid, max(options, key=lambda item: (item[0], item[3]))))
    collisions = Counter(match[3] for _, _, match in proposals)
    summary = Counter()
    for off, uid, (_, status, source, (namespace, identity), name, dob) in proposals:
        if collisions[namespace, identity] != 1:
            continue
        age = player_age(dob)
        roster[off] = age
        audit[uid].update(age=age, status=status, source=source, source_id=identity,
                          source_name=name, date_of_birth=dob.isoformat())
        summary[status] += 1
    with args.out.open('xb') as output:
        output.write(roster)
    with args.report.open('x', encoding='utf-8-sig', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=list(next(iter(audit.values()))))
        writer.writeheader()
        writer.writerows(audit.values())
    print(dict(summary), sum(summary.values()))


if __name__ == '__main__':
    main()
