#!/usr/bin/env python3
"""Rebuild tests/rules/specs from the Mod's packs: each World 2026-27 league's knockouts as the
Mod's own tools build them (mkseason.py's Pack.gplay: gplay.py compile_spec, a 'club' entrant
by its team word), and shapes.txt (each division's size). Bytes of the format only -- entrant
places, rounds, legs, team words --, no names; and each league with parallel groups' table
(pgroups.table_bytes: levels, sizes, each club's global number, division and area bytes).

    python3 -I tests/rules/specs/make_specs.py MOD PACKS [OUT]

MOD: a swos-9697-mod checkout (its tools/gplay.py); PACKS: the folder of <code>/pack.json (the
Mod's world5tm); OUT: default tests/rules/specs. The packs are not in this repository."""
import glob
import json
import os
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
mod, packs = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(mod, 'tools'))
import mkseason  # noqa: E402  (the Mod's)

shapes = []
for f in sorted(glob.glob(os.path.join(packs, '*', 'pack.json'))):
    code = os.path.basename(os.path.dirname(f))
    for c in json.load(open(f, encoding='utf-8'))['competitions']:
        if c.get('key') != 'league' or not c.get('ko_playoffs'):
            continue
        nd = len(c.get('divisions') or [1])
        sizes = [d['teams'] for d in c.get('divisions') or []] or [len(c.get('clubs') or []) or 16]
        built = mkseason.Pack(os.path.dirname(f)).gplay()
        if not built:
            sys.exit('%s: the Mod builds no knockouts for it' % code)
        open(os.path.join(out, code + '.spec'), 'wb').write(bytes(built[2]))
        shapes.append('%s %s\n' % (code, ','.join(map(str, sizes))))
open(os.path.join(out, 'shapes.txt'), 'w').write(''.join(shapes))
print('%d leagues -> %s' % (len(shapes), out))

# The leagues with parallel groups: each one's table as the Mod's pgroups.table_bytes writes it
# (Pack.parallel: levels, sizes, every club's global number, file division and area bytes) and its
# divisions' promoted / relegated counts (<code>.pg, <code>.pr; tests/rules/groups_*.c).
import pgroups  # noqa: E402  (the Mod's)
groups = []
for f in sorted(glob.glob(os.path.join(packs, '*', 'pack.json'))):
    pk = mkseason.Pack(os.path.dirname(f))
    t = pk.parallel()
    if not t:
        continue
    code = os.path.basename(os.path.dirname(f))
    open(os.path.join(out, code + '.pg'), 'wb').write(pgroups.table_bytes([t]))
    divs = pk.divisions()
    open(os.path.join(out, code + '.pr'), 'w').write(' '.join(str(d.get('promoted', 0)) for d in divs) + '\n'
                                                     + ' '.join(str(d.get('relegated', 0)) for d in divs) + '\n')
    groups.append(code + '\n')
open(os.path.join(out, 'groups.txt'), 'w').write(''.join(groups))
print('%d leagues with groups -> %s' % (len(groups), out))
