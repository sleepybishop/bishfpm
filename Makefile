CC ?= cc
AR ?= ar
RM ?= rm -f

override CPPFLAGS += -Iinclude -I. -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -MMD -MP
CFLAGS ?= -O3 -g -std=c11 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -lm

CORE_OBJ = \
	lib/arena.o \
	lib/rate.o \
	lib/itemset.o \
	lib/topk.o \
	lib/pairs.o \
	lib/compact.o \
	lib/bishfpm.o

TEST_UTILS = \
	t/00util/test_rate \
	t/00util/test_topk \
	t/00util/test_cooldown \
	t/00util/test_itemset \
	t/00util/test_engine \
	t/00util/test_multi_group \
	t/00util/test_weighted \
	t/00util/test_suppression \
	t/00util/test_invariants \
	t/00util/test_regressions \
	t/00util/test_lowlevel \
	t/00util/test_pairs \
	t/00util/test_compact_exact \
	t/00util/test_online \
	t/00util/test_fss \
	t/00util/test_rejections \
	t/00util/test_general_fss \
	t/00util/test_dynamic \
	t/00util/test_pressure

BENCH = \
	t/00util/bench \
	t/00util/bench_stores \
	t/00util/bench_cooldown \
	t/00util/bench_general

EXAMPLES = \
	examples/stream_tsv

all: libraries $(TEST_UTILS) $(BENCH) $(EXAMPLES)

libraries: libbishfpm.a libbishfpm.so

ci: all
	$(MAKE) check
	$(MAKE) check-installed

libbishfpm.a: $(CORE_OBJ)
	$(AR) rcs $@ $^

lib/%.o: lib/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Assertions execute test and benchmark checks, even with release library flags.
t/00util/test_rejections: t/00util/test_rejections.c libbishfpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< libbishfpm.a $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=strdup $(LDLIBS) -UNDEBUG -o $@

t/00util/test_dynamic: t/00util/test_dynamic.c libbishfpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< libbishfpm.a $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=strdup,--wrap=free $(LDLIBS) -UNDEBUG -o $@

t/00util/test_pressure: t/00util/test_pressure.c libbishfpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< libbishfpm.a $(LDFLAGS) -Wl,--wrap=calloc $(LDLIBS) -UNDEBUG -o $@

t/00util/%: t/00util/%.c libbishfpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< libbishfpm.a $(LDFLAGS) $(LDLIBS) -UNDEBUG -o $@

examples/%: examples/%.c libbishfpm.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< libbishfpm.a $(LDFLAGS) $(LDLIBS) -o $@

-include $(CORE_OBJ:.o=.d)
-include $(patsubst %,%.d,$(TEST_UTILS))
-include $(patsubst %,%.d,$(BENCH))
-include $(patsubst %,%.d,$(EXAMPLES))

test: $(TEST_UTILS) $(EXAMPLES) t/00util/bench_general
	prove -I. -v t/*.t

check: test

bench: $(BENCH)
	@for b in $(BENCH); do echo "Running $$b..."; ./$$b || exit 1; done

scan:
	$(MAKE) clean
	scan-build --status-bugs $(MAKE) all

valgrind: $(TEST_UTILS) $(EXAMPLES)
	for t in $(TEST_UTILS) $(EXAMPLES); do \
		valgrind --leak-check=full --error-exitcode=1 ./$$t || exit 1; \
	done

indent:
	clang-format -style=LLVM -i include/*.h lib/*.c t/00util/*.c examples/*.c

clean:
	$(RM) lib/*.o lib/*.d t/00util/*.o t/00util/*.d examples/*.o examples/*.d *.o *.a *.d libbishfpm.so* bishfpm.pc $(TEST_UTILS) $(BENCH) $(EXAMPLES)

.PHONY: all test check bench scan indent clean

PREFIX ?= /usr/local
DESTDIR ?=
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
VERSION := $(shell sed -n 's/^\#define BISHFPM_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$$/\2/p' include/bishfpm_version.h | paste -sd .)
SOVERSION = 0.2
PIC_OBJ = $(CORE_OBJ:.o=.pic.o)

lib/%.pic.o: lib/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

libbishfpm.so.$(VERSION): $(PIC_OBJ)
	$(CC) -shared $(LDFLAGS) -Wl,-soname,libbishfpm.so.$(SOVERSION) $^ $(LDLIBS) -o $@

libbishfpm.so: libbishfpm.so.$(VERSION)
	ln -sf libbishfpm.so.$(VERSION) libbishfpm.so.$(SOVERSION)
	ln -sf libbishfpm.so.$(SOVERSION) $@

bishfpm.pc: Makefile include/bishfpm_version.h FORCE
	@{ echo 'prefix=$(PREFIX)'; echo 'libdir=$(LIBDIR)'; echo 'includedir=$(INCLUDEDIR)'; \
	   echo 'Name: bishfpm'; echo 'Description: Decayed heuristic itemset concentration'; \
	   echo 'Version: $(VERSION)'; echo 'Libs: -L$${libdir} -lbishfpm'; \
	   echo 'Libs.private: -lm'; echo 'Cflags: -I$${includedir}'; } > $@

install: libbishfpm.a libbishfpm.so bishfpm.pc
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(INCLUDEDIR)
	install -m 644 include/*.h $(DESTDIR)$(INCLUDEDIR)/
	install -m 644 libbishfpm.a libbishfpm.so.$(VERSION) $(DESTDIR)$(LIBDIR)/
	ln -sf libbishfpm.so.$(VERSION) $(DESTDIR)$(LIBDIR)/libbishfpm.so.$(SOVERSION)
	ln -sf libbishfpm.so.$(SOVERSION) $(DESTDIR)$(LIBDIR)/libbishfpm.so
	install -m 644 bishfpm.pc $(DESTDIR)$(LIBDIR)/pkgconfig/

sanitize:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined,float-cast-overflow' check

build/fuzz_stream: t/fuzz_stream.c $(wildcard lib/*.c) $(wildcard include/*.h) $(wildcard lib/*.h)
	mkdir -p build
	clang $(CPPFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined,float-cast-overflow -fno-omit-frame-pointer t/fuzz_stream.c lib/*.c -lm -UNDEBUG -o $@

fuzz-smoke: build/fuzz_stream
	mkdir -p build/corpus
	./build/fuzz_stream -runs=10000 -max_len=4096 -artifact_prefix=build/ build/corpus

-include $(PIC_OBJ:.o=.d)
.PHONY: install sanitize fuzz-smoke valgrind FORCE

# Install in a disposable staging prefix, then exercise both supported languages
# and linkage modes. This target never touches the system installation.
check-installed: libraries bishfpm.pc
	sh scripts/check-installed.sh "$(SOVERSION)"

.PHONY: libraries ci check-installed
