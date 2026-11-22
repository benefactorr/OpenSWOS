#!/bin/sh
# Every tests/specs/ok/*.json must validate, compile and read back; every tests/specs/bad/*.json must fail with the
# message in its "_expect" member (bad/syntax.json: a syntax error).
SWCC=$1; TMP=${TMPDIR:-/tmp}/swcc-tests.$$; mkdir -p "$TMP"; fail=0
for f in tests/specs/ok/*.json examples/*.json; do
  if ! "$SWCC" compile "$f" -o "$TMP/out.swc" >/dev/null 2>"$TMP/err" || ! "$SWCC" dump "$TMP/out.swc" >/dev/null; then
    echo "FAIL (should be valid): $f"; cat "$TMP/err"; fail=1; fi
done
for f in tests/specs/ok-openswos/*.json packs/*/world.json examples/*.json; do       # the OpenSWOS profile (latin1 + overrides)
  if ! "$SWCC" validate "$f" --profile profiles/openswos.json >/dev/null 2>"$TMP/err"; then
    echo "FAIL (should be valid for openswos): $f"; cat "$TMP/err"; fail=1; fi
done
for f in tests/specs/ok-openswos/*.json; do                           # ... and refused by the DOS exe's profile
  if "$SWCC" validate "$f" >/dev/null 2>&1; then echo "FAIL (swos9697 should refuse): $f"; fail=1; fi
done
for f in tests/specs/bad/*.json; do
  exp=$(sed -n 's/.*"_expect": "\(.*\)".*/\1/p' "$f" | sed 's/\\"/"/g'); [ -z "$exp" ] && exp="(syntax)"
  if "$SWCC" validate "$f" >/dev/null 2>"$TMP/err"; then echo "FAIL (should be refused): $f"; fail=1
  elif ! grep -qF -- "$exp" "$TMP/err"; then echo "FAIL (message): $f: expected: $exp"; cat "$TMP/err"; fail=1; fi
done
for p in packs/*/world.json; do                     # packs: compile + the dump equals tests/golden/<pack>.dump
  n=$(basename "$(dirname "$p")")
  if ! "$SWCC" compile "$p" -o "$TMP/$n.swc" >/dev/null 2>"$TMP/err"; then echo "FAIL (pack): $p"; cat "$TMP/err"; fail=1; continue; fi
  "$SWCC" dump "$TMP/$n.swc" | sed "s|$TMP/||" > "$TMP/$n.dump"
  if ! diff -u "tests/golden/$n.dump" "$TMP/$n.dump"; then echo "FAIL (golden): $p"; fail=1; fi
  if ! cmp -s "tests/golden/$n.swc" "$TMP/$n.swc"; then echo "FAIL (golden bytes): $p differs from tests/golden/$n.swc"; fail=1; fi
done
rm -rf "$TMP"
[ $fail = 0 ] && echo "run_specs: $(ls tests/specs/ok/*.json | wc -l | tr -d ' ') valid, $(ls tests/specs/bad/*.json | wc -l | tr -d ' ') refused with the expected message, $(ls packs/*/world.json | wc -l | tr -d ' ') pack(s) = golden"
exit $fail
