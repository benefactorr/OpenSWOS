#!/usr/bin/env python3
"""Install the matched OpenSWOS world and build Modernised's expanded rosters."""
import argparse
import json
import re
import shutil
import struct
import unicodedata
from collections import defaultdict
from pathlib import Path
from tempfile import TemporaryDirectory
from uuid import UUID, uuid5
from zipfile import ZipFile

from swos_team_file import encode_player, encode_team, parse_team

WORLD_ROOT = 'OpenSWOS-World-2627/'
PLAYER_SIZE = 38
TEAM_SIZE = 684


def name_key(name):
    return ''.join(c for c in unicodedata.normalize('NFKD', name).upper()
                   if c.isascii() and c.isalnum())


def read_birth_years(archive):
    """B-team birth years in the DOS mod's own file and player-slot order."""
    try:
        data = archive.read('DATA/BAGE.005')
    except KeyError:
        return {}
    if data[:3] != b'BA\x02' or len(data) < 4:
        return {}
    years = {}
    offset = 4
    for _ in range(data[3]):
        team, _, _, count = struct.unpack_from('<HHBB', data, offset)
        offset += 6
        years[team] = data[offset:offset + count]
        offset += count
    if data[offset:offset + 2] != b'BR':
        raise ValueError('Invalid B-team birth-year table.')
    return years


def read_reserves(archive):
    clubs = defaultdict(list)
    birth_years = read_birth_years(archive)
    for filename in sorted(archive.namelist()):
        if not re.fullmatch(r'DATA/XTW5\.\d{3}', filename):
            continue
        extra = archive.read(filename)
        team = archive.read(filename.replace('XTW5', 'TMW5'))
        count, extra_slots = struct.unpack_from('>HB', extra)
        if (count != int.from_bytes(team[:2], 'big') or not 1 <= extra_slots <= 16
                or len(team) != 2 + count * TEAM_SIZE
                or len(extra) != 3 + count * extra_slots * PLAYER_SIZE):
            raise ValueError(f'Mismatched team and reserve file: {filename}')
        for index in range(count):
            offset = 2 + index * TEAM_SIZE
            entry = parse_team(team[offset:offset + TEAM_SIZE])
            starter_names = {name_key(p['player_name']) for p in entry['players']}
            start = 3 + index * extra_slots * PLAYER_SIZE
            reserves = [extra[pos:pos + PLAYER_SIZE] for pos in
                        range(start, start + extra_slots * PLAYER_SIZE, PLAYER_SIZE)
                        if extra[pos + 3]]
            codes = birth_years.get(int(filename[-3:]) | index << 8, ())
            records = [p['player_name'] for p in entry['players']] + [
                extra[pos + 3:pos + 26].split(b'\0')[0].decode('latin1')
                for pos in range(start, start + extra_slots * PLAYER_SIZE, PLAYER_SIZE)]
            age_by_name = {name_key(name): 126 - code for name, code in zip(records, codes)
                           if name and 14 <= 126 - code <= 60}
            clubs[name_key(entry['team']['team_name'])].append((starter_names, reserves, age_by_name))
    if not clubs:
        raise ValueError('The DOS archive contains no paired TMW5 and XTW5 data.')
    return clubs


def read_ages(path):
    if path is None:
        return {}
    import csv
    with path.open(encoding='utf-8-sig', newline='') as source:
        ages = {str(UUID(row['player_id'])): int(row['age']) for row in csv.DictReader(source)}
    if any(not 14 <= age <= 60 for age in ages.values()):
        raise ValueError('The age CSV contains an age outside 14 to 60.')
    return ages


def encode_squad(entry, dos_clubs, ages):
    club_id = UUID(entry['uuid'])
    starters = entry['players']
    if len(starters) != 16 or not all(p['player_name'] for p in starters):
        raise ValueError(f'Invalid base squad: {entry["team"]["team_name"]}')
    players = [(UUID(p['uuid']), encode_player(p)) for p in starters]
    names = {name_key(p['player_name']) for p in starters}
    candidates = dos_clubs[name_key(entry['team']['team_name'])]
    ranked = sorted(((len(starter_names & names), records, birth_ages)
                     for starter_names, records, birth_ages in candidates),
                    key=lambda match: match[0], reverse=True)
    paired = ranked and ranked[0][0] >= 4 and (len(ranked) == 1 or ranked[0][0] > ranked[1][0])
    birth_ages = ranked[0][2] if paired else {}
    for record in ranked[0][1] if paired else ():
        name = name_key(record[3:26].split(b'\0')[0].decode('latin1'))
        if name and name not in names:
            players.append((uuid5(club_id, 'reserve/' + name), record))
            names.add(name)
    header = encode_team(entry)[:76]
    out = str(club_id).encode('ascii') + b'\0' + header + struct.pack('<I', len(players))
    sourced_ages = 0
    for player_id, record in players:
        name = name_key(record[3:26].split(b'\0')[0].decode('latin1'))
        known_age = ages.get(str(player_id), birth_ages.get(name))
        sourced_ages += known_age is not None
        age = known_age if known_age is not None else 25
        out += str(player_id).encode('ascii') + b'\0' + record + bytes((age, 0))
    return out, len(players), len(players) - 16, sourced_ages


def install(world_zip, dos_zip, destination, ages):
    fingerprint = Path(__file__).resolve().parents[1].joinpath('src/world_data.c').read_text()
    target = destination / 'data_json/worlds/2627'
    with ZipFile(world_zip) as world, ZipFile(dos_zip) as dos:
        manifest = world.read(WORLD_ROOT + 'WORLD.ini')
        actual = re.search(rb'^fingerprint\s*=\s*([a-f0-9]+)\s*$', manifest, re.M)
        if actual is None or f'"{actual[1].decode()}"' not in fingerprint:
            raise ValueError('The World 2026/27 pack does not match this OpenSWOS source.')
        dos_clubs = read_reserves(dos)
        squads = []
        keys = set()
        totals = [0, 0, 0]
        files = sorted(name for name in world.namelist() if name.startswith(WORLD_ROOT + 'teams/')
                       and name.endswith('.json'))
        for name in files:
            for entry in json.loads(world.read(name))['teams']:
                header = entry['team']
                key = (header['country_number'], header['team_ordinal'])
                if key in keys:
                    raise ValueError(f'Duplicate team identity {key}')
                keys.add(key)
                block, *counts = encode_squad(entry, dos_clubs, ages)
                squads.append(block)
                totals = [a + b for a, b in zip(totals, counts)]
        if not squads:
            raise ValueError('The world has no club squads.')
        target.parent.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=target.parent) as directory:
            staged = Path(directory) / '2627'
            staged.mkdir()
            for name in world.namelist():
                if not name.startswith(WORLD_ROOT) or name.endswith('/'):
                    continue
                relative = Path(name.removeprefix(WORLD_ROOT))
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError(f'Invalid world path: {name}')
                output = staged / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(world.read(name))
            with (staged / 'modern_rosters.r02').open('wb') as out:
                out.write(b'SW27R02\0' + struct.pack('<I', len(squads)))
                out.writelines(squads)
            if target.exists():
                # An existing official install can be used if its fingerprint agrees.
                if (target / 'WORLD.ini').read_bytes() != manifest:
                    raise ValueError(f'Existing world differs: {target}')
                shutil.move(staged / 'modern_rosters.r02', target / 'modern_rosters.r02')
            else:
                staged.rename(target)
    print(f'{len(squads)} clubs, {totals[0]} players, {totals[1]} real reserves, '
          f'{totals[2]} sourced ages: {target}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('world_zip', type=Path)
    parser.add_argument('dos_zip', type=Path)
    parser.add_argument('--assets', type=Path, default=Path('bin'))
    parser.add_argument('--ages', type=Path, help='Optional CSV with player_id and age columns')
    args = parser.parse_args()
    try:
        install(args.world_zip, args.dos_zip, args.assets, read_ages(args.ages))
    except (ValueError, OSError, KeyError) as exc:
        parser.exit(1, f'{exc}\n')
