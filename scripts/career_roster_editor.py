#!/usr/bin/env python3
"""Edit the SWOS 2026/27 Modernised Career+ starting database."""
import json
import os
import sys
import tempfile
import tkinter as tk
import unicodedata
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from reorder_modern_lineups import ID_SIZE, RECORD_SIZE, starting_squads
from swos_team_file import POSITIONS, parse_player

SKILLS = {
    'Passing': (28, 0), 'Shot power': (29, 4), 'Heading': (29, 0),
    'Tackling': (30, 4), 'Ball control': (30, 0), 'Speed': (31, 4), 'Finishing': (31, 0),
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
    return tuple(f'£{BASE_VALUES[band] * scale:,}k  (index {i:02})'
                 for i, band in enumerate(BANDS))


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
    country: str = ''


class Database:
    def __init__(self, path):
        self.path = path.resolve()
        self.original = self.path.read_bytes()
        self.data = bytearray(self.original)
        self.clubs = []
        self.json_files = {}
        self.team_entries = {}
        self.club_edits = {}
        offset = 12
        for uid, _, block, records, preferred in starting_squads(self.original):
            first = offset + len(block) - len(records) * RECORD_SIZE
            slots = [first + i * RECORD_SIZE for i in range(len(records))]
            lineup = ({preferred[i:i + ID_SIZE]: (i - 1) // ID_SIZE + 1
                       for i in range(1, len(preferred), ID_SIZE)} if preferred[:1] == b'\1' else {})
            self.clubs.append(Club(uid, offset + ID_SIZE, slots, lineup))
            offset += len(block) + len(preferred)
        for json_path in sorted((self.path.parent / 'teams').glob('*.json')):
            raw = json_path.read_bytes()
            document = json.loads(raw)
            self.json_files[json_path] = raw
            country = document.get('country_name') or json_path.stem.replace('_', ' ')
            for team in document.get('teams', []):
                uid = team.get('uuid')
                if uid in self.team_entries:
                    raise ValueError(f'Duplicate club ID in team files: {uid}')
                self.team_entries[uid] = (json_path, country)
        for club in self.clubs:
            club.country = self.team_entries.get(club.uid, (None, str(self.data[club.offset])))[1]

    def club_fields(self, club):
        return read_name(self.data, club.offset + 5, 17), read_name(self.data, club.offset + 36, 23)

    def edit_club(self, club, name, coach):
        previous = self.club_fields(club)
        name_bytes = pack_name(name, 16, 'Club name') if name != previous[0] else None
        coach_bytes = pack_name(coach, 22, 'Coach name', True) if coach != previous[1] else None
        if name_bytes is None and coach_bytes is None:
            return
        if club.uid not in self.team_entries:
            raise ValueError('Club edits require the matching teams folder beside the roster file.')
        if name_bytes is not None:
            self.data[club.offset + 5:club.offset + 22] = name_bytes
        if coach_bytes is not None:
            self.data[club.offset + 36:club.offset + 59] = coach_bytes
        self.club_edits[club.uid] = self.club_fields(club)

    def player_fields(self, offset):
        record = self.data[offset + ID_SIZE:offset + ID_SIZE + 38]
        player = parse_player(record)
        fields = {
            'Player name': player['player_name'],
            'Position': POSITIONS[player['position']],
            'Base value': VALUES[player['price']] if 0 <= player['price'] < len(VALUES) else '',
            'Shirt number': str(player['shirt_number']),
            'Age': str(self.data[offset + RECORD_SIZE - 2]),
        }
        fields.update({label: str(record[byte] >> shift & 7)
                       for label, (byte, shift) in SKILLS.items()})
        return fields

    def edit_player(self, club, offset, fields):
        previous = self.player_fields(offset)
        if fields == previous:
            return
        player_start = offset + ID_SIZE
        record = self.data[player_start:offset + RECORD_SIZE]
        if fields['Player name'] != previous['Player name']:
            record[3:26] = pack_name(fields['Player name'], 22, 'Player name')
        position = tuple(POSITIONS.values()).index(fields['Position'])
        if position != 0 and previous['Position'] == 'GK':
            if club.preferred.get(bytes(self.data[offset:player_start])) == 1:
                raise ValueError('This player is the saved starting goalkeeper. Keep the GK position for this slot.')
            if not any(other != offset and self.data[other + ID_SIZE + 26] >> 5 == 0
                       for other in club.player_offsets):
                raise ValueError('The club needs at least one goalkeeper.')
        record[26] = record[26] & 31 | position << 5
        if fields['Base value'] != previous['Base value']:
            record[32] = VALUES.index(fields['Base value'])
        if fields['Shirt number'] != previous['Shirt number']:
            record[2] = number(fields['Shirt number'], 1, 99, 'Shirt number')
        record[38] = number(fields['Age'], 14, 60, 'Age')
        for label, (byte, shift) in SKILLS.items():
            skill = number(fields[label], 0, 7, label)
            record[byte] = record[byte] & ~(7 << shift) | skill << shift
        self.data[player_start:offset + RECORD_SIZE] = record

    def save(self):
        if self.data == self.original and not self.club_edits:
            return None
        originals = {self.path: self.original}
        updates = {self.path: bytes(self.data)}
        changed_files = {}
        for uid, (name, coach) in self.club_edits.items():
            path, _ = self.team_entries[uid]
            if path not in changed_files:
                originals[path] = self.json_files[path]
                changed_files[path] = json.loads(originals[path])
            team = next(item['team'] for item in changed_files[path]['teams'] if item.get('uuid') == uid)
            team.update(team_name=name, coach_name=coach)
        for path, document in changed_files.items():
            updates[path] = (json.dumps(document, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
        for path, raw in originals.items():
            if path.read_bytes() != raw:
                raise ValueError(f'{path.name} changed on disk. Reopen the database before saving.')
        backup = self.path.parent / 'editor_backups' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        for path, raw in originals.items():
            copy = backup / path.relative_to(self.path.parent)
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(raw)
        written = []
        try:
            for path, raw in updates.items():
                atomic_write(path, raw)
                written.append(path)
        except OSError:
            for path in reversed(written):
                atomic_write(path, originals[path])
            raise
        self.original = updates[self.path]
        self.json_files.update({path: updates[path] for path in changed_files})
        self.club_edits.clear()
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
        self.current_clubs = []
        root.title('SWOS 2026/27 Career+ Editor')
        root.geometry('1140x710')
        root.minsize(970, 630)
        outer = ttk.Frame(root, padding=16)
        outer.pack(fill='both', expand=True)
        toolbar = ttk.Frame(outer)
        toolbar.pack(fill='x', pady=(0, 14))
        ttk.Button(toolbar, text='Open roster…', command=self.open).pack(side='left')
        self.save_button = ttk.Button(toolbar, text='Save database', command=self.save, state='disabled')
        self.save_button.pack(side='left', padx=8)
        self.modern_values = tk.BooleanVar(value=True)
        ttk.Checkbutton(toolbar, text='Modern values (×7)', variable=self.modern_values,
                        command=self.change_value_scale).pack(side='left', padx=8)
        self.status = tk.StringVar(value='Open your 2026/27 modern_rosters.r02 file.')
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
        club_box = ttk.LabelFrame(edit, text='Club', padding=9)
        club_box.pack(fill='x', pady=(0, 10))
        self.club_form = self.form(club_box, {'Club name': None, 'Coach name': None})
        player_box = ttk.LabelFrame(edit, text='Player', padding=9)
        player_box.pack(fill='x')
        fields = {'Player name': None, 'Position': tuple(POSITIONS.values()), 'Base value': value_labels(7),
                  'Shirt number': None, 'Age': None, **dict.fromkeys(SKILLS, tuple(map(str, range(8))))}
        self.player_form = self.form(player_box, fields)
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
        return value_labels(7 if self.modern_values.get() else 1)

    def display_value(self, value):
        return self.current_value_labels()[VALUES.index(value)] if value in VALUES else value

    def player_form_fields(self, offset):
        fields = self.db.player_fields(offset)
        fields['Base value'] = self.display_value(fields['Base value'])
        return fields

    def change_value_scale(self):
        # Preserve the pending selection by index when switching its display units.
        var, widget, _ = self.player_form['Base value']
        previous = tuple(widget.cget('values'))
        current = var.get()
        labels = self.current_value_labels()
        widget.configure(values=labels)
        if current in previous:
            var.set(labels[previous.index(current)])
        if self.player is not None:
            self.player_base = self.player_form_fields(self.player)
        self.refresh_players()

    def apply(self):
        if self.player is not None:
            current = self.values(self.player_form)
            if current != self.player_base:
                value = current['Base value']
                labels = self.current_value_labels()
                if value in labels:
                    current['Base value'] = VALUES[labels.index(value)]
                self.db.edit_player(self.club, self.player, current)
                self.player_base = self.player_form_fields(self.player)
                self.load_form(self.player_form, self.player_base)
        if self.club is not None:
            current = self.values(self.club_form)
            if current != self.club_base:
                self.db.edit_club(self.club, current['Club name'], current['Coach name'])
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
            name, _ = self.db.club_fields(club)
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
            return self.club.preferred.get(bytes(self.db.data[off:off + ID_SIZE]), 99)
        for off in sorted(self.club.player_offsets, key=lineup_slot):
            fields = self.player_form_fields(off)
            if query in fields['Player name'].casefold():
                slot = lineup_slot(off)
                label = str(slot) if slot <= 11 else 'Bench' if slot <= 16 else ''
                self.player_list.insert('', 'end', iid=str(off), values=(
                    label, fields['Player name'], fields['Position'], fields['Base value'].split('  ')[0]))
                if off == self.player:
                    self.player_list.selection_set(str(off))

    def show_club_form(self):
        self.club_base = dict(zip(('Club name', 'Coach name'), self.db.club_fields(self.club)))
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
        if not pending and self.db.data == self.db.original and not self.db.club_edits:
            return True
        answer = messagebox.askyesnocancel('Unsaved edits', 'Save your changes before continuing?', parent=self.root)
        return answer is False or (answer is True and self.save())

    def open(self, path=None):
        if not self.confirm_leave():
            return
        if path is None:
            path = filedialog.askopenfilename(parent=self.root, title='Open the installed Career+ roster',
                                              filetypes=[('Career+ roster', '*.r02'), ('All files', '*')])
        if not path:
            return
        try:
            db = Database(Path(path))
        except (ValueError, OSError) as exc:
            messagebox.showerror('Could not open roster', str(exc), parent=self.root)
            return
        self.db, self.club, self.player = db, None, None
        self.load_form(self.club_form, {})
        self.load_form(self.player_form, {})
        self.club_query.set('')
        self.player_query.set('')
        self.refresh_players()
        self.save_button.configure(state='normal')
        self.status.set(f'{len(db.clubs):,} clubs  •  {db.path}')
        first = self.club_list.get_children()[0]
        self.club_list.selection_set(first)
        self.club_list.focus(first)

    def save(self):
        if not self.check_apply():
            return False
        try:
            backup = self.db.save()
        except (ValueError, OSError) as exc:
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
    if len(sys.argv) > 1:
        path = Path(sys.argv[1])
    else:
        assets = os.environ.get('SWOS_ASSETS')
        user = Path.home() / ('Documents/SWOS' if os.name == 'nt' else '.swos')
        install = Path(__file__).resolve().parents[1]
        roots = (Path(assets) if assets else user / 'assets', install / 'bin', install)
        path = next((base / RELATIVE_ROSTER for base in roots if (base / RELATIVE_ROSTER).is_file()), None)
    if path is not None:
        root.after_idle(lambda: editor.open(path))
    root.mainloop()


if __name__ == '__main__':
    main()
