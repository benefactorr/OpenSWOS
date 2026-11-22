#!/usr/bin/env python3
"""Fill unresolved SWOS 2026/27 ages using additional birth-date evidence."""
import argparse
import csv
import gzip
import io
import json
import re
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import date
from pathlib import Path

from import_modern_ages import club_key, key, player_age, position, roster_players, same_role, words

UNRESOLVED = {'no_supported_match', 'ambiguous_age'}
CLUB_WORDS = set('football futbol futebol calcio club clube athletic atletico association associacao '
                 'sport sports sporting sportif sportiva societa deportivo deportiva squadra '
                 'fc afc cf sc fk ac as sk sfc ssd usd asd us ss srl spa'.split())


def rows(path):
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            member, = [name for name in archive.namelist() if name.lower().endswith('.csv')]
            with io.TextIOWrapper(archive.open(member), encoding='utf-8-sig', newline='') as stream:
                yield from csv.DictReader(stream)
    else:
        opener = gzip.open if path.suffix == '.gz' else open
        with opener(path, 'rt', encoding='utf-8-sig', newline='') as stream:
            yield from csv.DictReader(stream)


def club_keys(name):
    compact = ''.join(word for word in words(name) if word not in CLUB_WORDS)
    return {value for value in (club_key(name), compact) if len(value) >= 4}


def role_codes(value):
    codes = set()
    for item in re.findall(r'\b(?:GK|D|DF|DM|WB|M|MF|AM|ST|FW|F|CB|RB|LB|RWB|LWB|CDM|CM|CAM|RM|LM|RW|LW|CF)\b', value):
        codes.add('G' if item == 'GK' else 'D' if item in {'D', 'DF', 'WB', 'CB', 'RB', 'LB', 'RWB', 'LWB'}
                  else 'A' if item in {'ST', 'FW', 'F', 'CF'} else 'M')
        if item in {'RW', 'LW', 'AM'}:
            codes.add('A')
    return codes


@dataclass
class Profile:
    identity: str
    source: str
    name: str
    birth: date
    nations: set
    roles: set
    clubs: dict
    recent: bool
    names: set


def read_profiles(args):
    for row in rows(args.players):
        role = position(row['sub_position']) or position(row['position'])
        yield ('Transfermarkt', row['player_id'], row['name'],
               [row['name'], ' '.join((row['first_name'], row['last_name']))],
               row['date_of_birth'][:10], [row['country_of_citizenship']], {role} - {''},
               [(row['current_club_name'], 3), ('#' + row['current_club_id'], 3)],
               row['last_season'].isdigit() and int(row['last_season']) >= 2023)
    for row in rows(args.profiles):
        if row['date_of_death'] or row['current_club_name'] == 'Retired':
            continue
        name = re.sub(r' \(\d+\)$', '', row['player_name'])
        role = position(row['position'].split(' - ')[-1]) or position(row['main_position'])
        yield ('Transfermarkt profiles', row['player_id'], name, [name, row['name_in_home_country']],
               row['date_of_birth'], re.split(r'\s{2,}', row['citizenship']), {role} - {''},
               [(row['current_club_name'], 2), ('#' + row['current_club_id'], 2),
                (row['on_loan_from_club_name'], 1)], row['joined'][:4] >= '2023')
    for row in rows(args.fm23):
        match = re.match(r'(\d+)/(\d+)/(\d{4})\b', row['DOB'])
        if match and row['UID'].isdigit():
            day, month, year = match.groups()
            yield ('FM23', row['UID'], row['Name'], [row['Name']], f'{year}-{month.zfill(2)}-{day.zfill(2)}',
                   [row['Nat']], role_codes(row['Position']), [(row['Club'], 1)], True)
    for row in rows(args.sofifa):
        name = row['name'].removesuffix(' -')
        yield ('SoFIFA', row['player_id'], name, [name, row['full_name']], row['dob'], [row['country_name']],
               role_codes(row['positions']), [(row['club_name'], 2)], True)
    for row in rows(args.fc26):
        yield ('FC26', row['player_id'], row['short_name'], [row['short_name'], row['long_name']],
               row['dob'], [row['nationality_name']], role_codes(row['player_positions']),
               [(row['club_name'], 2), (row['club_loaned_from'], 1)], True)
    for row in rows(args.fc27):
        if row['gender'] != "Men's Football":
            continue
        full = ' '.join((row['first_name'], row['last_name'])).strip()
        name = row['common_name'] or full
        yield ('FC27', row['player_id'], name, [name, full], row['birthdate'],
               [row['nationality']], role_codes(row['position'] + ' ' + row['alternate_positions']),
               [(row['club'], 3)], True)
    for row in rows(args.reep):
        if row['type'] != 'player' or not row['date_of_birth']:
            continue
        role = position(row['position_detail']) or position(row['position'])
        yield ('Reep', row['reep_id'], row['name'], [row['name'], row['full_name']], row['date_of_birth'][:10],
               [row['nationality']], {role} - {''}, [], False)
    for row in rows(args.world_cup):
        yield ('2026 World Cup squads', row['player_id'], row['player'], [row['player']], row['date_of_birth'],
               [row['team']], role_codes(row['position_code']), [(row['club'], 3)], True)


def load_sources(args):
    profiles = {}
    for source, identity, name, names, dob, nations, roles, clubs, recent in read_profiles(args):
        if not identity or not key(name):
            continue
        try:
            birth = date.fromisoformat(dob)
        except ValueError:
            continue
        if not 14 <= player_age(birth) <= 55:
            continue
        # Keep profiles with conflicting dates separate so they can be rejected during matching.
        if source in {'Transfermarkt', 'Transfermarkt profiles', '2026 World Cup squads'}:
            namespace = 'TM'
        elif source in {'SoFIFA', 'FC26', 'FC27'}:
            namespace = 'EA'
        else:
            namespace = source
        profile_key = namespace, identity, birth
        profile = profiles.setdefault(profile_key, Profile(identity, source, name, birth, set(), set(), {}, False, set()))
        if source in {'FC26', 'FC27'}:
            profile.source, profile.name = source, name
        profile.names.update(tuple(words(name)) for name in names if key(name))
        profile.nations.update(key(nation) for nation in nations if key(nation))
        profile.roles.update(roles)
        profile.recent |= recent
        for club, strength in clubs:
            if club.startswith('#'):
                aliases = {club} if club[1:] not in {'', '0', '75', '123', '515'} else set()
            else:
                aliases = club_keys(club) if club not in {'Retired', 'Without Club', 'Unknown', 'Career break'} else set()
            for alias in aliases:
                profile.clubs[alias] = max(strength, profile.clubs.get(alias, 0))
    tm = defaultdict(list)
    for (namespace, identity, _), profile in profiles.items():
        if namespace == 'TM':
            tm[identity].append(profile)
    aliases = {row['club_id']: club_keys(row['name']) | club_keys(row['club_code']) for row in rows(args.clubs)}
    for row in rows(args.appearances):
        club_id = row['player_club_id']
        if row['date'] < '2023-07-01' or club_id not in aliases:
            continue
        for profile in tm.get(row['player_id'], ()):
            profile.clubs.setdefault('#' + club_id, 1)
    for records in tm.values():
        for profile in records:
            for alias, strength in list(profile.clubs.items()):
                if alias.startswith('#'):
                    for label in aliases.get(alias[1:], ()):
                        profile.clubs[label] = max(strength, profile.clubs.get(label, 0))
    return profiles, tm


def make_index(profiles):
    index = defaultdict(dict)
    for profile_key, profile in profiles.items():
        for tokens in profile.names:
            exact = ''.join(tokens)
            index[exact][profile_key] = True
            if len(tokens) < 2:
                continue
            aliases = {tokens[0][0] + ''.join(tokens[1:]), tokens[0][0] + tokens[-1],
                       tokens[0] + tokens[-1]}
            for count in range(2, len(tokens)):
                aliases.add(''.join(tokens[:count]))
                aliases.add(''.join(tokens[-count:]))
            if len(tokens) == 2 and min(map(len, tokens)) >= 2:
                aliases.add(tokens[1] + tokens[0])
            if len(exact) > 22:
                aliases.add(exact[:22])
            for alias in aliases:
                if len(alias) >= 5:
                    index[alias].setdefault(profile_key, False)
    return index


def linked_clubs(group, report, tm):
    current, historical = Counter(), Counter()
    for _, uid, _, _, _, _, _ in group:
        old = report[uid]
        if old['source'] != 'Transfermarkt':
            continue
        for profile in tm.get(old['source_id'], ()):
            if profile.birth.isoformat() != old['date_of_birth']:
                continue
            for club, weight in profile.clubs.items():
                if club.startswith('#'):
                    (current if weight >= 2 else historical)[club] += 1
    score = {club: 3 * current[club] + historical[club] for club in current.keys() | historical.keys()}
    best = sorted(score, key=score.get, reverse=True)
    aliases = club_keys(group[0][2])
    if best:
        first = best[0]
        runner_up = score[best[1]] if len(best) > 1 else 0
        if current[first] >= 3 and score[first] >= runner_up + 5:
            aliases.add(first)
    return aliases


def qualify(profile, exact, names, nations, role, clubs, distinct, cross_source):
    goalkeeper_ok = (role == 0 and 'G' in profile.roles) or (role != 0 and bool(profile.roles - {'G'}))
    if not goalkeeper_ok:
        return None
    same_nat = bool(nations & profile.nations)
    same_pos = any(same_role(role, code) for code in profile.roles)
    club = max((profile.clubs.get(alias, 0) for alias in clubs), default=0)
    full_name = len(names) > 1 and min(map(len, names)) >= 2
    if club and same_nat and same_pos and (exact or len(names) > 1 or club >= 2):
        return 5 + club, 'additional_club_match'
    if club >= 2 and same_pos and exact and full_name:
        return 5, 'additional_club_and_position'
    if club >= 2 and same_nat and exact and full_name:
        return 5, 'additional_club_and_nationality'
    if exact and full_name and distinct and same_nat and cross_source and profile.source != 'Reep':
        if same_pos or len(''.join(names)) >= 12:
            return 4, 'additional_cross_source'
    if exact and full_name and distinct and same_nat and same_pos:
        if profile.recent and profile.source not in {'FM23', 'Transfermarkt profiles'}:
            return 3, 'additional_recent_name_nationality_position'
        if profile.recent and len(''.join(names)) >= 10 and player_age(profile.birth) <= 37:
            return 2, 'additional_register_name_nationality_position'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roster', type=Path)
    parser.add_argument('previous_audit', type=Path)
    for name in ('players', 'profiles', 'appearances', 'clubs', 'fm23', 'sofifa', 'fc26', 'fc27', 'reep', 'world-cup', 'out', 'report'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists() or args.report.exists() or args.out.resolve() == args.report.resolve():
        parser.error('Choose two separate new output paths.')
    data = bytearray(args.roster.read_bytes())
    players = list(roster_players(data))
    audit = {row['player_id']: row for row in rows(args.previous_audit)}
    if len(audit) != len(players) or any(uid not in audit or int(audit[uid]['age']) != data[offset]
                                        for offset, uid, *_ in players):
        raise ValueError('The supplied audit does not match the roster seed.')
    nations_file = Path(__file__).with_name('swos2627_age_second_pass_nationalities.json')
    nations = {int(k): set(map(key, v)) for k, v in json.loads(nations_file.read_text(encoding='utf-8')).items()}
    name_counts = Counter((key(name), nat) for _, _, _, name, nat, _, _ in players)
    profiles, tm = load_sources(args)
    index = make_index(profiles)
    used = {(source, row['source_id']) for row in audit.values()
            if (source := {'Transfermarkt': 'TM', 'Reep': 'Reep'}.get(row['source']))}
    groups = []
    for player in players:
        if not groups or player[0] != groups[-1][-1][0] + 77:
            groups.append([])
        groups[-1].append(player)
    proposals = []
    ambiguous = Counter()
    for group in groups:
        clubs = linked_clubs(group, audit, tm)
        for offset, uid, _, name, nat, role, generated in group:
            if generated or audit[uid]['status'] not in UNRESOLVED:
                continue
            name_key = key(name)
            entries = index.get(name_key, {})
            candidates = []
            concordance = defaultdict(set)
            for match_key, exact in entries.items():
                profile = profiles[match_key]
                if exact and nations.get(nat, set()) & profile.nations:
                    concordance[profile.birth].add(match_key[0])
            for match_key, exact in entries.items():
                profile = profiles[match_key]
                if match_key[:2] in used:
                    continue
                grade = qualify(profile, exact, words(name), nations.get(nat, set()), role, clubs,
                                name_counts[name_key, nat] == 1, len(concordance[profile.birth] - {'Reep'}) > 1 or
                                ('Reep' in concordance[profile.birth] and match_key[0] in {'FM23', 'EA'}))
                if grade:
                    candidates.append((*grade, match_key, profile))
            if not candidates:
                continue
            best = max(candidate[0] for candidate in candidates)
            winners = [candidate for candidate in candidates if candidate[0] == best]
            if len({candidate[3].birth for candidate in winners}) != 1:
                audit[uid]['status'] = 'ambiguous_age'
                ambiguous['competing_birth_dates'] += 1
                continue
            # A competing exact club match with a different DOB is also material.
            if any(candidate[0] >= 5 and candidate[3].birth != winners[0][3].birth for candidate in candidates):
                audit[uid]['status'] = 'ambiguous_age'
                ambiguous['competing_club_match'] += 1
                continue
            proposals.append((offset, uid, min(winners, key=lambda item: item[2])))
    claims = Counter(proposal[2][2][:2] for proposal in proposals)
    updates = Counter()
    for offset, uid, (_, status, match_key, profile) in proposals:
        if claims[match_key[:2]] != 1:
            audit[uid]['status'] = 'ambiguous_age'
            ambiguous['competing_roster_players'] += 1
            continue
        age = player_age(profile.birth)
        data[offset] = age
        audit[uid].update(age=age, status=status, source=profile.source, source_id=profile.identity,
                          source_name=profile.name, date_of_birth=profile.birth.isoformat())
        updates[profile.source, status] += 1
    with args.out.open('xb') as stream:
        stream.write(data)
    with args.report.open('x', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(next(iter(audit.values()))))
        writer.writeheader()
        writer.writerows(audit.values())
    print('Additional matches:', sum(updates.values()), dict(updates))
    print('Ambiguous proposals:', dict(ambiguous))
    print('Still unresolved:', sum(row['status'] in UNRESOLVED for row in audit.values()))


if __name__ == '__main__':
    main()
