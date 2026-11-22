# Contract tests

> **In breve (IT).** I test che ogni implementazione del motore deve passare (quella dentro OpenSWOS, poi la
> libreria). Ogni scenario è un file JSON: un pacchetto dati, i risultati forzati delle partite, e cosa deve
> uscire (classifiche, vincitori, chi passa e perché). Niente partite giocate "sperando": i risultati sono
> imposti. `tests/contract/check.py` controlla che gli scenari stessi siano coerenti; ogni motore li esegue con
> il proprio runner e deve produrre esattamente le osservazioni attese.

The engine is written first inside OpenSWOS, then extracted as a library; the Mod's DOS exe links that library
later. These files are what makes the implementations agree. They follow `docs/testing-strategy.md` L3 (forced
scenarios) and L4 (golden worlds).

## Files

| Path | Content |
|---|---|
| `tests/contract/<pack>/<name>.json` | one scenario (format below) |
| `tests/contract/check.py` | the scenario checker: well-formed against the pack; expected round-robin tables recomputed from the scripted results (a cross-check of the hand-written expectations, not an engine) |
| `tests/golden/<pack>.swc` | the compiled pack, byte for byte (format 2, big-endian): every implementation reads the same bytes |
| `tests/golden/<pack>.dump` | `swcc dump` of it |

## Scenario format `swc-contract/1` (canonical JSON; YAML only as authoring sugar)

```json
{
  "contract": "swc-contract/1",
  "name": "one line: what the case proves",
  "pack": "packs/sample",
  "season": 1,
  "results": {
    "<comp id>:<stage id>": {                         round robin
      "default": [0, 0],                              every match not listed
      "matches": [ {"home": "<club id>", "away": "<club id>", "score": [h, a]} ]
    },
    "<comp id>:<stage id>": {                         knockout
      "ties": [ {"round": 1, "clubs": ["<club id>", "<club id>"],
                 "legs": [ {"goals": [x, y], "extra_time": [x, y], "penalties": [x, y]} ]} ]
    }
  },
  "expect": {
    "tables":    { "<comp>:<stage>": {"columns": [...], "rows": [[club, played, won, drawn, lost, gf, ga, points, zone]]} },
    "ties":      { "<comp>:<stage>": [ {"round": n, "winner": "<club>", "decided_by": "goals|aggregate|away_goals|extra_time|penalties|better_placed", "neutral": bool},
                                       {"round": n, "pairs": [[club, club], ...]} ] },
    "champions": { "<comp id>": "<club id>" },
    "moves":     [ {"club": "<club>", "kind": "promoted|relegated|qualified", "to": "<comp id>[:<stage id>]"} ],
    "why":       [ "plain-language reasons (documentation only)" ]
  }
}
```

Rules:

- **Results are forced, never hoped for.** A knockout leg gives goals **per club in the order of `clubs`**, not
  per venue, so a scenario does not depend on a drawn first-leg host — except a case that tests away goals,
  which must also fix the venue (`"home": "<club>"` on the leg; next version of the format).
- **A scenario must not depend on `draw_lots`** or on any random draw it does not fix: the checker refuses a
  table whose order would need the lots. Draw-dependent behaviour is tested by golden worlds with a fixed seed.
- Expected values are written by hand from the competition's real rules; `check.py` recomputes the tables as
  a guard against typos. The `why` lines say which rule decided each close case.
- Every rule primitive of `schema/v1.md` gets at least one scenario; every bug found in a game becomes one.

## Running against an engine

An implementation provides a runner that loads `tests/golden/<pack>.swc` (or compiles the pack with `swcc`),
plays season `season` feeding the scripted results to the engine through its own result path (the same path the
game uses), and writes its observations in the `expect` shape; the observations must equal `expect` (fields
not in `expect` are ignored). The runner's name and invocation belong to each host (OpenSWOS: a headless
`--comp-selftest`-style switch, testing-strategy L5).

## Present scenarios

| Scenario | Proves |
|---|---|
| `sample/league-h2h.json` | points → goal difference → goals for → head-to-head points; zone of the champion |
| `sample/cup-fixed-draw.json` | fixed first-round draw; extra time; penalties; two-legged aggregate; bracket pairing of the next round; neutral final |
