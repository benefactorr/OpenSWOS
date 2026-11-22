# SWOS Competition Engine

A deterministic, declarative competition engine for Sensible World of Soccer: leagues, cups and continental
competitions of any country and era described as data (JSON specs), played by one portable C engine.

> **In breve (IT).** Motore delle competizioni per SWOS: campionati, coppe e coppe continentali di qualsiasi
> nazione ed epoca descritti come dati (specifiche JSON) e giocati da un unico motore in C. Sarà usato da
> [OpenSWOS](https://github.com/benbaker76/OpenSWOS) come submodule e dal
> [SWOS 96/97 Mod](https://github.com/aminta/swos-9697-mod). Stato: progetto e specifiche; il codice arriva nelle
> prossime settimane.

**Status: the shared part.** The engine is first integrated into OpenSWOS by its maintainer, from this spec, and
then extracted here as a library. This repository holds what every implementation shares: the spec schema
(`schema/v1.md`, frozen), the spec compiler / validator `swcc`, the compiled spec format (`.SWC`, big-endian,
`docs/format.md`) with its reference reader and the determinism primitives (`format/`), host profiles, a sample pack,
and the **contract tests** both sides must pass (`docs/contract-tests.md`).

## Layout and build

| Path | Content |
|---|---|
| `rules/` | the rules library: the Mod's season-end and cup rules both games play, one C copy, freestanding (`docs/rules-library.md`) |
| `format/` | the shared reference, C99 freestanding: `.SWC` big-endian accessors, record layouts, reference reader; PCG32, substreams, FNV-1a |
| `swcc/` | the spec compiler / validator (hosted C11): JSON + host profile -> diagnostics + binary spec `.SWC` |
| `schema/v1.md` | spec schema swc/1: the frozen primitives list, what S0 validates |
| `profiles/` | host limits: `swos9697.json` (the 1996 DOS exe), `openswos.json` (OpenSWOS SDL + Amiga, draft) |
| `examples/` | specs of real competitions, club names only (Serie A + Serie B 2026-27) |
| `packs/sample/` | the reference data pack (invented clubs, CC0): one league + one cup with a fixed draw |
| `tests/` | format tests, valid and broken specs (each with its expected message), golden outputs; `tests/contract/`: the scenarios |
| `docs/` | design documents |

```
make            # build/swcc, build/libswcc.so, the tests, the i486 freestanding object check
build/swcc validate SPEC.json [--profile profiles/swos9697.json]
build/swcc compile  SPEC.json -o OUT.SWC
build/swcc dump     OUT.SWC
make rules-check MOD=../swos-9697-mod    # the rules library against the Mod's own C
```

## Who uses it

- **[OpenSWOS](https://github.com/benbaker76/OpenSWOS)** (Ben Baker's C port of SWOS), as a git submodule.
  World 2026-27 is planned as the default there, with SWOS 96/97 one switch away.
- **[SWOS 96/97 Mod](https://github.com/aminta/swos-9697-mod)** (Davide Lorigliola), linked into the original
  1996 DOS executable.
- The engine itself is also built natively for tests and tools.

## Design in short

- **Declarative**: a competition is an ordered list of stages (round robin, groups, league phase, knockout,
  play-off/play-out, split, aggregate table, promedio...), with named tie-breakers, deciders, outcomes and links
  between competitions and seasons. Worlds are lists of **modules** (one per country or confederation) that
  connect only through named **ports**, so a module can be added or removed without touching the others.
- **Deterministic**: same specs + same seed + same results = the same season, byte for byte. Golden-file tests.
- **Portable C99, freestanding**: no libc, fixed-width integers, no floats in decisions; small enough for a
  486 and an Amiga, built into OpenSWOS, into the DOS mod, and natively.
- **Compiled specs**: JSON is the source; a spec compiler produces a compact, versioned binary form for small hosts.
- **Persistent state with versioning**: hosts save the engine state (named JSON fields in OpenSWOS, a versioned
  trailer in the DOS mod); saves record the data pack and its version.
- **User-authored specs**: strict validation with clear, located error messages.
- **Every world in every mode**: a world registers its teams and competitions once; career, friendly, season and
  preset competitions all list them.
- **No original game data** in this repository: data packs contain only new data, each with its own licence and
  credits.

## Documents

| Document | Content |
|---|---|
| [docs/rules-library.md](docs/rules-library.md) | the rules library: the Mod's rules both games link, module by module, each checked against the Mod's C |
| [docs/format.md](docs/format.md) | the compiled spec (.SWC): big-endian, one file for DOS, SDL and Amiga; layouts; C arrays |
| [docs/contract-tests.md](docs/contract-tests.md) | **the contract tests**: YAML scenarios + golden files every engine implementation must pass |
| [docs/competition-engine-proposal.md](docs/competition-engine-proposal.md) | spec model, API, determinism, persistence, four real formats (Liga MX, Serie C, UEFA Champions League, Argentina) |
| [docs/rule-catalogue.md](docs/rule-catalogue.md) | modules and ports, every rule primitive (from the Mod's World 2026-27), entitlements and precedence, a complete CONCACAF module, the validator |
| [docs/testing-strategy.md](docs/testing-strategy.md) | test pyramid: validation, unit tests per primitive, properties, forced scenarios, golden worlds, few end-to-end tests |
| [docs/lessons-from-the-mod.md](docs/lessons-from-the-mod.md) | real bugs met by the Mod and how the engine prevents each |
| [docs/ai-development-guidelines.md](docs/ai-development-guidelines.md) | how AI-assisted work is done here: contract first, tests, small reviewed PRs |

Background (how OpenSWOS, the original DOS game and the Mod run competitions today):
[OpenSWOS convergence docs](https://github.com/aminta/OpenSWOS/tree/convergence-docs/docs/convergence).
The documents were written for the OpenSWOS convergence and still refer to OpenSWOS paths in places; they will be
adapted as the design is frozen here.

## Licence

[MIT](LICENSE). Compatible with OpenSWOS's GPL-3.0-or-later. Data packs are licensed separately.
