#!/bin/sh
# tests/rules/check.sh -- the rules library against the SWOS 96/97 Mod's own C: both over the
# same random seasons, every result compared (docs/rules-library.md, "Tests").
#
#   MOD=<a swos-9697-mod checkout> sh tests/rules/check.sh [SEASONS]    (default 20000)
#
# The Mod's tools/*.c are built as they are; skipped (exit 0) without a checkout.
set -eu
N=${1:-20000}
MOD=${MOD:-}
CC=${CC:-cc}
if [ -z "$MOD" ] || [ ! -f "$MOD/tools/dplay.c" ]; then
    echo "SKIP: tests/rules/check.sh needs MOD=<a swos-9697-mod checkout>"
    exit 0
fi
T=tests/rules
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
C="$CC -O1 -w"
# The Mod's own C, built as it is -- with its automatic variables given a value where the compiler
# can: the Mod 4.0.5's per-play-off seed (gplay.c gp_season, rc19) reads club[i] of an entrant that
# failed its check and never set it (San Marino's 'europe' entrants past its 6 clubs), which here
# was a random address -- a crash, once in a few runs. Pattern-filled it is negative, i.e. what the
# library does (no club). Reported to the Mod (2026-11-01).
MC="$C"
if echo 'int main(void){return 0;}' | $CC -ftrivial-auto-var-init=pattern -x c -o /dev/null - 2>/dev/null; then
    MC="$C -ftrivial-auto-var-init=pattern"
fi

# The Italian rules (sr_italy = dplay.c dp_season): Serie B and Serie C's play-offs, play-outs.
$MC -DDPLAY_C="\"$MOD/tools/dplay.c\"" -o "$OUT/italy_mod" $T/italy_mod.c
$C -Irules -o "$OUT/italy_ours" $T/italy_ours.c rules/sr_dplay.c rules/sr_check.c
"$OUT/italy_mod" "$N" > "$OUT/italy_mod.txt"
"$OUT/italy_ours" "$N" > "$OUT/italy_ours.txt"
if cmp -s "$OUT/italy_mod.txt" "$OUT/italy_ours.txt"; then
    echo "PASS: the Italian rules, $N seasons, $(awk '{ m += NF - 1 } END { print m }' "$OUT/italy_ours.txt") moves, every one the Mod's"
else
    echo "FAIL: the Italian rules: the first season that differs (Mod, then the library):"
    diff "$OUT/italy_mod.txt" "$OUT/italy_ours.txt" | head -4
    exit 1
fi

# The season end (sr_italy, then sr_knockouts = gplay.c gp_season, whole): Italy 2026-27 first --
# both steps, every move, result, stage title and SEASON WINNERS page --, then every pack's
# knockouts (tests/rules/specs: the Mod's compile_spec bytes of each World 2026-27 league, no
# names) over random tables of that league's shape, its stage tables given.
$MC -DDPLAY_C="\"$MOD/tools/dplay.c\"" -c -o "$OUT/kmod_dp.o" $T/knockouts_mod_dp.c
$MC -DGPLAY_C="\"$MOD/tools/gplay.c\"" -c -o "$OUT/kmod_gp.o" $T/knockouts_mod_gp.c
$C -o "$OUT/kmod" $T/knockouts_mod.c "$OUT/kmod_dp.o" "$OUT/kmod_gp.o"
$C -Irules -o "$OUT/kours" $T/knockouts_ours.c rules/sr_dplay.c rules/sr_gplay.c rules/sr_check.c
"$OUT/kmod" $T/specs/ita.spec "$N" > "$OUT/k_mod.txt"
"$OUT/kours" $T/specs/ita.spec "$N" > "$OUT/k_ours.txt"
if cmp -s "$OUT/k_mod.txt" "$OUT/k_ours.txt"; then
    echo "PASS: Italy's season end, $N seasons, $(awk '{ m += NF - 1 } END { print m }' "$OUT/k_ours.txt") moves and results, every one the Mod's"
else
    echo "FAIL: Italy's season end: the first season that differs (Mod, then the library):"
    diff "$OUT/k_mod.txt" "$OUT/k_ours.txt" | head -4
    exit 1
fi
PN=$((N / 10)); np=0; bad=""
while read -r code shape; do
    "$OUT/kmod" "$T/specs/$code.spec" "$PN" "$shape" ko > "$OUT/p_mod.txt"
    "$OUT/kours" "$T/specs/$code.spec" "$PN" "$shape" ko > "$OUT/p_ours.txt"
    cat "$OUT/p_ours.txt" >> "$OUT/p_all.txt"
    if cmp -s "$OUT/p_mod.txt" "$OUT/p_ours.txt"; then np=$((np + 1)); else
        bad="$bad $code"
        [ -n "${VERBOSE:-}" ] && { echo "  $code ($shape):"; diff "$OUT/p_mod.txt" "$OUT/p_ours.txt" | head -3; }
    fi
done < $T/specs/shapes.txt
if [ -z "$bad" ]; then
    echo "PASS: every pack's knockouts ($np leagues), $PN seasons each: every move, champion, stage title and SEASON WINNERS page the Mod's; the career club's contest built as its ko_build ($(grep -o "K[0-9]*:" "$OUT"/p_all.txt | wc -l) contests, their round-1 ties simulated at the build), every club's play-off chosen as its gp_need"
else
    echo "FAIL: the knockouts of$bad differ from the Mod's ($np equal; VERBOSE=1 for the first seasons)"
    exit 1
fi

# The season-end preview (gplay.c GP_PRE, 4.0.5 rc19): before each season end, the same knockouts
# previewed -- play-off 0 the career club's own every other season --: moves, titles and the
# SEASON WINNERS entries it appends, Italy and every pack.
KO_PRE=1 "$OUT/kmod" $T/specs/ita.spec "$N" > "$OUT/pre_mod.txt"
KO_PRE=1 "$OUT/kours" $T/specs/ita.spec "$N" > "$OUT/pre_ours.txt"
cmp -s "$OUT/pre_mod.txt" "$OUT/pre_ours.txt" || { echo "FAIL: Italy's season-end preview"; diff "$OUT/pre_mod.txt" "$OUT/pre_ours.txt" | head -4; exit 1; }
while read -r code shape; do
    KO_PRE=1 "$OUT/kmod" "$T/specs/$code.spec" "$PN" "$shape" > "$OUT/pre_mod.txt"
    KO_PRE=1 "$OUT/kours" "$T/specs/$code.spec" "$PN" "$shape" > "$OUT/pre_ours.txt"
    cmp -s "$OUT/pre_mod.txt" "$OUT/pre_ours.txt" || { echo "FAIL: $code's season-end preview"; exit 1; }
done < $T/specs/shapes.txt
echo "PASS: the season-end preview, Italy ($N seasons) and every pack ($PN each): the Mod's"

# B teams as play-off entrants (gplay.c place_club / sec_inelig): tables.h's two B teams and their
# first teams at random places (-DKO_BT), passed over by place unless they may play play-offs and go
# up -- Italy's levels on Italy's knockouts, a level per division on every pack's (the contests too:
# the Mod given the tables the game has then, its club's divisions only); three settings each:
# play-offs allowed no higher than level 1, not allowed, allowed at any level.
nbt=0
for lv in 2 1; do
    for v in "1 1" "255 0" "255 1"; do
        set -- $v
        D="-DKO_BT=$lv -DKO_BT_TOP=$1 -DKO_BT_FLAGS=$2"
        $MC $D -DDPLAY_C="\"$MOD/tools/dplay.c\"" -c -o "$OUT/bt_dp.o" $T/knockouts_mod_dp.c
        $MC $D -DGPLAY_C="\"$MOD/tools/gplay.c\"" -c -o "$OUT/bt_gp.o" $T/knockouts_mod_gp.c
        $C $D -o "$OUT/btmod" $T/knockouts_mod.c "$OUT/bt_dp.o" "$OUT/bt_gp.o"
        $C $D -Irules -o "$OUT/btours" $T/knockouts_ours.c rules/sr_dplay.c rules/sr_gplay.c rules/sr_check.c
        if [ $lv = 2 ]; then
            "$OUT/btmod" $T/specs/ita.spec "$PN" > "$OUT/bt_mod.txt"
            "$OUT/btours" $T/specs/ita.spec "$PN" > "$OUT/bt_ours.txt"
            cmp -s "$OUT/bt_mod.txt" "$OUT/bt_ours.txt" || { echo "FAIL: B teams in Italy's knockouts (top $1, flags $2)"; exit 1; }
            nbt=$((nbt + 1))
        else
            while read -r code shape; do
                "$OUT/btmod" "$T/specs/$code.spec" "$PN" "$shape" ko > "$OUT/bt_mod.txt"
                "$OUT/btours" "$T/specs/$code.spec" "$PN" "$shape" ko > "$OUT/bt_ours.txt"
                cmp -s "$OUT/bt_mod.txt" "$OUT/bt_ours.txt" || { echo "FAIL: B teams in $code's knockouts (top $1, flags $2)"; exit 1; }
                nbt=$((nbt + 1))
            done < $T/specs/shapes.txt
        fi
    done
done
echo "PASS: B teams as play-off entrants, $nbt leagues and settings, $PN seasons each: every move, result and contest the Mod's"

# The season's moves and the B-team rules (sr_final_moves, sr_b_teams = gplay.c sec_post): built
# cases, then random ones against the Mod's sec_post -- Italy (levels of groups) and the other
# countries with B teams (Spain, Germany, the Netherlands, Portugal).
$MC -DGPLAY_C="\"$MOD/tools/gplay.c\"" -o "$OUT/bmod" $T/bteams_mod.c
$C -Irules -o "$OUT/bours" $T/bteams_ours.c rules/sr_post.c rules/sr_check.c
"$OUT/bours" || exit 1
"$OUT/bmod" "$N" > "$OUT/b_mod.txt"
"$OUT/bours" random "$N" > "$OUT/b_ours.txt" 2> "$OUT/b.log"
if cmp -s "$OUT/b_mod.txt" "$OUT/b_ours.txt"; then
    echo "PASS: B teams, $N cases in each of Italy, Spain, Germany, the Netherlands, Portugal ($(cut -d' ' -f1 "$OUT/b.log") changed by the rules), every move the Mod's"
else
    echo "FAIL: B teams: the first case that differs (Mod, then the library):"
    diff "$OUT/b_mod.txt" "$OUT/b_ours.txt" | head -4
    exit 1
fi

# A cup's clubs from the second season on (sr_cup_entrants = gplay.c cf_build, CF_SEED 1 as
# released): Italy 2026-27's two cups over random moves, last season's places and career clubs.
$MC -DCF_SEED=1 -DGPLAY_C="\"$MOD/tools/gplay.c\"" -o "$OUT/cmod" $T/cups_mod.c
$C -Irules -o "$OUT/cours" $T/cups_ours.c rules/sr_cups.c rules/sr_check.c
"$OUT/cmod" "$N" > "$OUT/c_mod.txt"
"$OUT/cours" "$N" > "$OUT/c_ours.txt"
if cmp -s "$OUT/c_mod.txt" "$OUT/c_ours.txt"; then
    echo "PASS: cups, $N seasons, both cups' clubs the Mod's"
else
    echo "FAIL: cups: the first season that differs (Mod, then the library):"
    diff "$OUT/c_mod.txt" "$OUT/c_ours.txt" | head -4
    exit 1
fi

# The parallel groups (sr_pg_season = gplay.c pg_season, 4.0.5's two choices of group): each league
# with groups (tests/rules/specs/<code>.pg: the Mod's pgroups.table_bytes) over random season ends --
# the clubs coming back from below on odd seasons, every fourth season all of one area --, every
# club placed in the Mod's group.
$MC -I$T -DGPLAY_C="\"$MOD/tools/gplay.c\"" -o "$OUT/gmod" $T/groups_mod.c
$C -Irules -I$T -o "$OUT/gours" $T/groups_ours.c rules/sr_groups.c
GN=$((N / 10)); bad=""
for code in $(cat $T/specs/groups.txt); do
    "$OUT/gmod" $T/specs/$code.pg $T/specs/$code.pr "$GN" > "$OUT/g_mod.txt"
    "$OUT/gours" $T/specs/$code.pg $T/specs/$code.pr "$GN" > "$OUT/g_ours.txt"
    cmp -s "$OUT/g_mod.txt" "$OUT/g_ours.txt" || bad="$bad $code"
done
if [ -z "$bad" ]; then
    echo "PASS: the parallel groups ($(tr '\n' ' ' < $T/specs/groups.txt| sed 's/ $//')), $GN season ends each: every club placed as the Mod's pg_season places it"
else
    echo "FAIL: the parallel groups of$bad differ from the Mod's pg_season"
    exit 1
fi

# The Coppa Italia Serie D's later seasons (sr_cupd_make = gplay.c cd_rank, cd_season, cd_make):
# random season ends of Italy 2026-27 (moves, Serie D's tables, the pool's arrivals), each later
# season's preliminary round and first round of 64.
$MC -DGPLAY_C="\"$MOD/tools/gplay.c\"" -o "$OUT/dmod" $T/cupd_mod.c
$C -Irules -o "$OUT/dours" $T/cupd_ours.c rules/sr_cupd.c rules/sr_check.c
"$OUT/dmod" "$N" > "$OUT/d_mod.txt"
"$OUT/dours" "$N" > "$OUT/d_ours.txt"
if cmp -s "$OUT/d_mod.txt" "$OUT/d_ours.txt"; then
    echo "PASS: the Coppa Italia Serie D, $N later seasons, every first two rounds the Mod's"
else
    echo "FAIL: the Coppa Italia Serie D: the first season that differs (Mod, then the library):"
    diff "$OUT/d_mod.txt" "$OUT/d_ours.txt" | head -4 | cut -c1-200
    exit 1
fi
