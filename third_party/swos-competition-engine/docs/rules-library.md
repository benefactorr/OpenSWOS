# The rules library (`rules/`)

> **In breve (IT).** Le regole di fine stagione e delle coppe che il Mod e OpenSWOS condividono, in
> una sola copia C: playoff e playout, i knockout dei pack, le squadre B, l'ingresso nelle coppe, la
> Coppa Italia Serie D. Ognuna è il codice del Mod come OpenSWOS l'ha portato, confrontato con il C
> del Mod stagione per stagione (`make rules-check MOD=...`).

Status: 2026-11-01, version 1 being moved in, one module per pull request (§4); every module the
Mod **4.0.5**'s (tag v4.0.5, main 77b3767; §4a).

## 1. What it is

The competition rules the SWOS 96/97 Mod and OpenSWOS both play, as one C library both games link:

- **Input**: a league's final tables and its rules as the Mod's tools compile them (`compile_spec`).
- **Output**: each club's move (and the results the rules decide: champions, cup entrants).
- **No engine**: calendars, menus, contests, saves stay each game's own.

Agreed between Davide Lorigliola (the Mod: he owns the rules and their tests) and OpenSWOS (it keeps
the C portable and the interface clean), 2026-10-27; the interface was approved 2026-10-29
(OpenSWOS `docs/RULES_LIBRARY.md` §7).

It is not the declarative engine of `docs/competition-engine-proposal.md`: it is the rules both
games play **today**, the Mod's own code, kept identical between them. The engine can take them as
its reference behaviour.

## 2. The code

Each module is the Mod's tool C (`tools/dplay.c`, `tools/gplay.c`, ...) as OpenSWOS ported it and
checked it against the Mod's own C, season after season: its random numbers, seeds, tie model,
order and types (`int` is 32 bits on every target), so it reads against the Mod's line by line.
What changed in the move here: the file statics are one caller-owned state, `sr_state`.

**The DOS build's limits** (the Mod's `tools/cobj.py`: clang `-target i686`, freestanding,
`-Os -march=i486`, no SSE, cdecl, linked into the exe):

- no undefined symbols: **no libc**, no compiler-generated `memcpy` / `memset` -- `make test`
  builds every module as one i486 object and fails on any undefined symbol;
- **one caller-owned state passed by pointer**, no globals (few absolute references: each is a
  fixup);
- `<stdint.h>` the one include.

## 3. Interface (`rules/swosrules.h`)

```c
sr_state st;            /* the caller's memory */
sr_reset(&st);          /* once */
/* each season end: the final tables into st.t, then */
sr_italy(&st, &league, &season);    /* Serie B, Serie C: st.mv[0 .. st.nmv) */
sr_knockouts(&st, &league, &season);    /* the pack's knockouts: their moves after those */
sr_final_moves(&st, &league, &levels, base);    /* each club's one-level move: st.want[d][k] */
```

The caller keeps, between seasons and in its saves, what the Mod keeps in its state: a league's
stage tables (`sr_stage_bank`, written at each fixture stage's end), Argentina's promedio history
(`sr_promedio`) and the SEASON WINNERS page (`sr_winners`); `sr_season` points at them. What the
career club played itself comes back through `sr_host`'s callbacks (all optional).

| Function | The Mod's | What it does |
|---|---|---|
| `sr_italy` | `dplay.c` `dp_season` | Serie B's play-off and play-out, Serie C's play-off (the career club's played ties and the Coppa Italia Serie C winner given in `sr_season`) |
| `sr_club_playoff` | `gplay.c` `gp_need` | the career club's play-off at its league's end, and the divisions to simulate early for it |
| `sr_contest_entrants`, `sr_contest_seed`, `sr_contest_tie` | `gplay.c` `ko_build` | that play-off's entrants (ranks, points) for the game's contest, and its round-1 ties simulated at the build (presim) |
| `sr_group_shape_of` | `gplay.c` `t2_shape` | a title of groups then knockouts (the Poule Scudetto): its shape, for a group contest |
| `sr_spec_at` | `gplay.c` `po_at` | play-off pidx's bytes |
| `sr_final_moves` | -- | each club's one-level move: the record's by place, the play-offs' on top, then the B-team rules |
| `sr_cup_entrants` | `gplay.c` `cf_build` (4.0.1) | a cup's clubs from the second season: the real list's shape per division, taken from where the clubs are now |
| `sr_cupd_keep_place`, `sr_cupd_note_arrival`, `sr_cupd_make` | `gplay.c` `cd_rank`, `cd_season`, `cd_make` | the Coppa Italia Serie D's later seasons: the entry order from the season end, the preliminary round and the first round of 64 |
| `sr_b_teams` | `gplay.c` `sec_post` | the B-team rules alone (in levels, a level of groups counting as one; the Mod's division deltas are the same moves) |
| `sr_spec_check`, `sr_league_check`, `sr_season_check` | -- | untrusted input, checked once: a spec written by hand, a damaged save (§3a) |
| `sr_pg_season` | `gplay.c` `pg_season` (4.0.5; 4.0.6's `pg_a2` guard) | a league with parallel groups at the season end: each move one level, into a level of groups by the club's 1st / 2nd choice of group, a full group making room (schema/v1.md, "Parallel leagues by area") |
| `sr_knockouts` | `gplay.c` `gp_season` | every pack's knockouts, whole: each kind and entrant, group rounds, the drawn pot, the promedio; titles, continental places and stage titles in `st.gp.res` / `st.gp.sres`; the SEASON WINNERS page |

### 3a. Untrusted input (`rules/sr_check.c`)

The rules are the Mod's code with the Mod's fixed buffers: a play-off's entrants in 64 slots, a
round's members in 128, a group's rows in 32, a stage table's in 32. The Mod's own tools never
exceed them; a spec a user wrote, or a damaged save, can. So the input is checked once, up front,
and the play code stays the Mod's line for line:

- `sr_spec_check(spec, len, divisions, &err)`: a pack's knockouts -- cut short, a count over its
  buffer (entrants, ties, group members, placings), an index past what exists (a division, a
  member: an entrant, a placing, a tie's winner or loser not there yet), a byte with no meaning
  (kind, entrant type, legs, host, level) -- refused with `SR_CHECK_TRUNCATED` / `COUNT` / `INDEX` /
  `VALUE`, the play-off, the byte and the field's name;
- `sr_league.spec_len`: required with a spec -- every entry point (`sr_italy`, `sr_knockouts`,
  `sr_club_playoff`, `sr_contest_entrants`) checks the league first (`sr_league_check`: the spec
  and its length, the Italian rules' divisions, the B teams) and the season's saved state
  (`sr_season_check`: the stage tables' and the kept table's clubs, the promedio's, the SEASON
  WINNERS entries, the levels) and refuses what is not sound -- nothing is played (`sr_italy` and
  `sr_knockouts` return -1); a spec with no length is refused (`SR_CHECK_COUNT`, "spec_len");
- `sr_cupd_make`: rounds outside 2..8 (a first round of more than 64 ties) refused (-1).

`make test` runs `tests/rules/spec_check.c` under AddressSanitizer and UBSan: every real spec
accepted; each cut short anywhere, and targeted faults, refused with their codes; random faults --
what the check lets through is played (the season end, the contests, each club's play-off) over
random tables, within every buffer (31,312 of them over the 4.0.5 and 4.0.2 specs, once, at ten
times `make test`'s count). The i486 check builds one object per module (no `-r` link: macOS has no
ELF linker) and requires every symbol one needs to be another's.

A zeroed `sr_league` / `sr_season` is the safe default in every field added since version 1's
first modules: no B-team team words, no preview (`own_pi` is a play-off's index + 1, 0 none), no B
team passed over in the knockouts (`b_in_ko` 0 -- its own switch: `base` 0 is a real value, a league
that comes first, and the team-word match does not depend on it), a level per division, team words
laid out as the Mod's (`team_file_high` 0).

**Team words are each game's own; a spec's bytes are not.** A team word is the game's in-memory
name for a club (`sr_row.team`, `second_team`): the Mod's is `file | ordinal << 8` (the x86 word over
its team id's two bytes), OpenSWOS's `file << 8 | ordinal`. A spec names a club (entrant type 8,
New Zealand's reserves) by its `(file, ordinal)` bytes, as the Mod's compile_spec writes them, so one
compiled spec -- a `.SWC` section, a pack's tables -- is the same in both games; `team_file_high`
says how to make that game's word from them.

## 4. Modules, one pull request each

| # | Module | The Mod's | Test (`tests/rules/check.sh`) |
|---|---|---|---|
| 1 | The Italian rules | `dplay.c` `dp_season` | 20,000 random seasons of Italy 2026-27's divisions: every move the Mod's |
| 2 | The pack's knockouts | `gplay.c` `gp_season` | Italy's whole season end, 20,000 seasons; then each of the 35 leagues with knockouts (4.0.5), 2,000 seasons of its own shape with stage tables: every move, result, stage title and SEASON WINNERS page the Mod's |
| 3 | The career club's contests | `gplay.c` `ko_build`, `gp_need`, `t2_shape` | the same seasons: each play-off's contest built for a waiting club (30,667: entrants, ranks, the round-1 ties simulated), every club's play-off chosen, the Mod's |
| 4 | B teams, the season's moves | `gplay.c` `sec_post` | 5 built cases, then 20,000 random ones in each of Italy (levels of groups), Spain, Germany, the Netherlands, Portugal: every move the Mod's |
| 5 | The cups' clubs from the second season | `gplay.c` `cf_build` (`CF_SEED` 1) | Italy 2026-27's two cups over 20,000 random seasons (moves, last season's places, career clubs): both lists the Mod's |
| 6 | The Coppa Italia Serie D's later seasons | `gplay.c` `cd_rank`, `cd_season`, `cd_make` | 20,000 random season ends of Italy 2026-27: every later season's first two rounds the Mod's |
| 11 | The parallel groups | `gplay.c` `pg_season`, `pg_make_room` | each league with groups (Italy, Argentina, Australia, El Salvador: `specs/<code>.pg`, the Mod's `pgroups.table_bytes`), 2,000 random season ends each -- clubs entering from below on odd seasons, every fourth season all of one area --: every club in the Mod's group. `make test`: 4.0.6's `pg_a2` guard (built cases, under the sanitizers) |

### 4a. The Mod 4.0.5 (pull request 7)

`gplay.c` from v4.0.2 to v4.0.5, in the modules above (`dplay.c`, `cf_build` and the Coppa D's part
were unchanged; d3c1221 was already in):

- **86e23df (rc19)**: each play-off draws from its own random stream, seeded from its entrants (team
  words and points) -- so the **season-end preview** (`sr_season.preview`, `own_pi`: the Mod's
  `GP_PRE`, this season's titles decided at the career club's league end; no promedio, no new stage
  tables, its SEASON WINNERS entries appended after the caller's marker) and the season end decide
  the same.
- **7c22855 (New Zealand)**: entrant type 8, a given club by its team word (absent from the tables:
  left out of a group round -- NZ's A-League reserves); a knockout round after a group decides the
  title (the Grand Final); `skip_second` (`PO_SKIP2`); `winners` in flag bits 4-6; a group shape of
  one group; SEC_TAB's entries 8 bytes (+ the B team's team word: `sr_league.second_team`).
- **PO_SUPER**: Mexico's Campeon de Campeones, the next season's supercup -- not played at the
  season end, not a contest.
- **B teams as play-off entrants** (`place_club` / `sec_inelig`, older than 4.0.2, never in the
  library: the tests gave no B teams): a B team is passed over by place (not in a play-out) unless
  its league's B teams may play play-offs and it may go up; `sr_season.base` and `levels` give it the
  global numbers and the levels.

Tested against the Mod's C at 77b3767 (`check.sh`: every part, the preview and 108 B-team leagues and
settings besides), and against Davide's own 4.0.5 goldens (his C through these harnesses at d02210b,
N = 20,000): the Italian rules, Italy's season end, the cups, the Coppa D and all 34 packs, every
line the same.

The Mod's C is built for the test with its locals pattern-filled where the compiler can
(`-ftrivial-auto-var-init=pattern`): 4.0.5's per-play-off seed reads `club[i]` of an entrant whose
check failed and never set it (San Marino's 'europe' entrants past its 6 clubs) -- a random address,
a crash once in a few runs; pattern-filled it is negative, the library's "no club".

**Not in yet**, each waiting for its comparison:

- the rest of the Coppa D's pure part -- the table walk (`cd_side`, `cd_raw`), the hosts, the
  simulated ties (`cd_str`, `cd_sim`), a round's draw: to be checked against the Mod 4.0.5, which
  has the drawn-round fix (d3c1221);
- `sec_post`'s next-season flags (4.0.5 `SEC_BQ`: may each B team go up next season, the zone
  colours): the game's, not yet the library's;
- the pool's queue (OpenSWOS `src/team_pool.c`): once compared with the Mod's RI section.

## 5. Tests

`make rules-check MOD=<a swos-9697-mod checkout> [SEASONS=n]` builds the Mod's `tools/*.c` as they
are and the library, gives both the same random seasons (`tests/rules/tables.h`: their own LCG, not
`rand()`) and compares every result. Without a checkout it is skipped. `make test` checks the i486
object.

`tests/rules/specs/` holds each World 2026-27 league's knockouts as the Mod's own tools build them
(`shapes.txt`: their divisions' sizes): bytes of the format only -- entrant places, rounds, legs, team
words --, no names, made from the Mod's packs so the test needs no world data. Compiled with
`mkseason.py`'s `Pack.gplay` (`gplay.py compile_spec`, a 'club' entrant by its team word) from the
Mod's World 2026-27 packs, 4.0.5 (data `world5tm-403c11`): `tests/rules/specs/make_specs.py MOD
PACKS` rebuilds them (the packs are not in this repository).

## 6. Licence and credits

MIT (`LICENSE`). The rules and their original C: Davide Lorigliola (the SWOS 96/97 Mod, MIT). The
port, the state and the tests: Ben Baker (OpenSWOS), who contributes them here under MIT.
