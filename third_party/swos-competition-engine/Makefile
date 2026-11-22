# The shared part: swcc (spec compiler / validator), the .SWC format reference (format/), the contract tests.
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers
FORMAT   = format/swc_format.c
SWCC     = swcc/swcc.c swcc/json.c
OUT      = build
PROFILE  = $(CURDIR)/profiles/swos9697.json
RULES    = $(wildcard rules/*.c)
I486     = -target i686-unknown-none-elf -march=i486 -ffreestanding -fno-pic -fno-pie -Os -fno-stack-protector \
           -mno-sse -mno-mmx -fno-builtin -fno-asynchronous-unwind-tables -std=c99 -Wall -Wextra -Werror

all: $(OUT)/swcc $(OUT)/libswcc.so test

$(OUT):
	mkdir -p $(OUT)

$(OUT)/swcc: $(SWCC) swcc/swcc_main.c $(FORMAT) format/swc_format.h swcc/swcc.h swcc/json.h | $(OUT)
	$(CC) $(CFLAGS) -DSWCC_DEFAULT_PROFILE='"$(PROFILE)"' -o $@ $(SWCC) swcc/swcc_main.c $(FORMAT)

$(OUT)/libswcc.so: $(SWCC) $(FORMAT) format/swc_format.h | $(OUT)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $(SWCC) $(FORMAT)

$(OUT)/test_format: tests/test_format.c $(FORMAT) format/swc_format.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_format.c $(FORMAT)

# the format reference must stay freestanding C99 (any engine links it): i486, no library calls
$(OUT)/swc_i486.o: $(FORMAT) format/swc_format.h | $(OUT)
	$(CC) $(I486) -c -o $@ format/swc_format.c

# the rules library (rules/): the same, every module -- linked into the DOS exe as it is. One i486
# object per module (no `-r` link: it needs an ELF linker, which macOS has not); the test below
# requires every symbol one of them needs to be another's.
RULES_I486 = $(patsubst rules/%.c,$(OUT)/rules_i486/%.o,$(RULES))
$(OUT)/rules_i486/%.o: rules/%.c rules/swosrules.h | $(OUT)
	@mkdir -p $(OUT)/rules_i486
	$(CC) $(I486) -c -o $@ $<

# untrusted specs and saves (rules/sr_check.c): every real spec accepted, broken ones refused, and
# what the check lets through played within every buffer -- under the sanitizers where the compiler
# has them
SANITIZE := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c -o /dev/null - 2>/dev/null && echo -fsanitize=address,undefined -fno-sanitize-recover=all)
$(OUT)/spec_check: tests/rules/spec_check.c tests/rules/tables.h $(RULES) rules/swosrules.h | $(OUT)
	$(CC) -std=c99 -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(SANITIZE) -Irules -Itests/rules -o $@ \
	    tests/rules/spec_check.c rules/sr_check.c rules/sr_gplay.c rules/sr_dplay.c

# a club named by (file, ordinal) found from team words in either game's layout (team_file_high)
$(OUT)/team_order: tests/rules/team_order.c tests/rules/tables.h $(RULES) rules/swosrules.h | $(OUT)
	$(CC) -std=c99 -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(SANITIZE) -Irules -Itests/rules -o $@ \
	    tests/rules/team_order.c rules/sr_check.c rules/sr_gplay.c rules/sr_dplay.c

# the parallel groups past what any data has: 4.0.6's pg_a2 guard (no group -1), a league past the limits
$(OUT)/groups_guard: tests/rules/groups_guard.c $(RULES) rules/swosrules.h | $(OUT)
	$(CC) -std=c99 -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(SANITIZE) -Irules -o $@ \
	    tests/rules/groups_guard.c rules/sr_groups.c

test: $(OUT)/test_format $(OUT)/swcc $(OUT)/swc_i486.o $(RULES_I486) $(OUT)/spec_check $(OUT)/team_order $(OUT)/groups_guard
	$(OUT)/swcc compile packs/sample/world.json -o $(OUT)/sample.swc >/dev/null
	$(OUT)/test_format $(OUT)/sample.swc
	sh tests/run_specs.sh $(OUT)/swcc
	python3 tests/contract/check.py tests/contract/*/*.json
	@if nm $(OUT)/swc_i486.o | grep -q ' U '; then echo "i486 object has undefined symbols:"; nm $(OUT)/swc_i486.o | grep ' U '; exit 1; fi
	@echo "i486 object: no undefined symbols"
	@nm $(RULES_I486) | awk '$$1 == "U" { print $$2 }' | sort -u > $(OUT)/rules_u.txt
	@nm $(RULES_I486) | awk 'NF == 3 && $$2 ~ /[TDBR]/ { print $$3 }' | sort -u > $(OUT)/rules_d.txt
	@if [ -n "$$(comm -23 $(OUT)/rules_u.txt $(OUT)/rules_d.txt)" ]; then echo "the rules' i486 objects need symbols from outside:"; comm -23 $(OUT)/rules_u.txt $(OUT)/rules_d.txt; exit 1; fi
	@echo "rules i486 objects: nothing from outside (no libc, no memcpy / memset)"
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/spec_check tests/rules/specs/*.spec
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/team_order
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/groups_guard
	@if grep -n '(const Swc[A-Za-z]* \*)\|(const uint16_t \*)\|(const uint32_t \*)' format/*.c; then echo "the reader reads the spec buffer through a cast (big-endian rule: BE accessors only)"; exit 1; fi
	@echo "reader: no casts over the spec buffer (BE accessors only)"

# the rules library against the Mod's own C (MOD=<a swos-9697-mod checkout>; skipped without)
rules-check:
	CC=$(CC) sh tests/rules/check.sh $(SEASONS)

clean:
	rm -rf $(OUT)

.PHONY: all test clean rules-check
