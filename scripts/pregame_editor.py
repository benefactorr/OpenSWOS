#!/usr/bin/env python3
"""Edit both starting databases in SWOS Career+ Unified."""
import csv
import io
import json
import os
import tempfile
import tkinter as tk
import unicodedata
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from math import floor
from tkinter import filedialog, messagebox, ttk

from reorder_modern_lineups import ID_SIZE, RECORD_SIZE, starting_squads
from swos_team_file import POSITIONS, encode_team, parse_team, parse_player, rnc_unpack, rnc_pack

SKILLS = {
    'Passing': (28, 0), 'Shot power': (29, 4), 'Heading': (29, 0),
    'Tackling': (30, 4), 'Ball control': (30, 0), 'Speed': (31, 4), 'Finishing': (31, 0),
}
FACE = ('Light skin / dark hair', 'Light skin / fair hair', 'Dark skin / dark hair', 'Dark skin / blond hair')
KITS = {
    'Home shirt pattern': (26, 3, 'primary_shirt_type'),
    'Home shirt colour': (28, 10, 'primary_basic_color'),
    'Home stripe colour': (27, 10, 'primary_stripes_color'),
    'Home shorts colour': (29, 10, 'primary_shorts_color'),
    'Home socks colour': (30, 10, 'primary_socks_color'),
    'Away shirt pattern': (31, 3, 'secondary_shirt_type'),
    'Away shirt colour': (33, 10, 'secondary_basic_color'),
    'Away stripe colour': (32, 10, 'secondary_stripes_color'),
    'Away shorts colour': (34, 10, 'secondary_shorts_color'),
    'Away socks colour': (35, 10, 'secondary_socks_color'),
}

# _avgTeamValues[:50] and _avgTeamValueBases from the game, in thousands.
BANDS = (5, 5, 6, 8, 10, 13, 15, 17, 20, 21, 23, 25, 26, 28, 30, 32, 34, 35, 37, 38,
         39, 40, 41, 42, 43, 44, 45, 47, 48, 49, 51, 53, 54, 56, 57, 58, 59, 61, 62, 63,
         65, 66, 67, 68, 69, 70, 71, 73, 76, 76)
BASE_VALUES = (*range(0, 100, 5), *range(100, 210, 10), 225, 250, 275, 300,
               *range(350, 1000, 50), *range(1000, 2000, 100), 2000, 2250, 2500, 2750,
               3000, 3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000,
               *range(10000, 31000, 1000))
def value_labels(scale):
    values = (tuple(floor(50 + 179950 * (index / 49) ** 4.5 + 0.5) for index in range(50))
              if scale == 7 else tuple(BASE_VALUES[band] for band in BANDS))
    return tuple(f'£{value:,}k  (index {i:02})' for i, value in enumerate(values))


VALUES = value_labels(1)
RELATIVE_ROSTER = Path('data_json/worlds/2627/modern_rosters.r02')


def read_name(data, start, size):
    return bytes(data[start:start + size]).split(b'\0', 1)[0].decode('latin1')


def pack_name(value, limit, label, allow_empty=False):
    value = value.strip()
    folded = unicodedata.normalize('NFKD', value.translate(str.maketrans(
        {'Đ': 'D', 'đ': 'd', 'Ł': 'L', 'ł': 'l', 'Ø': 'O', 'ø': 'o', 'ß': 'ss', '’': "'"})))
    name = ''.join(ch for ch in folded if not unicodedata.combining(ch)).upper()
    if (not name and not allow_empty) or any(not 32 <= ord(ch) <= 126 for ch in name):
        raise ValueError(f'{label} must use SWOS characters (letters, numbers or basic punctuation).')
    if len(name) > limit:
        raise ValueError(f'{label} allows up to {limit} characters.')
    return name.encode('ascii').ljust(limit + 1, b'\0')


def number(text, low, high, label):
    try:
        value = int(text)
    except ValueError:
        value = low - 1
    if not low <= value <= high:
        raise ValueError(f'{label} must be a whole number from {low} to {high}.')
    return value


@dataclass
class Club:
    uid: str
    offset: int
    player_offsets: list
    preferred: dict
    country: str
    path: Path
    json_index: int = -1


def existing(path):
    """Follow the game's case-insensitive lookup for relative asset names."""
    if path.is_file():
        return path
    if path == path.parent:
        return None
    folder = path.parent
    if not folder.is_dir():
        parts = list(path.parts)
        folder = Path(parts.pop(0))
        for part in parts[:-1]:
            if not folder.is_dir():
                return None
            folder = next((entry for entry in folder.iterdir() if entry.name.casefold() == part.casefold()), folder / part)
    if folder.is_dir():
        return next((entry for entry in folder.iterdir()
                     if entry.is_file() and entry.name.casefold() == path.name.casefold()), None)
    return None


class Database:
    def __init__(self, install, season, assets=None):
        self.install = install.resolve()
        self.path = self.install / 'bin'
        if season not in ('1996/97', '2026/27'):
            raise ValueError('Choose 1996/97 or 2026/27.')
        self.modern = season == '2026/27'
        self.season = season
        self.assets = assets or Path(os.environ.get('SWOS_ASSETS') or
            Path.home() / ('Documents/SWOS/assets' if os.name == 'nt' else '.swos/assets'))
        self.clubs = []
        self.originals = {}
        self.buffers = {}
        self.projections = {}
        self.json_sources = set()
        self.methods = {}
        self.changed_clubs = set()
        self.changed_players = set()
        default_age_path = self.path / 'data/player_ages_by_name.csv'
        self.age_path = existing(default_age_path) or default_age_path
        self.ages = {}
        self.age_dirty = False
        if self.modern:
            self.load_modern()
        else:
            self.load_classic()
        if not self.clubs:
            raise ValueError('No teams could be loaded. Select the Unified game folder and its SWOS assets folder.')

    def load_modern(self):
        path = self.path / RELATIVE_ROSTER
        raw = self.originals[path] = path.read_bytes()
        self.buffers[path] = bytearray(raw)
        for file in sorted((path.parent / 'teams').glob('*.json')):
            self.originals[file] = file.read_bytes()
            document = json.loads(self.originals[file])
            country = document.get('country_name') or file.stem.replace('_', ' ')
            for i, team in enumerate(document.get('teams', [])):
                uid = team.get('uuid')
                if uid:
                    if uid in self.projections:
                        raise ValueError(f'Duplicate team UUID in {file.name}.')
                    self.projections[uid] = file, i, country
        offset = 12
        for uid, _, block, records, preferred in starting_squads(raw):
            first = offset + len(block) - len(records) * RECORD_SIZE
            slots = [first + i * RECORD_SIZE + ID_SIZE for i in range(len(records))]
            lineup = ({preferred[i:i + ID_SIZE]: (i - 1) // ID_SIZE + 1
                       for i in range(1, len(preferred), ID_SIZE)} if preferred[:1] == b'\1' else {})
            country = self.projections.get(uid, (None, None, str(raw[offset + ID_SIZE])))[2]
            self.clubs.append(Club(uid, offset + ID_SIZE, slots, lineup, country, path))
            offset += len(block) + len(preferred)


    def load_classic(self):
        index_file = next((file for base in (self.assets, self.path)
                           if (file := existing(base / 'data_json/competitions/countries.json'))), None)
        if index_file is None:
            raise ValueError('Select the Unified game folder containing bin/data_json/competitions/countries.json.')
        entries = {item['number']: item for item in json.loads(index_file.read_bytes())['countries']}
        names = {9: 'Bosnia', 41: 'Yugoslavia', 47: 'Macedonia', 80: 'Europe national teams',
                 81: 'Africa national teams', 82: 'South America national teams', 83: 'North America national teams',
                 84: 'Asia national teams', 85: 'Oceania national teams'}
        overrides = {9, 41, 47}
        for country in sorted(set(entries) | set(names)):
            if country == 72:
                continue
            entry = entries.get(country, {})
            label = names.get(country, entry.get('name', str(country)))
            relative = entry.get('team_file')
            binary = Path(f'data/team.{country:03}')
            roots = (self.path, self.assets) if country in overrides else (self.assets, self.path)
            file = None
            if relative and country not in overrides:
                file = next((found for base in roots if (found := existing(base / 'data_json' / relative))), None)
            if file is None:
                file = next((found for base in roots if (found := existing(base / binary))), None)
            if file is None or file in self.buffers:
                continue
            original = self.originals[file] = file.read_bytes()
            if file.suffix.lower() == '.json':
                self.json_sources.add(file)
                document = json.loads(original)
                teams = document['teams']
                raw = len(teams).to_bytes(2, 'big') + b''.join(encode_team(team) for team in teams)
            else:
                method = self.methods[file] = original[3] if original[:3] == b'RNC' else 0
                raw = rnc_unpack(original) if method else original
            n = int.from_bytes(raw[:2], 'big')
            if not n or n > 256 or len(raw) != 2 + 684 * n:
                raise ValueError(f'Unexpected TEAM layout in {file}.')
            self.buffers[file] = bytearray(raw)
            for i in range(n):
                off = 2 + i * 684
                slots = [off + 76 + j * 38 for j in range(16)]
                self.clubs.append(Club(f'{country}:{i}', off, slots, {}, label, file,
                                       i if file in self.json_sources else -1))
        if self.age_path.is_file():
            raw = self.originals[self.age_path] = self.age_path.read_bytes()
            for row in csv.reader(io.StringIO(raw.decode('utf-8-sig'))):
                if len(row) == 2 and row[1].strip().isdigit():
                    self.ages[row[0]] = row[1].strip()
        else:
            self.originals[self.age_path] = None

    def club_fields(self, club):
        buf, off = self.buffers[club.path], club.offset
        fields = {'Team name': read_name(buf, off + 5, 17), 'Coach name': read_name(buf, off + 36, 23)}
        fields.update({label: str(buf[off + byte]) for label, (byte, _, _) in KITS.items()})
        return fields

    def edit_club(self, club, fields):
        previous = self.club_fields(club)
        if previous == fields:
            return
        if self.modern and club.uid not in self.projections:
            raise ValueError('This club is missing from the modern teams folder.')
        buf = self.buffers[club.path]
        header = buf[club.offset:club.offset + 76]
        for label, off, limit in (('Team name', 5, 16), ('Coach name', 36, 22)):
            if fields[label] != previous[label]:
                header[off:off + limit + 1] = pack_name(fields[label], limit, label, label == 'Coach name')
        for label, (byte, high, _) in KITS.items():
            if fields[label] != previous[label]:
                header[byte] = number(fields[label], 0, high, label)
        buf[club.offset:club.offset + 76] = header
        self.changed_clubs.add(club.uid)

    def lineup_slot(self, club, off):
        buf = self.buffers[club.path]
        if self.modern:
            return club.preferred.get(bytes(buf[off - ID_SIZE:off]), 99)
        order = list(buf[club.offset + 60:club.offset + 76])
        slot = (off - club.offset - 76) // 38
        return order.index(slot) + 1 if slot in order else 99

    def player_fields(self, club, off):
        buf = self.buffers[club.path]
        record = buf[off:off + 38]
        player = parse_player(record)
        fields = {
            'Player name': player['player_name'],
            'Position': POSITIONS[player['position']],
            'Player value': VALUES[player['price']] if 0 <= player['price'] < len(VALUES) else '',
            'Shirt number': str(record[2]),
            'Nationality code': str(record[0]),
            'Skin and hair': FACE[player['face']],
            'Age': str(buf[off + 38]) if self.modern else self.ages.get(self.age_key(player['player_name']), ''),
        }
        fields.update({label: str(record[byte] >> shift & 7)
                       for label, (byte, shift) in SKILLS.items()})
        return fields

    @staticmethod
    def age_key(name):
        return name[:22].rstrip(' ').upper()

    def edit_player(self, club, off, fields):
        previous = self.player_fields(club, off)
        if fields == previous:
            return
        buf = self.buffers[club.path]
        record = buf[off:off + 38]
        if fields['Player name'] != previous['Player name']:
            record[3:26] = pack_name(fields['Player name'], 22, 'Player name')
        position = tuple(POSITIONS.values()).index(fields['Position'])
        if position != 0 and previous['Position'] == 'GK':
            if self.modern and self.lineup_slot(club, off) == 1:
                raise ValueError('This player is the saved starting goalkeeper. Keep the GK position for this slot.')
            if not any(other != off and buf[other + 26] >> 5 == 0 and buf[other + 3] not in (0, 255)
                       for other in club.player_offsets):
                raise ValueError('The team needs at least one goalkeeper.')
        record[26] = record[26] & 7 | position << 5 | FACE.index(fields['Skin and hair']) << 3
        if fields['Player value'] != previous['Player value']:
            record[32] = VALUES.index(fields['Player value'])
        if fields['Shirt number'] != previous['Shirt number']:
            record[2] = number(fields['Shirt number'], 1, 99, 'Shirt number')
        if fields['Nationality code'] != previous['Nationality code']:
            record[0] = number(fields['Nationality code'], 0, 213, 'Nationality code')
        age = fields['Age'].strip()
        if self.modern or age:
            parsed_age = number(age, 14 if self.modern else 15, 60, 'Age')
        for label, (byte, shift) in SKILLS.items():
            if fields[label] != previous[label]:
                skill = number(fields[label], 0, 7, label)
                record[byte] = record[byte] & ~(7 << shift) | skill << shift
        if self.modern:
            buf[off + 38] = parsed_age
        elif age != previous['Age'] or fields['Player name'] != previous['Player name']:
            key = self.age_key(read_name(record, 3, 23))
            if age:
                self.ages[key] = str(parsed_age)
            elif age != previous['Age']:
                self.ages.pop(key, None)
            self.age_dirty = True
        if record != buf[off:off + 38]:
            buf[off:off + 38] = record
            buf[club.offset + 59] = 1
            self.changed_players.add((club.uid, off))

    def dirty(self):
        if self.changed_clubs or self.changed_players or self.age_dirty:
            return True
        if self.modern:
            path = self.path / RELATIVE_ROSTER
            return self.buffers[path] != self.originals[path]
        return False

    @staticmethod
    def update_player_projection(player, record):
        values = parse_player(record)
        keys = ('player_name', 'nationality', 'shirt_number', 'position', 'face', 'passing',
                'velocity', 'heading', 'tackling', 'control', 'speed', 'finishing', 'price')
        player.update({key: values[key] for key in keys})
        if 'position_and_face' in player:
            player['position_and_face'] = record[26]
        for key, byte in (('velocity_heading', 29), ('tackling_control', 30), ('speed_finishing', 31)):
            if key in player:
                player[key] = record[byte]

    def save(self):
        if not self.dirty():
            return None
        updates = {}
        documents = {}
        edited = {}
        for uid, offset in self.changed_players:
            edited.setdefault(uid, []).append(offset)
        if self.modern:
            path = self.path / RELATIVE_ROSTER
            updates[path] = bytes(self.buffers[path])
        for club in self.clubs:
            if club.uid not in self.changed_clubs and club.uid not in edited:
                continue
            buf = self.buffers[club.path]
            if self.modern:
                if club.uid not in self.projections:
                    raise ValueError(f'Missing team projection for {self.club_fields(club)["Team name"]}.')
                file, index, _ = self.projections[club.uid]
            elif club.json_index >= 0:
                file, index = club.path, club.json_index
            else:
                if club.path not in updates:
                    raw = bytes(buf)
                    method = self.methods[club.path]
                    updates[club.path] = rnc_pack(raw, method) if method else raw
                    if method and rnc_unpack(updates[club.path]) != raw:
                        raise ValueError(f'RNC write verification failed for {club.path.name}.')
                continue
            if file not in documents:
                documents[file] = json.loads(self.originals[file])
            team = documents[file]['teams'][index]
            head = team['team']
            if club.uid in self.changed_clubs:
                fields = self.club_fields(club)
                head.update(team_name=fields['Team name'], coach_name=fields['Coach name'])
                for label, (_, _, key) in KITS.items():
                    head[key] = int(fields[label])
                if 'kit_colors' in head:
                    head['kit_colors'] = list(buf[club.offset + 26:club.offset + 36])
            if club.uid in edited:
                head['player_values_dirty'] = 1
                by_id = {p.get('uuid'): p for p in team['players']} if self.modern else None
                for off in edited[club.uid]:
                    if self.modern:
                        uid = read_name(buf, off - ID_SIZE, ID_SIZE).lower()
                        player = by_id.get(uid)
                    else:
                        player = team['players'][(off - club.offset - 76) // 38]
                    if player is not None:
                        self.update_player_projection(player, buf[off:off + 38])
        for file, data in documents.items():
            updates[file] = (json.dumps(data, ensure_ascii=True, indent=2) + '\n').encode()
        if self.age_dirty:
            stream = io.StringIO(newline='')
            writer = csv.writer(stream)
            writer.writerow(('name', 'age'))
            writer.writerows(sorted(self.ages.items()))
            updates[self.age_path] = stream.getvalue().encode()
        updates = {file: raw for file, raw in updates.items() if raw != self.originals[file]}
        if not updates:
            self.changed_clubs.clear()
            self.changed_players.clear()
            self.age_dirty = False
            return None
        originals = {file: self.originals[file] for file in updates}
        for file, raw in originals.items():
            if (file.read_bytes() if file.exists() else None) != raw:
                raise ValueError(f'{file} changed on disk. Reopen the editor before saving.')
        backup = self.install / 'editor_backups' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        for i, (file, raw) in enumerate(originals.items()):
            if raw is not None:
                copy = backup / f'{i:03}' / file.name
                copy.parent.mkdir(parents=True, exist_ok=True)
                copy.write_bytes(raw)
        backup.mkdir(parents=True, exist_ok=True)
        (backup / 'restore_paths.json').write_text(json.dumps([
            {'file': str(file), 'backup': f'{i:03}/{file.name}' if raw is not None else None}
            for i, (file, raw) in enumerate(originals.items())
        ], indent=2), encoding='utf-8')
        written = []
        try:
            for file, raw in updates.items():
                file.parent.mkdir(parents=True, exist_ok=True)
                atomic_write(file, raw)
                written.append(file)
        except OSError as exc:
            try:
                for file in reversed(written):
                    if originals[file] is None:
                        file.unlink()
                    else:
                        atomic_write(file, originals[file])
            except OSError as rollback:
                raise OSError(f'Save and rollback failed. Originals are in {backup}.') from rollback
            raise exc
        self.originals.update(updates)
        self.changed_clubs.clear()
        self.changed_players.clear()
        self.age_dirty = False
        return backup


def atomic_write(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temp = Path(stream.name)
    try:
        temp.write_bytes(data)
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


class Editor:
    def __init__(self, root):
        self.root = root
        self.db = None
        self.club = self.player = None
        root.title('SWOS Career+ | Pre-game Editor')
        root.geometry('1220x750')
        root.minsize(970, 630)
        outer = ttk.Frame(root, padding=16)
        outer.pack(fill='both', expand=True)
        toolbar = ttk.Frame(outer)
        toolbar.pack(fill='x', pady=(0, 14))
        ttk.Button(toolbar, text='Choose database…', command=self.open).pack(side='left')
        self.save_button = ttk.Button(toolbar, text='Save database', command=self.save, state='disabled')
        self.save_button.pack(side='left', padx=8)
        self.status = tk.StringVar(value='Choose a starting season to begin.')
        ttk.Label(outer, textvariable=self.status, wraplength=900).pack(side='bottom', anchor='w', pady=(12, 0))
        columns = ttk.Panedwindow(outer, orient='horizontal')
        columns.pack(fill='both', expand=True)
        clubs = ttk.Frame(columns, padding=(0, 0, 12, 0))
        squad = ttk.Frame(columns, padding=(8, 0, 12, 0))
        edit = ttk.Frame(columns, padding=(8, 0, 0, 0))
        for pane, weight in ((clubs, 1), (squad, 2), (edit, 1)):
            columns.add(pane, weight=weight)
        self.club_query = self.search_field(clubs, 'Find club or country', self.refresh_clubs)
        self.club_list = self.tree(clubs, ('club', 'country'), (145, 110), ('Club', 'Country / pool'))
        self.club_list.bind('<<TreeviewSelect>>', self.choose_club)
        self.player_query = self.search_field(squad, 'Find player in squad', self.refresh_players)
        self.player_list = self.tree(squad, ('slot', 'name', 'role', 'value'), (48, 175, 45, 78),
                                     ('XI', 'Player', 'Pos', 'Value'))
        self.player_list.bind('<<TreeviewSelect>>', self.choose_player)
        tabs = ttk.Notebook(edit)
        tabs.pack(fill='both', expand=True)
        player_box = ttk.Frame(tabs, padding=12)
        club_box = ttk.Frame(tabs, padding=12)
        tabs.add(player_box, text='Player details')
        tabs.add(club_box, text='Team details')
        self.club_form = self.form(club_box, {'Team name': None, 'Coach name': None,
            **{label: tuple(map(str, range(high + 1))) for label, (_, high, _) in KITS.items()}})
        ttk.Label(club_box, text='Kit selections use the SWOS colour and pattern codes.',
                  wraplength=290).grid(columnspan=2, pady=8, sticky='w')
        fields = {'Player name': None, 'Position': tuple(POSITIONS.values()), 'Player value': VALUES,
                  'Shirt number': None, 'Nationality code': None, 'Skin and hair': FACE, 'Age': None,
                  **dict.fromkeys(SKILLS, tuple(map(str, range(8))))}
        self.player_form = self.form(player_box, fields)
        self.age_note = tk.StringVar()
        ttk.Label(player_box, textvariable=self.age_note, wraplength=290).grid(
            columnspan=2, pady=8, sticky='w')
        self.player_base = {}
        self.club_base = {}
        self.load_form(self.club_form, {})
        self.load_form(self.player_form, {})
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.bind('<Control-s>', lambda _: self.save() if self.db else None)

    @staticmethod
    def search_field(pane, label, callback):
        ttk.Label(pane, text=label).pack(anchor='w')
        var = tk.StringVar()
        ttk.Entry(pane, textvariable=var).pack(fill='x', pady=(4, 8))
        var.trace_add('write', lambda *_: callback())
        return var

    @staticmethod
    def tree(pane, columns, widths, titles):
        body = ttk.Frame(pane)
        body.pack(fill='both', expand=True)
        tree = ttk.Treeview(body, columns=columns, show='headings', selectmode='browse')
        bar = ttk.Scrollbar(body, command=tree.yview)
        tree.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        tree.pack(side='left', fill='both', expand=True)
        for column, width, title in zip(columns, widths, titles):
            tree.heading(column, text=title)
            tree.column(column, width=width, minwidth=width - 12, stretch=column in {'club', 'name', 'country'})
        return tree

    @staticmethod
    def form(pane, fields):
        form = {}
        pane.columnconfigure(1, weight=1)
        for i, (label, choices) in enumerate(fields.items()):
            ttk.Label(pane, text=label).grid(row=i, column=0, sticky='w', padx=(0, 8), pady=2)
            var = tk.StringVar()
            widget = (ttk.Combobox(pane, textvariable=var, values=choices, width=17)
                      if choices else ttk.Entry(pane, textvariable=var, width=19))
            widget.grid(row=i, column=1, sticky='ew', pady=2)
            form[label] = (var, widget, 'readonly' if choices else 'normal')
        return form

    @staticmethod
    def load_form(form, values):
        for label, (var, widget, state) in form.items():
            var.set(values.get(label, ''))
            widget.configure(state=state if values else 'disabled')

    @staticmethod
    def values(form):
        return {key: row[0].get() for key, row in form.items()}

    def current_value_labels(self):
        return value_labels(7 if self.db.modern else 1)

    def player_form_fields(self, offset):
        fields = self.db.player_fields(self.club, offset)
        value = fields['Player value']
        if value in VALUES:
            fields['Player value'] = self.current_value_labels()[VALUES.index(value)]
        return fields

    def apply(self):
        if self.player is not None:
            current = self.values(self.player_form)
            if current != self.player_base:
                labels = self.current_value_labels()
                value = current['Player value']
                if value in labels:
                    current['Player value'] = VALUES[labels.index(value)]
                self.db.edit_player(self.club, self.player, current)
                self.player_base = self.player_form_fields(self.player)
                self.load_form(self.player_form, self.player_base)
        if self.club is not None:
            current = self.values(self.club_form)
            if current != self.club_base:
                self.db.edit_club(self.club, current)
                self.show_club_form()

    def check_apply(self):
        try:
            self.apply()
            return True
        except (ValueError, OSError) as exc:
            messagebox.showerror('Check the edit', str(exc), parent=self.root)
            return False

    def refresh_clubs(self):
        if self.db is None:
            return
        query = self.club_query.get().strip().casefold()
        self.club_list.delete(*self.club_list.get_children())
        for i, club in enumerate(self.db.clubs):
            name = self.db.club_fields(club)['Team name']
            if query in f'{name} {club.country}'.casefold():
                self.club_list.insert('', 'end', iid=str(i), values=(name, club.country))
                if club is self.club:
                    self.club_list.selection_set(str(i))

    def refresh_players(self):
        self.player_list.delete(*self.player_list.get_children())
        if self.club is None:
            return
        query = self.player_query.get().strip().casefold()
        def lineup_slot(off):
            return self.db.lineup_slot(self.club, off)
        for off in sorted(self.club.player_offsets, key=lineup_slot):
            fields = self.player_form_fields(off)
            if fields['Player name'] and not fields['Player name'].startswith('ÿ') and query in fields['Player name'].casefold():
                slot = lineup_slot(off)
                label = str(slot) if slot <= 11 else 'Bench' if slot <= 16 else ''
                self.player_list.insert('', 'end', iid=str(off), values=(
                    label, fields['Player name'], fields['Position'], fields['Player value'].split('  ')[0]))
                if off == self.player:
                    self.player_list.selection_set(str(off))

    def show_club_form(self):
        self.club_base = self.db.club_fields(self.club)
        self.load_form(self.club_form, self.club_base)

    def choose_club(self, _event):
        selected = self.club_list.selection()
        if not selected:
            return
        club = self.db.clubs[int(selected[0])]
        if club is self.club:
            return
        if not self.check_apply():
            self.refresh_clubs()
            return
        self.club, self.player = club, None
        self.show_club_form()
        self.load_form(self.player_form, {})
        self.player_query.set('')
        self.refresh_players()

    def choose_player(self, _event):
        selected = self.player_list.selection()
        if not selected:
            return
        offset = int(selected[0])
        if offset == self.player:
            return
        if not self.check_apply():
            self.refresh_players()
            return
        self.player = offset
        self.player_base = self.player_form_fields(offset)
        self.load_form(self.player_form, self.player_base)
        self.refresh_players()

    def confirm_leave(self):
        if self.db is None:
            return True
        pending = ((self.player is not None and self.values(self.player_form) != self.player_base) or
                   (self.club is not None and self.values(self.club_form) != self.club_base))
        if not pending and not self.db.dirty():
            return True
        answer = messagebox.askyesnocancel('Unsaved edits', 'Save your changes before continuing?', parent=self.root)
        return answer is False or (answer is True and self.save())

    def open(self):
        if not self.confirm_leave():
            return
        dialog = tk.Toplevel(self.root)
        dialog.title('Choose starting database')
        dialog.transient(self.root)
        dialog.resizable(False, False)
        pane = ttk.Frame(dialog, padding=20)
        pane.pack(fill='both', expand=True)
        season = tk.StringVar(value=self.db.season if self.db else '1996/97')
        default = Path(__file__).resolve().parents[1]
        install = tk.StringVar(value=str(self.db.install if self.db else default))
        assets = tk.StringVar(value=str(self.db.assets) if self.db else
            os.environ.get('SWOS_ASSETS') or str(Path.home() / (
                'Documents/SWOS/assets' if os.name == 'nt' else '.swos/assets')))
        ttk.Label(pane, text='Starting season').grid(row=0, column=0, sticky='w', pady=6)
        ttk.Combobox(pane, state='readonly', textvariable=season,
                     values=('1996/97', '2026/27')).grid(row=0, column=1, sticky='w', padx=8)
        for row, (label, variable) in enumerate((('Unified game folder', install), ('SWOS assets folder', assets)), 1):
            ttk.Label(pane, text=label).grid(row=row, column=0, sticky='w', pady=6)
            ttk.Entry(pane, textvariable=variable, width=53).grid(row=row, column=1, padx=8)
            def browse(var=variable):
                selected = filedialog.askdirectory(parent=dialog, initialdir=var.get())
                if selected:
                    var.set(selected)
            ttk.Button(pane, text='Browse…', command=browse).grid(row=row, column=2)
        def load():
            try:
                assets_path = Path(assets.get()).expanduser().resolve() if assets.get().strip() else None
                db = Database(Path(install.get()).expanduser(), season.get(), assets_path)
            except (ValueError, OSError, RuntimeError, ImportError, KeyError, IndexError) as exc:
                messagebox.showerror('Could not open database', str(exc), parent=dialog)
                return
            dialog.destroy()
            self.load_database(db)
        ttk.Button(pane, text='Cancel', command=dialog.destroy).grid(row=3, column=1, sticky='e', padx=8, pady=(10, 0))
        ttk.Button(pane, text='Open', command=load).grid(row=3, column=2, pady=(10, 0))
        dialog.grab_set()
        dialog.bind('<Escape>', lambda _: dialog.destroy())
        dialog.bind('<Return>', lambda _: load())

    def load_database(self, db):
        self.db, self.club, self.player = db, None, None
        self.player_form['Player value'][1].configure(values=self.current_value_labels())
        self.age_note.set('Age is stored per player.' if db.modern else
            'Historical ages are shared by exact player name. An empty age has no CSV override.')
        self.load_form(self.club_form, {})
        self.load_form(self.player_form, {})
        self.club_query.set('')
        self.player_query.set('')
        self.refresh_clubs()
        self.refresh_players()
        self.save_button.configure(state='normal')
        self.root.title(f'SWOS Career+ | {db.season} Pre-game Editor')
        self.status.set(f'{db.season}  •  {len(db.clubs):,} teams  •  {db.install}')
        first = self.club_list.get_children()[0]
        self.club_list.selection_set(first)
        self.club_list.focus(first)

    def save(self):
        if not self.check_apply():
            return False
        try:
            backup = self.db.save()
        except (ValueError, OSError, RuntimeError) as exc:
            messagebox.showerror('Could not save', str(exc), parent=self.root)
            return False
        self.refresh_clubs()
        self.refresh_players()
        self.status.set(f'Saved. Backup: {backup}' if backup else f'No changes to save.  •  {self.db.path}')
        return True

    def close(self):
        if self.confirm_leave():
            self.root.destroy()


def main():
    root = tk.Tk()
    editor = Editor(root)
    root.after_idle(editor.open)
    root.mainloop()


if __name__ == '__main__':
    main()
