# Compiled spec format (.SWC), format 2

> **In breve (IT).** Il formato binario che `swcc` scrive e il motore legge. **Big-endian**, come i dati di
> OpenSWOS (nati sull'Amiga): lo stesso file gira sull'exe DOS (i486), sugli host SDL e sull'Amiga m68k. Il lettore
> non legge mai strutture "in place": ogni campo passa dagli accessori big-endian e viene decodificato in record
> nativi nell'arena.

JSON is the format outside the game; inside, everything is data structures. `swcc` turns a spec (`schema/v1.md`)
into this binary; an engine decodes it once when it opens a world (reference reader: `format/swc_format.c`).

## Rules

- **Byte order: big-endian** for every multi-byte field (OpenSWOS's convention, `include/endian_helpers.h` there:
  data authored big-endian as on the Amiga, no-op reads on m68k). The reference accessors are
  `SWC_BE_READ_U16 / U32` and `SWC_BE_WRITE_U16 / U32` (`format/swc_format.h`), byte reads that work at any alignment
  on any host.
- **No struct casts over the buffer.** Records are decoded into native structs allocated in the arena, field by
  field, by the layout strings of `swc_layout()` (`'1'`, `'2'`, `'4'` = field sizes in order). File and native
  struct have the same field order and no padding; the engine checks each layout's size against `sizeof` at open and
  refuses (`SWC_E_FORMAT`) a compiler that lays a struct out differently. The build fails if engine code casts the
  spec buffer (`make test`, "no casts over the spec buffer").
- Strings (`STRS`) and byte lists (`TIEB`, `DECI`) stay in the buffer: the buffer must outlive the engine.
- Strings are in the **host's encoding** (the profile's `encoding`: `ascii`, or `latin1` + `overrides` for bytes
  such as OpenSWOS's `€` 0x9D); `swcc` converts from the JSON's UTF-8 and checks every byte against the profile's
  font.
- Sections start at 4-byte offsets (the reader does not need it; it keeps dumps readable).

## Layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic: the bytes `S W C B` |
| 4 | 2 | format = 2 (format 1 was the S0 little-endian draft: refused) |
| 6 | 2 | engine_min: lowest engine version that runs it |
| 8 | 4 | flags (0) |
| 12 | 2 | world: the host's world number |
| 14 | 2 | nsect: directory entries |
| 16 | 4 | hash: FNV-1a 32 of every byte after the directory |
| 20 | 4 | arena: an engine's worst-case working memory for this spec, beside the decoded records (≈ the file size) |
| 24 | 4 | length of the file |
| 28 | 4 | reserved (0) |
| 32 | 12 × nsect | directory: tag (4 ASCII bytes, e.g. `STRS`), offset, length |

| Tag | Record | Layout |
|---|---|---|
| `STRS` | bytes: NUL-terminated strings | — |
| `NAME` | id; per language (it, en, fr, de) long, short: string offsets | `444444444` |
| `CLUB` | id, name, host file, host ordinal, country, area | `442222` |
| `COMP` | id, name, kind, level, country, first stage, stages, entrants, n entrants, first outcome, n outcomes, win, draw, loss, flags | `421142222221111` |
| `STAG` | id, RNG purpose, competition, type, legs, teams, groups, tie-breakers, first round, rounds, flags | `44211222222` |
| `ENTR` | club index | `2` |
| `TIEB`, `DECI` | bytes: code lists ending with 0 | — |
| `ROUN` | ties, legs, home, deciders, pairing, pad | `211211` |
| `OUTC` | stage, from, to (positions: 1..127, or 128 + N = N-th from the bottom), tag, zone, target (0xFFFE = out of the world), season, pad | `21111211` |
| `REG ` | competition, game modes | `22` |
| `META` | pack id, pack version (string offsets) | `44` |
| `FIXT` | round, home club, away club (fixed draws) | `222` |
| `GRPS` | stage, size (one record per group; only with unequal groups) | `22` |
| `SRCE` | competition, source competition | `22` |

Unknown tags are skipped (a newer compiler may add sections a reader can ignore); a format that changes a layout
bumps `format`. `tests/golden/sample.swc` is the reference file (the sample pack, byte for byte).

## Baking a pack into a build

`swcc compile SPEC.json -o pack.c --c NAME` writes the same bytes as a C array (`const unsigned char NAME[]` +
`NAME_size`), for hosts that build their data into the binary (OpenSWOS's JSON -> C habit). The engine reads it
exactly as a file loaded from disk.
