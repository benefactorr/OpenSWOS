#!/usr/bin/env python3
"""Update only the age bytes in a World 2026/27 SWOS roster seed."""
import argparse
import csv
import gzip
import json
import re
import struct
import unicodedata
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from uuid import UUID

SEASON_START = date(2026, 7, 1)
FOLD = str.maketrans({'ø': 'o', 'ł': 'l', 'đ': 'd', 'ð': 'd', 'þ': 'th', 'æ': 'ae', 'œ': 'oe', 'ı': 'i'})


def words(value):
    value = unicodedata.normalize('NFKD', value.casefold().translate(FOLD))
    return re.findall(r'[a-z0-9]+', ''.join(c for c in value if not unicodedata.combining(c)))


def key(value):
    return ''.join(words(value))


def club_key(value):
    tokens = words(value)
    # The roster's Santos is the Brazilian club, not Santos Laguna.
    if tokens == ['santos', 'futebol', 'clube']:
        tokens = ['santos']
    return ''.join(w for w in tokens if w not in {'fc', 'fk', 'afc', 'cf', 'sc', 'ac', 'sfc', 'sk'})


def field(raw, start, width):
    return raw[start:start + width].split(b'\0', 1)[0].decode('latin1').strip()


def player_age(born):
    return SEASON_START.year - born.year - ((SEASON_START.month, SEASON_START.day) < (born.month, born.day))


def roster_players(data):
    if data[:8] != b'SW27R02\0':
        raise ValueError('Expected a 2026/27 roster seed in SW27R02 format.')
    clubs, = struct.unpack_from('<I', data, 8)
    offset = 12
    ids = set()
    for _ in range(clubs):
        header = data[offset + 37:offset + 113]
        club = field(header, 5, 17)
        offset += 113
        count, = struct.unpack_from('<I', data, offset)
        offset += 4
        for _ in range(count):
            row = data[offset:offset + 77]
            if len(row) != 77 or row[36] != 0 or row[76] not in (0, 1) or not 14 <= row[75] <= 60:
                raise ValueError(f'Invalid player record at byte {offset}.')
            player_id = str(UUID(row[:36].decode('ascii')))
            if player_id in ids:
                raise ValueError(f'Duplicate player ID {player_id}.')
            ids.add(player_id)
            yield offset + 75, player_id, club, field(row, 40, 23), row[37], row[63] >> 5, row[76]
            offset += 77
    if offset != len(data):
        raise ValueError('Unexpected trailing bytes in roster seed.')


def position(value):
    p = value.lower()
    if 'goal' in p or 'keeper' in p:
        return 'G'
    if p in {'defender', 'sweeper', 'stopper', 'centre-back', 'center-back', 'fullback'} or 'back' in p:
        return 'D'
    if 'wing' in p or 'midfield' in p:
        return 'M'
    if 'attack' in p or 'forward' in p or 'striker' in p:
        return 'A'
    return ''


@dataclass(frozen=True)
class Record:
    name: str
    dob: date
    nationality: str
    role: str
    club: str
    source: str
    identity: str
    season: int


def index_source(path, source, wanted, wanted_clubs, index):
    opener = gzip.open if path.suffix.lower() == '.gz' else open
    with opener(path, 'rt', encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if source == 'Reep' and row.get('type') != 'player':
                continue
            primary = row['name']
            names = {key(primary), key(row.get('full_name', ''))} - {''}
            club = club_key(row.get('current_club_name', ''))
            aliases = set()
            if club in wanted_clubs:
                for name in (primary, row.get('full_name', '')):
                    folded = words(name)
                    if len(folded) > 1:
                        aliases.add(folded[0][:1] + ''.join(folded[1:]))
                        if len(key(name)) > 22:
                            aliases.add(key(name)[:22])
            matches = (names | aliases) & wanted
            if not matches:
                continue
            try:
                dob = date.fromisoformat(row['date_of_birth'][:10])
            except ValueError:
                continue
            nationality = key(row.get('country_of_citizenship') or row.get('nationality', ''))
            role = position(row.get('sub_position', '')) or position(row.get('position', ''))
            identity = row.get('player_id') or row.get('reep_id', '')
            season = row.get('last_season', '')
            rec = Record(primary, dob, nationality, role, club, source, identity,
                         int(season) if season.isdecimal() else 0)
            for name in matches:
                index[name].append((rec, name in names))


def same_role(swos, source):
    return source in ('G', 'D', 'D', 'D', 'MA', 'MA', 'M', 'A')[swos]


def choose(candidates, club, nat, role, distinct, tokens):
    qualified = []
    for rec, exact in candidates:
        same_club = bool(club) and rec.club == club
        same_nat = rec.nationality in nat
        same_pos = same_role(role, rec.role)
        if (role == 0 and rec.role and rec.role != 'G') or (role != 0 and rec.role == 'G'):
            continue
        age = player_age(rec.dob)
        if not 14 <= age <= 60:
            continue
        if rec.source == 'Transfermarkt' and same_club and (same_nat or same_pos) and (exact or same_nat):
            rank = 3
        elif (rec.source == 'Transfermarkt' and rec.season >= 2023 and distinct
              and exact and same_nat and same_pos and len(tokens) > 1):
            rank = 2
        elif (rec.source == 'Reep' and distinct and age <= 39 and exact and same_nat and same_pos
              and len(tokens) > 1 and min(map(len, tokens)) >= 2):
            # The register has historical players but no current club field.
            rank = 1
        else:
            continue
        qualified.append((rank, rec))
    if not qualified:
        return None, 'no_supported_match'
    best = max(rank for rank, _ in qualified)
    winners = [rec for rank, rec in qualified if rank == best]
    ages = {player_age(rec.dob) for rec in winners}
    if len(ages) != 1:
        return None, 'ambiguous_age'
    return min(winners, key=lambda r: (-r.season, r.identity)), {
        3: 'club_match', 2: 'recent_name_nationality_position', 1: 'register_name_nationality_position'
    }[best]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roster', type=Path)
    parser.add_argument('--transfermarkt', type=Path)
    parser.add_argument('--reep', type=Path)
    parser.add_argument('--verified', type=Path, help='Optional reviewed player_id,age CSV')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if not (args.transfermarkt or args.reep or args.verified):
        parser.error('Provide at least one birth-date source or a reviewed UUID age file.')
    if len({p.resolve() for p in (args.roster, args.out, args.report)}) != 3:
        parser.error('Choose separate input, output and report paths.')
    if args.out.exists() or args.report.exists():
        parser.error('Output and report must be new files.')
    verified = {}
    if args.verified:
        with args.verified.open(encoding='utf-8-sig', newline='') as stream:
            for row in csv.DictReader(stream):
                pid, age = str(UUID(row['player_id'])), int(row['age'])
                if not 14 <= age <= 60 or (pid in verified and verified[pid] != age):
                    raise ValueError(f'Invalid or conflicting verified age for {pid}.')
                verified[pid] = age
    data = bytearray(args.roster.read_bytes())
    players = list(roster_players(data))
    missing = verified.keys() - {row[1] for row in players}
    if missing:
        raise ValueError(f'{len(missing)} verified UUIDs do not exist in this roster.')
    mapping = json.loads(Path(__file__).with_name('swos2627_age_nationalities.json').read_text(encoding='utf-8'))
    nations = {int(k): set(map(key, v)) for k, v in mapping.items()}
    name_count = Counter(key(row[3]) for row in players)
    clubs = {club_key(row[2]) for row in players}
    index = defaultdict(list)
    for path, source in ((args.transfermarkt, 'Transfermarkt'), (args.reep, 'Reep')):
        if path:
            index_source(path, source, name_count.keys(), clubs, index)
    summary = Counter()
    with args.report.open('x', encoding='utf-8-sig', newline='') as stream:
        report = csv.writer(stream)
        report.writerow(('player_id', 'club', 'swos_name', 'nationality_code', 'position_code',
                         'old_age', 'age', 'status', 'source', 'source_id', 'source_name', 'date_of_birth'))
        for offset, pid, club, name, nat, role, generated in players:
            old = data[offset]
            match = None
            age, status = old, 'existing_age'
            if generated:
                status = 'generated'
            elif pid in verified:
                age, status = verified[pid], 'verified_uuid'
            elif old == 25:
                name_key = key(name)
                match, status = choose(index.get(name_key, ()), club_key(club), nations.get(nat, ()),
                                       role, name_count[name_key] == 1, words(name))
                if match:
                    age = player_age(match.dob)
            data[offset] = age
            summary[status] += 1
            report.writerow((pid, club, name, nat, role, old, age, status,
                             match.source if match else 'Reviewed UUID' if status == 'verified_uuid' else '',
                             match.identity if match else '', match.name if match else '',
                             match.dob.isoformat() if match else ''))
    with args.out.open('xb') as stream:
        stream.write(data)
    print(f'{len(players)} player records: {dict(sorted(summary.items()))}')


if __name__ == '__main__':
    main()
