#!/usr/bin/env python3
"""Set starting 4-4-2 lineups in a SWOS 2026/27 Modernised Career+ roster seed."""
import argparse
import csv
import struct
from collections import Counter
from pathlib import Path
from uuid import UUID

from swos_team_file import POSITIONS, TEAM_NAME_LEN, TEAM_NAME_OFF, parse_player

MAGIC_R02 = b'SW27R02\0'
MAGIC_R07 = b'SW27R07\0'
ID_SIZE = 37
TEAM_SIZE = 76
PLAYER_SIZE = 38
RECORD_SIZE = ID_SIZE + PLAYER_SIZE + 2
PREFERRED_SIZE = 1 + 16 * ID_SIZE

# SWOS's 4-4-2 slot map and positional fallback order from src/player_data.c.
FORMATION = (0, 1, 3, 3, 2, 4, 6, 6, 5, 7, 7)
POSITION_ORDER = (
    (0, 3, 6, 7, 1, 2, 4, 5),
    (1, 3, 4, 6, 7, 2, 5, 0),
    (2, 3, 5, 6, 7, 1, 4, 0),
    (3, 1, 2, 6, 7, 4, 5, 0),
    (4, 6, 7, 1, 3, 5, 2, 0),
    (5, 6, 7, 2, 3, 4, 1, 0),
    (6, 4, 5, 3, 7, 1, 2, 0),
    (7, 6, 4, 5, 3, 1, 2, 0),
)


def record_id(record):
    raw = record[:ID_SIZE]
    if len(raw) != ID_SIZE or raw[-1] != 0:
        raise ValueError('Malformed roster UUID.')
    name = raw[:-1].decode('ascii')
    identity = str(UUID(name))
    if identity != name.lower():
        raise ValueError(f'Invalid roster UUID: {name}')
    return identity


def starting_squads(data):
    if len(data) < 12 or data[:8] not in (MAGIC_R02, MAGIC_R07):
        raise ValueError('Expected a SW27R02 or SW27R07 starting roster seed.')
    offset = 12
    count, = struct.unpack_from('<I', data, 8)
    seen_clubs, seen_players = set(), set()
    for _ in range(count):
        base = offset
        header_end = base + ID_SIZE + TEAM_SIZE
        club_id = record_id(data[base:header_end])
        if club_id in seen_clubs or header_end + 4 > len(data):
            raise ValueError(f'Invalid or duplicate club: {club_id}')
        seen_clubs.add(club_id)
        nplayers, = struct.unpack_from('<I', data, header_end)
        player_start = header_end + 4
        player_end = player_start + nplayers * RECORD_SIZE
        offset = player_end + (PREFERRED_SIZE if data[:8] == MAGIC_R07 else 0)
        if nplayers < 16 or offset > len(data):
            raise ValueError(f'Incomplete squad: {club_id}')
        records = [data[start:start + RECORD_SIZE]
                   for start in range(player_start, player_end, RECORD_SIZE)]
        for record in records:
            player_id = record_id(record)
            if player_id in seen_players or not 14 <= record[-2] <= 60 or record[-1] not in (0, 1):
                raise ValueError(f'Invalid or duplicate player: {player_id}')
            seen_players.add(player_id)
        header = data[base + ID_SIZE:header_end]
        yield club_id, header, data[base:player_end], records, data[player_end:offset]
    if offset != len(data) or not seen_clubs:
        raise ValueError('Unexpected end of roster file.')


def select_lineup(players):
    # The stock roster price index increases with SWOS base transfer value
    # for the supported range 0..49. Keep database order for ties.
    for player in players:
        if not 0 <= player['price'] <= 49:
            raise ValueError(f'Unsupported SWOS price index: {player["price"]}')
    available = sorted(range(len(players)), key=lambda i: (not players[i]['player_name'], -players[i]['price']))
    if not any(player['position'] == 0 and player['player_name'] for player in players):
        raise ValueError('No natural goalkeeper in the squad.')
    selected = [None] * len(FORMATION)
    for priority in range(len(POSITION_ORDER[0])):
        for slot, position in enumerate(FORMATION):
            if selected[slot] is not None:
                continue
            role = POSITION_ORDER[position][priority]
            candidate = next((i for i in available if players[i]['position'] == role and
                              (position != 0 or players[i]['player_name'])), None)
            if candidate is not None:
                selected[slot] = candidate
                available.remove(candidate)
    reserve_keeper = next((i for i in available if players[i]['position'] == 0), None)
    if reserve_keeper is not None:
        available.remove(reserve_keeper)
        available.insert(0, reserve_keeper)
    return selected + available[:5]


def reorder(data):
    result = bytearray(MAGIC_R07 + data[8:12])
    audit = []
    summary = Counter()
    for club_id, header, original, records, preferred in starting_squads(data):
        result.extend(original)
        summary['clubs'] += 1
        club = header[TEAM_NAME_OFF:TEAM_NAME_OFF + TEAM_NAME_LEN].split(b'\0', 1)[0].decode('latin1')
        if header[24] != 0:
            result.extend(preferred or bytes(PREFERRED_SIZE))
            summary['other tactics'] += 1
            continue
        players = [parse_player(record[ID_SIZE:ID_SIZE + PLAYER_SIZE]) for record in records]
        try:
            selection = select_lineup(players)
        except ValueError as exc:
            raise ValueError(f'{club}: {exc}') from exc
        result.append(1)
        for index in selection:
            result.extend(records[index][:ID_SIZE])
        summary['4-4-2 lineups'] += 1
        for slot, index in enumerate(selection[:len(FORMATION)]):
            position, player = FORMATION[slot], players[index]
            exact = position == player['position']
            summary['natural slots' if exact else 'fallback slots'] += 1
            audit.append((club_id, club, slot + 1, POSITIONS[position],
                          record_id(records[index]), player['player_name'], POSITIONS[player['position']],
                          player['price'], 'natural' if exact else 'fallback'))
    return result, audit, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roster', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists() or args.report.exists() or args.out.resolve() == args.report.resolve():
        parser.error('Choose two distinct new output paths.')
    try:
        result, audit, summary = reorder(args.roster.read_bytes())
        with args.out.open('xb') as output:
            output.write(result)
        with args.report.open('x', encoding='utf-8-sig', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(('club_id', 'club', 'slot', 'target_position', 'player_id', 'player',
                             'natural_position', 'swos_price_index', 'selection'))
            writer.writerows(audit)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'{exc}\n')
    print(dict(summary))


if __name__ == '__main__':
    main()
