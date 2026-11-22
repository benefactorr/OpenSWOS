#!/usr/bin/env python3
"""Import 2026/27 Italian market values from a supplied CSV or saved squad pages."""
import argparse
from collections import Counter, defaultdict
import csv
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import gzip
from hashlib import sha1
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
import unicodedata
from urllib.parse import parse_qs, urlsplit

from pregame_editor import Database, ID_SIZE, VALUES, read_name, value_labels

SEASON = '2026/27'
INPUT = ('tm_club_id', 'tm_player_id', 'player_name', 'value_eur')
REPORT = (*INPUT, 'swos_club', 'club_uuid', 'swos_player', 'player_uuid', 'status',
          'old_band', 'old_gbp', 'fx_date', 'gbp_per_eur', 'converted_gbp', 'new_band',
          'new_gbp', 'rounding_gbp', 'limits')
BANDS = tuple(int(label.split('k', 1)[0][1:].replace(',', '')) * 1000
              for label in value_labels(7))
PLAYER_LINK = re.compile(r'/profil/spieler/(\d+)(?:/|$)')
CLUB_LINK = re.compile(r'/[^/]+/(?:startseite|kader)/verein/(\d+)(?:/|$)')
SQUAD_PATH = re.compile(r'/[^/]+/kader/verein/(\d+)(?:/|$)')
EURO = re.compile(r'€\s*(\d+(?:[.,]\d+)?)\s*(m|k|mio\.?|tsd\.?)?', re.I)


def normalise(name):
    name = name.casefold().translate(str.maketrans({'ł': 'l', 'đ': 'd', 'ø': 'o'}))
    return ''.join(c for c in unicodedata.normalize('NFKD', name) if c.isalnum())


def digit_id(value):
    return re.fullmatch(r'[1-9][0-9]*', value or '') is not None


def euros(text):
    match = EURO.fullmatch(text.strip())
    if match is None:
        return None
    number, suffix = match.groups()
    units = {None: 1, 'm': 1000000, 'mio': 1000000, 'k': 1000, 'tsd': 1000}
    return Decimal(number.replace(',', '.')) * units[suffix.lower().rstrip('.') if suffix else None]


class TransfermarktPage(HTMLParser):
    """Read squad table rows, ignoring other tables and nested layout rows."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = {}
        self.players = []
        self.level = 0
        self.table = None
        self.row = None
        self.cell = None
        self.anchor = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'table':
            self.level += 1
            if self.table is None and 'items' in attrs.get('class', '').split():
                self.table = self.level
        if tag == 'tr' and self.table is not None and self.level == self.table:
            self.row = {'names': {}, 'amounts': set()}
        if tag == 'td' and self.row is not None and self.level == self.table:
            self.cell = []
        if tag == 'a':
            href = urlsplit(attrs.get('href', ''))
            if href.netloc and href.hostname not in {'transfermarkt.com', 'www.transfermarkt.com'}:
                return
            club = CLUB_LINK.match(href.path)
            if club and self.table is not None and self.level == self.table and self.row is not None:
                self.links.setdefault(club[1], href.path.split('/')[1])
            player = PLAYER_LINK.search(href.path)
            if self.row is not None and player:
                self.anchor = (player[1], attrs.get('title', ''), [])

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)
        if self.anchor:
            self.anchor[2].append(data)

    def handle_endtag(self, tag):
        if tag == 'a' and self.anchor:
            ident, title, parts = self.anchor
            name = ''.join(parts).strip() or title.strip()
            if name and self.row is not None:
                self.row['names'].setdefault(ident, name)
            self.anchor = None
        if tag == 'td' and self.cell is not None and self.level == self.table:
            amount = euros(''.join(self.cell))
            if amount is not None:
                self.row['amounts'].add(amount)
            self.cell = None
        if tag == 'tr' and self.level == self.table and self.row is not None:
            if len(self.row['names']) == 1:
                ident, name = next(iter(self.row['names'].items()))
                amounts = self.row['amounts']
                if len(amounts) > 1:
                    raise ValueError(f'Conflicting euro amounts for player {ident}.')
                self.players.append((ident, name, next(iter(amounts), None)))
            self.row = None
            self.cell = None
        if tag == 'table':
            if self.level == self.table:
                self.table = None
            self.level = max(0, self.level - 1)


def read_page(path):
    parsed = TransfermarktPage()
    if path.suffix == '.gz':
        with gzip.open(path, 'rt', encoding='utf-8') as stream:
            parsed.feed(stream.read())
    else:
        parsed.feed(path.read_text(encoding='utf-8-sig'))
    parsed.close()
    return parsed


def read_csv(path, fields):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        missing = set(fields) - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f'{path.name} is missing columns: {", ".join(sorted(missing))}')
        return [{key: (value or '').strip() for key, value in row.items() if key}
                for row in reader]


def write_csv(path, fields, rows):
    with path.open('x', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def extract(args):
    page_list = read_csv(args.pages, ('squad_path', 'html_file'))
    output = []
    for row in page_list:
        link = urlsplit(row['squad_path'])
        club = SQUAD_PATH.match(link.path)
        if not club or not (re.search(r'/saison_id/2026(?:/|$)', link.path)
                           or parse_qs(link.query).get('saison_id') == ['2026']):
            raise ValueError(f'Expected a 2026 club squad path: {row["squad_path"]}')
        if row['html_file']:
            file = args.pages.parent / row['html_file']
        elif args.cache:
            key = sha1(row['squad_path'].encode()).hexdigest()
            file = args.cache / 'pages' / f'{key}.html.gz'
        else:
            raise ValueError('Supply html_file or specify --cache.')
        players = read_page(file).players
        if not any(value is not None for _, _, value in players):
            raise ValueError(f'No EUR player values found in {file}. Check the page and its currency.')
        output.extend(dict(zip(INPUT, (club[1], ident, name, value)))
                      for ident, name, value in players)
    if not output:
        raise ValueError('No squad pages listed.')
    write_csv(args.output, INPUT, output)
    print(f'Extracted {len(output)} player rows.')


def list_clubs(args):
    write_csv(args.output, ('tm_club_id', 'swos_club_uuid', 'swos_club', 'swos_division'), (
        {'tm_club_id': '', 'swos_club_uuid': club.uid,
         'swos_club': db.club_fields(club)['Team name'],
         'swos_division': db.buffers[club.path][club.offset + 25]}
        for db, club in italian_clubs(args.install)
    ))


def italian_clubs(install):
    db = Database(install, SEASON)
    for club in db.clubs:
        if club.country.casefold() == 'italy':
            yield db, club


def preview(args):
    from datetime import date

    date.fromisoformat(args.fx_date)
    rate = Decimal(args.gbp_per_eur)
    if not rate.is_finite() or rate <= 0:
        raise ValueError('GBP per EUR must be positive and finite.')
    source = read_csv(args.input, INPUT)
    if not source:
        raise ValueError('No player rows in the input.')
    if any(not digit_id(row[k]) for row in source for k in INPUT[:2]):
        raise ValueError('Transfermarkt club and player IDs must be positive integers.')
    clubs = {club.uid: (db, club) for db, club in italian_clubs(args.install)}
    mapping = {}
    mapped_uuids = set()
    for row in read_csv(args.clubs, ('tm_club_id', 'swos_club_uuid')):
        if not row['tm_club_id']:
            continue
        ident, uuid = row['tm_club_id'], row['swos_club_uuid'].lower()
        if not digit_id(ident) or ident in mapping or uuid in mapped_uuids or uuid not in clubs:
            raise ValueError(f'Invalid or duplicate Italian club mapping: {ident} -> {uuid}')
        mapping[ident] = clubs[uuid]
        mapped_uuids.add(uuid)

    names, uuids = {}, {}
    for ident, (db, club) in mapping.items():
        names[ident] = defaultdict(list)
        uuids[ident] = {}
        buf = db.buffers[club.path]
        for offset in club.player_offsets:
            player = read_name(buf, offset + 3, 23)
            uid = read_name(buf, offset - ID_SIZE, ID_SIZE).lower()
            record = (uid, player, offset)
            names[ident][normalise(player)].append(record)
            uuids[ident][uid] = record

    repeated_source = Counter(row['tm_player_id'] for row in source)
    changes = []
    report = []
    for row in source:
        club_id, player_id, name, value = (row[k] for k in INPUT)
        result = {k: row[k] for k in INPUT}
        result.update(fx_date=args.fx_date, gbp_per_eur=str(rate))
        report.append(result)
        if player_id in repeated_source and repeated_source[player_id] > 1:
            result['status'] = 'duplicate_source_player'
            continue
        if club_id not in mapping:
            result['status'] = 'unmapped_club'
            continue
        db, club = mapping[club_id]
        result.update(swos_club=db.club_fields(club)['Team name'], club_uuid=club.uid)
        override = row.get('swos_player_uuid', '').lower()
        if override:
            candidate = uuids[club_id].get(override)
            candidates = [candidate] if candidate else []
        else:
            key = normalise(name)
            candidates = names[club_id].get(key, ()) if key else ()
        if len(candidates) != 1:
            result['status'] = ('invalid_override' if override else
                                'ambiguous_name' if candidates else 'unmatched_name')
            continue
        uid, player, offset = candidates[0]
        result.update(swos_player=player, player_uuid=uid)
        try:
            amount = Decimal(value)
            if not amount.is_finite() or amount <= 0:
                raise InvalidOperation()
        except InvalidOperation:
            result['status'] = 'missing_value' if not value else 'invalid_value'
            continue
        converted = int((amount * rate).to_integral_value(rounding=ROUND_HALF_UP))
        buf = db.buffers[club.path]
        old = buf[offset + 32]
        if old >= len(BANDS):
            result['status'] = 'invalid_old_band'
            continue
        # Keep the old index if it already has the best price. Otherwise
        # prefer the lower band in a tie, excluding the reserved zero index.
        new = min(range(1, len(BANDS)), key=lambda index:
                  (abs(BANDS[index] - converted), index != old, index))
        if old == 0 and BANDS[0] == BANDS[new]:
            new = old
        limits = 'above_max' if converted > BANDS[-1] else 'below_min' if converted < BANDS[0] else ''
        result.update(old_band=old, old_gbp=BANDS[old], converted_gbp=converted, new_band=new,
                      new_gbp=BANDS[new], rounding_gbp=BANDS[new] - converted, limits=limits,
                      status='unchanged' if old == new else 'change')
        changes.append((db, club, offset, new, result))

    repeated_targets = Counter(row['player_uuid'] for row in report if row.get('player_uuid'))
    for row in report:
        if repeated_targets[row.get('player_uuid')] > 1:
            row['status'] = 'duplicate_target_player'
    for ident, (db, club) in mapping.items():
        if not any(row['tm_club_id'] == ident for row in source):
            continue
        for uid, player, offset in uuids[ident].values():
            if uid in repeated_targets or not player:
                continue
            old = db.buffers[club.path][offset + 32]
            report.append(dict(tm_club_id=ident, swos_club=db.club_fields(club)['Team name'],
                               club_uuid=club.uid, swos_player=player, player_uuid=uid,
                               old_band=old, old_gbp=BANDS[old] if old < len(BANDS) else '',
                               status='no_source_match'))
    write_csv(args.report, REPORT, report)
    print(', '.join(f'{key}: {n}' for key, n in sorted(Counter(r['status'] for r in report).items())))
    proposed = [item for item in changes if item[-1]['status'] == 'change']
    if not args.apply or not proposed:
        print('Database unchanged.')
        return

    for db, club, offset, new, _ in proposed:
        fields = db.player_fields(club, offset)
        fields['Player value'] = VALUES[new]
        db.edit_player(club, offset, fields)
    print(f'Updated {len(proposed)} valuations. Backup: {proposed[0][0].save()}')


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    modes = cli.add_subparsers(dest='mode', required=True)
    discover = modes.add_parser('discover', help='List club links found in a saved league page')
    discover.add_argument('html', type=Path)
    discover.add_argument('output', type=Path)
    clubs = modes.add_parser('clubs', help='Export Italian SWOS clubs for the club crosswalk')
    clubs.add_argument('--install', type=Path, default=Path.cwd())
    clubs.add_argument('output', type=Path)
    pages = modes.add_parser('extract', help='Read local squad HTML or the tmfetch.py cache')
    pages.add_argument('pages', type=Path)
    pages.add_argument('output', type=Path)
    pages.add_argument('--cache', type=Path)
    plan = modes.add_parser('import', help='Report proposed changes; add --apply to write')
    plan.add_argument('--install', type=Path, default=Path.cwd())
    plan.add_argument('--input', required=True, type=Path)
    plan.add_argument('--clubs', required=True, type=Path)
    plan.add_argument('--gbp-per-eur', required=True)
    plan.add_argument('--fx-date', required=True)
    plan.add_argument('--report', required=True, type=Path)
    plan.add_argument('--apply', action='store_true')
    args = cli.parse_args()
    try:
        if args.mode == 'clubs':
            list_clubs(args)
        elif args.mode == 'discover':
            links = read_page(args.html).links
            if not links:
                raise ValueError('No club links found in the saved league page.')
            write_csv(args.output, ('tm_club_id', 'squad_path', 'html_file'), (
                {'tm_club_id': club, 'squad_path': f'/{slug}/kader/verein/{club}/saison_id/2026/plus/1',
                 'html_file': ''} for club, slug in links.items()
            ))
        elif args.mode == 'extract':
            extract(args)
        else:
            preview(args)
    except (OSError, ValueError, ArithmeticError) as exc:
        cli.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
