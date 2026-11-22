#!/usr/bin/env python3
"""Preview or apply bulk player valuations to the SWOS 2026/27 starting database."""
import argparse
from collections import Counter, defaultdict
import csv
from decimal import Decimal, ROUND_HALF_UP
import gzip
from pathlib import Path
import re
import shutil
import subprocess

from import_league_values import BANDS, normalise, read_csv, write_csv
from pregame_editor import Database, ID_SIZE, VALUES, read_name

SOURCE = 'https://pub-e682421888d945d684bcae8890b0ec20.r2.dev/data/'
TABLES = ('clubs', 'competitions', 'players', 'player_valuations')
CUTOFF = '2026-06-12'
EARLIEST = '2025-07-01'
FX_DATE = '2026-07-01'
FX_RATE = Decimal('0.85973')
COUNTRIES = {'netherlands': 'holland', 'unitedstates': 'usa', 'koreasouth': 'southkorea',
             'turkiye': 'turkey', 'czechia': 'czechrepublic'}
REPORT = ('country', 'division', 'swos_club', 'club_uuid', 'swos_player', 'player_uuid',
          'tm_club_id', 'tm_player_id', 'source_player', 'valuation_date', 'value_eur',
          'converted_gbp', 'old_band', 'old_gbp', 'new_band', 'new_gbp', 'rounding_gbp',
          'limits', 'status')
CLUB_REPORT = ('country', 'division', 'swos_club', 'club_uuid', 'tm_club_id',
               'source_club', 'source_competition', 'shared_names', 'status')


def country_key(value):
    key = normalise(value)
    return COUNTRIES.get(key, key)


def valid_name(name):
    return name and not re.fullmatch(r'(?:trial(?:ist)?|res(?:erve)?|loan)\s*\d*', name, re.I)


def load_table(root, name):
    with gzip.open(root / f'{name}.csv.gz', 'rt', encoding='utf-8-sig', newline='') as stream:
        yield from csv.DictReader(stream)


def download_tables(root):
    root.mkdir(parents=True, exist_ok=True)
    curl = shutil.which('curl.exe') or shutil.which('curl')
    for table in TABLES:
        target = root / f'{table}.csv.gz'
        if not target.exists():
            temp = target.with_suffix('.partial')
            print(f'Downloading {table}…', flush=True)
            try:
                if not curl:
                    raise ValueError('curl is required for --download. You can supply the four CSV files with --source.')
                result = subprocess.run([curl, '--fail', '--silent', '--show-error', '--location',
                                         '--max-time', '120', '--output', str(temp), SOURCE + target.name])
                if result.returncode:
                    raise ValueError(f'Could not download {target.name} (curl exit code {result.returncode}).')
                with gzip.open(temp, 'rt', encoding='utf-8-sig') as stream:
                    if not stream.readline().strip():
                        raise ValueError(f'Empty source table: {table}')
                temp.replace(target)
            finally:
                temp.unlink(missing_ok=True)


def club_key(name):
    ignore = {'afc', 'ac', 'bc', 'cf', 'cfc', 'fc', 'fk', 'ks', 'sc', 'sk', 'ssc',
              'sv', 'tsv', 'us', 'calcio', 'football', 'club', 'sad', 'spa', '1907',
              '1909'}
    return ''.join(normalise(word) for word in re.split(r'[\s.]+', name)
                   if normalise(word) not in ignore)


def nearest_band(gbp, old):
    if 0 <= old < len(BANDS) and abs(BANDS[old] - gbp) == min(abs(p - gbp) for p in BANDS):
        return old
    return min(range(1, len(BANDS)), key=lambda i: (abs(BANDS[i] - gbp), i))


def source_index(root):
    competitions = {r['competition_id']: r['country_name'] for r in load_table(root, 'competitions')}
    clubs = {r['club_id']: r for r in load_table(root, 'clubs')}
    players = {}
    names = defaultdict(lambda: defaultdict(list))
    global_names = defaultdict(set)
    for player in load_table(root, 'players'):
        ident, club_id = player['player_id'], player['current_club_id']
        name = normalise(player['name'])
        if not name or club_id not in clubs or not player['last_season'].isdigit() or int(player['last_season']) < 2025:
            continue
        players[ident] = player
        names[club_id][name].append(player)
        global_names[name].add(ident)
    values = {}
    for row in load_table(root, 'player_valuations'):
        ident, when, amount = row['player_id'], row['date'][:10], row['market_value_in_eur']
        if ident in players and EARLIEST <= when <= CUTOFF and amount.isdigit() and int(amount) > 0:
            latest = values.get(ident)
            if latest is None or when > latest[0]:
                values[ident] = (when, int(amount))
            elif when == latest[0] and latest[1] != int(amount):
                values[ident] = (when, None)
    return competitions, clubs, names, global_names, values


def club_matches(db, source_clubs, competitions, names, excluded, mapping_path):
    by_country = defaultdict(list)
    for ident, row in source_clubs.items():
        country = competitions.get(row['domestic_competition_id'], '')
        if country and names.get(ident):
            by_country[country_key(country)].append(ident)
    clubs = {club.uid: club for club in db.clubs}
    manual = {}
    if mapping_path:
        for row in read_csv(mapping_path, ('tm_club_id', 'swos_club_uuid')):
            ident, uid = row['tm_club_id'], row['swos_club_uuid'].lower()
            if not ident:
                continue
            if (uid not in clubs or ident not in source_clubs or uid in manual or ident in manual.values()
                    or country_key(clubs[uid].country) != country_key(
                        competitions.get(source_clubs[ident]['domestic_competition_id'], ''))):
                raise ValueError(f'Invalid club mapping: {ident} -> {uid}')
            manual[uid] = ident

    records = {}
    proposals = {}
    audits = {}
    for club in db.clubs:
        buf = db.buffers[club.path]
        swos_club = db.club_fields(club)['Team name']
        division = buf[club.offset + 25]
        audit = dict(country=club.country, division=division, swos_club=swos_club, club_uuid=club.uid)
        audits[club.uid] = audit
        entries = [(read_name(buf, offset + 3, 23),
                    read_name(buf, offset - ID_SIZE, ID_SIZE).lower(), offset)
                   for offset in club.player_offsets]
        records[club.uid] = [entry for entry in entries if valid_name(entry[0])]
        country = country_key(club.country)
        if country in excluded:
            audit['status'] = 'excluded_country'
            continue
        if club.uid not in db.projections:
            audit['status'] = 'missing_team_projection'
            continue
        distinct = Counter(normalise(entry[0]) for entry in records[club.uid])
        keys = {name for name, n in distinct.items() if name and n == 1}
        candidates = []
        for ident in by_country.get(country, ()):
            source_keys = names[ident]
            shared = sum(len(source_keys.get(name, ())) == 1 for name in keys)
            same_name = club_key(swos_club) == club_key(source_clubs[ident]['name'])
            candidates.append((shared, same_name, ident))
        candidates.sort(reverse=True)
        if club.uid in manual:
            proposals[club.uid] = (manual[club.uid], 'manual_club_map', None)
        elif candidates:
            shared, same_name, ident = candidates[0]
            next_count = candidates[1][0] if len(candidates) > 1 else 0
            if (same_name and shared >= 2 and shared > next_count) or (shared >= 4 and shared >= next_count + 2):
                proposals[club.uid] = (ident, 'matched_club', shared)
            else:
                audit['status'] = 'no_confident_club_match'
        else:
            audit['status'] = 'country_outside_snapshot'
    claimants = defaultdict(list)
    for uid, (ident, status, shared) in proposals.items():
        exact = club_key(audits[uid]['swos_club']) == club_key(source_clubs[ident]['name'])
        claimants[ident].append((status == 'manual_club_map', exact, shared or 0, uid))
    winners = {}
    for ident, choices in claimants.items():
        choices.sort(reverse=True)
        manual_match, exact, shared, uid = choices[0]
        if len(choices) == 1 or manual_match or (
                exact and not choices[1][1] and shared >= choices[1][2] + 3):
            winners[ident] = uid
    matches = {}
    for uid, (ident, status, shared) in proposals.items():
        src = source_clubs[ident]
        audits[uid].update(tm_club_id=ident, source_club=src['name'],
                           source_competition=src['domestic_competition_id'], shared_names=shared,
                           status=status if winners.get(ident) == uid else 'ambiguous_club')
        if winners.get(ident) == uid:
            matches[uid] = ident
    return records, matches, audits


def player_matches(db, clubs, records, names, global_names, values, matches):
    report, updates = [], []
    for club in db.clubs:
        info = clubs[club.uid]
        ident = matches.get(club.uid)
        buf = db.buffers[club.path]
        squad = records[club.uid]
        squad_names = Counter(normalise(entry[0]) for entry in squad)
        for name, uid, offset in squad:
            old = buf[offset + 32]
            row = {key: info[key] for key in ('country', 'division', 'swos_club', 'club_uuid')}
            row.update(swos_player=name, player_uuid=uid, old_band=old,
                       old_gbp=BANDS[old] if old < len(BANDS) else '', status=info['status'])
            report.append(row)
            if not ident:
                continue
            row['tm_club_id'] = ident
            key = normalise(name)
            candidates = names[ident].get(key, ())
            if not candidates:
                row['status'] = 'club_mismatch' if key in global_names else 'no_source_match'
                continue
            if len(candidates) != 1 or squad_names[key] != 1:
                row['status'] = 'ambiguous_player'
                continue
            player = candidates[0]
            player_id = player['player_id']
            row.update(tm_player_id=player_id, source_player=player['name'])
            when, eur = values.get(player_id, ('', None))
            if eur is None:
                row.update(valuation_date=when, status='no_usable_valuation')
                continue
            if old >= len(BANDS):
                row['status'] = 'invalid_old_band'
                continue
            gbp = int((Decimal(eur) * FX_RATE).to_integral_value(rounding=ROUND_HALF_UP))
            new = nearest_band(gbp, old)
            limits = 'above_max' if gbp > BANDS[-1] else 'below_min' if gbp < BANDS[0] else ''
            row.update(valuation_date=when, value_eur=eur, converted_gbp=gbp, new_band=new,
                       new_gbp=BANDS[new], rounding_gbp=BANDS[new] - gbp, limits=limits,
                       status='unchanged' if old == new else 'change')
            if old != new:
                updates.append((club, offset, new))
    return report, updates


def summarise(report):
    totals = defaultdict(Counter)
    for row in report:
        totals[row['country'], row['division']][row['status']] += 1
    statuses = sorted({status for count in totals.values() for status in count})
    fields = ('country', 'division', 'named_players', 'matched', *statuses)
    rows = [dict(country=country, division=division, named_players=sum(count.values()),
                 matched=count['unchanged'] + count['change'],
                 **{status: count[status] for status in statuses})
            for (country, division), count in sorted(totals.items())]
    return fields, rows


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--install', type=Path, default=Path.cwd())
    cli.add_argument('--source', type=Path, default=Path('bulk_valuation_source'))
    cli.add_argument('--reports', type=Path, required=True, help='New folder for this run')
    cli.add_argument('--clubs', type=Path, help='Optional verified club map')
    cli.add_argument('--exclude-country', action='append', default=['Italy'],
                     help='Preserve another country in addition to Italy')
    cli.add_argument('--download', action='store_true', help='Download missing bulk tables')
    cli.add_argument('--apply', action='store_true', help='Write price bands after making reports')
    args = cli.parse_args()
    try:
        if args.download:
            download_tables(args.source)
        db = Database(args.install, '2026/27')
        excluded = {country_key(c) for c in args.exclude_country}
        excluded |= {country + 'pool' for country in excluded}
        competitions, source_clubs, names, global_names, values = source_index(args.source)
        records, matches, clubs = club_matches(db, source_clubs, competitions, names, excluded, args.clubs)
        report, updates = player_matches(db, clubs, records, names, global_names, values, matches)
        args.reports.mkdir(parents=True, exist_ok=False)
        write_csv(args.reports / 'players.csv', REPORT, report)
        write_csv(args.reports / 'clubs.csv', CLUB_REPORT, clubs.values())
        fields, rows = summarise(report)
        write_csv(args.reports / 'coverage.csv', fields, rows)
        (args.reports / 'source.txt').write_text(
            f'Dataset: {SOURCE}\nValuation dates: {EARLIEST} to {CUTOFF}\n'
            f'GBP per EUR: {FX_RATE} ({FX_DATE})\n', encoding='utf-8')
        print('Clubs:', dict(Counter(row['status'] for row in clubs.values())))
        print('Players:', dict(Counter(row['status'] for row in report)))
        print(f'Reports: {args.reports.resolve()}')
        if args.apply and updates:
            for club, offset, band in updates:
                fields = db.player_fields(club, offset)
                fields['Player value'] = VALUES[band]
                db.edit_player(club, offset, fields)
            print(f'Updated {len(updates)} valuations. Backup: {db.save()}')
        else:
            print('Database unchanged.')
    except (OSError, ValueError, ArithmeticError, KeyError) as exc:
        cli.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
