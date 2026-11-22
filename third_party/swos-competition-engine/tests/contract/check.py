"""Contract scenario checker (no engine): every scenario is well-formed against its pack (clubs, competitions,
stages, ties exist; a round-robin script covers no pair twice; knockout results are consistent with the stage's
deciders), and its expected round-robin tables follow from the scripted results by the stage's tie-breakers
(points, goal difference, goals for, head-to-head points: the criteria the sample uses). The engines (OpenSWOS's,
later the library) run the same files through their own runner and must produce the expected observations.

    python3 tests/contract/check.py tests/contract/*/*.json
"""
import itertools
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fail(sc, msg):
    print(f'FAIL {sc}: {msg}')
    return 1


def table(stage, comp, res):
    clubs = comp['participants']['clubs']
    pts = comp.get('points', {'win': 3, 'draw': 1, 'loss': 0})
    script = {(m['home'], m['away']): m['score'] for m in res.get('matches', [])}
    games = []
    for h, a in itertools.permutations(clubs, 2):
        for leg in range(stage['legs'] // 2 + stage['legs'] % 2):
            games.append((h, a, script.get((h, a), res['default'])))
    row = {c: dict(played=0, won=0, drawn=0, lost=0, goals_for=0, goals_against=0, points=0) for c in clubs}
    for h, a, (gh, ga) in games:
        for c, f, g in ((h, gh, ga), (a, ga, gh)):
            r = row[c]
            r['played'] += 1; r['goals_for'] += f; r['goals_against'] += g
            k = 'won' if f > g else 'drawn' if f == g else 'lost'
            r[k] += 1
            r['points'] += pts['win' if k == 'won' else 'draw' if k == 'drawn' else 'loss']

    def h2h(group):
        p = {c: 0 for c in group}
        for h, a, (gh, ga) in games:
            if h in group and a in group:
                p[h] += pts['win'] if gh > ga else pts['draw'] if gh == ga else pts['loss']
                p[a] += pts['win'] if ga > gh else pts['draw'] if gh == ga else pts['loss']
        return p
    crit = stage['tiebreakers']
    assert crit[:3] == ['points', 'goal_difference', 'goals_for'], crit
    key = lambda c: (row[c]['points'], row[c]['goals_for'] - row[c]['goals_against'], row[c]['goals_for'])
    order = sorted(clubs, key=key, reverse=True)
    out = []
    for _, grp in itertools.groupby(order, key=key):
        grp = list(grp)
        if len(grp) > 1:
            assert 'head_to_head_points' in crit
            p = h2h(grp)
            grp.sort(key=lambda c: p[c], reverse=True)
            if len(set(p.values())) < len(grp):
                return None, row                    # (would need draw_lots: a scenario must not depend on it)
        out += grp
    return out, row


def check(path):
    sc = json.load(open(path))
    bad = 0
    if sc.get('contract') != 'swc-contract/1':
        return fail(path, 'contract must be "swc-contract/1"')
    world = json.load(open(os.path.join(ROOT, sc['pack'], 'world.json')))
    clubs = {c['id'] for c in world['clubs']}
    comps = {c['id']: c for c in world['competitions']}
    stages = {f"{c['id']}:{s['id']}": (s, c) for c in world['competitions'] for s in c['stages']}
    for sid, res in sc['results'].items():
        if sid not in stages:
            bad += fail(path, f'results: no stage {sid}')
            continue
        st, comp = stages[sid]
        if st['type'] == 'round_robin':
            seen = set()
            for m in res.get('matches', []):
                k = (m['home'], m['away'])
                if k in seen or m['home'] not in comp['participants']['clubs'] or m['away'] not in comp['participants']['clubs']:
                    bad += fail(path, f'results {sid}: bad or repeated match {k}')
                seen.add(k)
        elif st['type'] == 'knockout':
            for t in res['ties']:
                rd = st['rounds'][t['round'] - 1]
                if len(t['legs']) != rd['legs'] or any(c not in clubs for c in t['clubs']):
                    bad += fail(path, f'results {sid}: tie {t["clubs"]} has {len(t["legs"])} legs, the round {rd["legs"]}')
                for lg in t['legs']:
                    if 'penalties' in lg and 'penalties' not in rd['decider']:
                        bad += fail(path, f'results {sid}: penalties in a round without them')
                    if 'extra_time' in lg and 'extra_time' not in rd['decider']:
                        bad += fail(path, f'results {sid}: extra time in a round without it')
    for sid, exp in sc['expect'].get('tables', {}).items():
        st, comp = stages[sid]
        order, row = table(st, comp, sc['results'][sid])
        if order is None:
            bad += fail(path, f'{sid}: the scripted results leave a tie only draw_lots breaks')
            continue
        cols = exp['columns']
        for i, r in enumerate(exp['rows']):
            got = [order[i]] + [row[order[i]][k] for k in cols[1:-1]]
            if got != r[:-1]:
                bad += fail(path, f'{sid} row {i + 1}: expected {r[:-1]}, the results give {got}')
    if not bad:
        print(f'ok   {path}: {sc["name"]}')
    return bad


if __name__ == '__main__':
    sys.exit(1 if sum(check(p) for p in sys.argv[1:]) else 0)
