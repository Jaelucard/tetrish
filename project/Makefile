# tetriSH build system.
# A single `make` from a clean checkout builds all libraries and binaries.
# Object files (.o) and header-dependency files (.d) live under build/,
# mirroring the src/ tree.

CC       := gcc
# -pthread: tetrisd now has the logshipper thread (and the test harnesses
# spawn threads too). It is both a compile flag (thread-safe libc) and a link
# flag (libpthread), so it lives in CFLAGS which we use for both steps.
# -I src/libtetrisbrain: tetrisbrain.h lives next to its .c (APP teammate's
# layout) rather than in include/; tetrisd's rooms module includes it.
# -I src/tetrish-view: same reasoning, for repl.h/control.h/net.h-style
# sibling headers -- specifically so tests/test_view_repl.c (a different
# directory) can quote-include "repl.h". Files inside src/tetrish-view/
# itself don't need this: quote-includes always check the including file's
# own directory first, so main.c/repl.c/dispatch.c already find each
# other's headers without it.
# -I src/tetrisu: src/tetrish-view/attach.c (Week 11) reuses tetrisu's
# net.h/net.c wire adapter (JOIN/START/STATE/LEAVE) rather than duplicating
# it, so it needs to reach a header that lives in a sibling src/ directory.
# (Literal paths, not $(SRCDIR): CFLAGS uses := which expands IMMEDIATELY,
# and SRCDIR is defined further down - $(SRCDIR) here would be empty.)
CFLAGS   := -D_GNU_SOURCE -Wall -Wextra -g -pthread -I include -I src/libtetrisbrain -I src/tetrish-view -I src/tetrisu
# OpenSSL: libtetrissh uses the EVP API, so every binary that links it needs
# libssl/libcrypto at the END of the link line (static .a resolution order).
SSLLIBS  := -lssl -lcrypto
# -lrt: POSIX message queues traditionally live in librt on Linux. Darwin has
# no librt at all (the mq shim below stands in for it), so it is a variable.
RTLIBS   := -lrt
# COMPAT_OBJ is empty on Linux; the Darwin block below fills it in.
COMPAT_OBJ :=

# --- macOS ------------------------------------------------------------------
# The tree is written against Linux kernel APIs (signalfd, epoll, timerfd,
# POSIX mqueues). On Darwin those are emulated by src/compat/macos.c over
# kqueue/pipes/AF_UNIX datagrams, and src/compat/include/ shadows the Linux
# headers so the sources build unmodified. On Linux none of this is in play:
# the -I is absent, COMPAT_OBJ stays empty, and the kernel headers win.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  # Apple ships LibreSSL headers-free; use Homebrew's OpenSSL 3.
  OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
  ifeq ($(OPENSSL_PREFIX),)
    $(error building on macOS needs Homebrew's openssl@3: brew install openssl@3)
  endif
  CFLAGS  += -I src/compat/include -I $(OPENSSL_PREFIX)/include
  SSLLIBS := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  RTLIBS  :=
  # Literal path, not $(BUILDDIR): same immediate-expansion trap as the -I
  # paths in CFLAGS above -- BUILDDIR is defined further down.
  COMPAT_OBJ := build/compat/macos.o
endif
# Header dependency generation, applied when compiling each .o:
#   -MMD  emit a .d file next to the .o listing the headers that .c #includes
#   -MP   also emit an empty phony target per header, so DELETING a header
#         later won't break the build with a "no rule to make target" error
DEPFLAGS := -MMD -MP
AR       := ar
ARFLAGS  := rcs

SRCDIR   := src
BUILDDIR := build
BINDIR   := bin
LIBDIR   := lib

# --- Static libraries and the objects they archive ---
LIB_SSH   := $(LIBDIR)/libtetrissh.a
LIB_HTTTP := $(LIBDIR)/libhtttp.a
LIB_BRAIN := $(LIBDIR)/libtetrisbrain.a
LIBS      := $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)

SSH_OBJ   := $(BUILDDIR)/libtetrissh/tetrissh.o
HTTTP_OBJ := $(BUILDDIR)/libhtttp/htttp.o
BRAIN_OBJ := $(BUILDDIR)/libtetrisbrain/tetrisbrain.o

# --- Shared objects linked into the binaries ---
# rc.o     : the .tetrishrc parser (tetrish, tetrisd, tetrislogd, tetrisctl)
# daemon.o : the double-fork helper (tetrisd, tetrislogd)
# ring.o   : the log ring buffer (tetrisd + the ring test harnesses)
# clients.o/rooms.o : tetrisd's per-connection + room state (Week 6)
COMMON_OBJ  := $(BUILDDIR)/common/rc.o
DAEMON_OBJ  := $(BUILDDIR)/common/daemon.o
RING_OBJ    := $(BUILDDIR)/tetrisd/ring.o
TETRISD_OBJ := $(BUILDDIR)/tetrisd/main.o $(RING_OBJ) \
               $(BUILDDIR)/tetrisd/clients.o $(BUILDDIR)/tetrisd/rooms.o
# tetrisu's and tetrish-view's objects, named like TETRISD_OBJ above: the
# link rules use these, and ALL_OBJ pulls in their generated .d files. an
# object left out of ALL_OBJ never has its header dependencies loaded, so
# editing a header it includes would NOT rebuild it -- an incremental make
# would silently link stale code.
TETRISU_OBJ := $(BUILDDIR)/tetrisu/main.o $(BUILDDIR)/tetrisu/net.o \
               $(BUILDDIR)/tetrisu/tetrisu.o
VIEW_OBJ    := $(BUILDDIR)/tetrish-view/main.o $(BUILDDIR)/tetrish-view/control.o \
               $(BUILDDIR)/tetrish-view/repl.o $(BUILDDIR)/tetrish-view/dispatch.o \
               $(BUILDDIR)/tetrish-view/jsonish.o $(BUILDDIR)/tetrish-view/edit.o \
               $(BUILDDIR)/tetrish-view/attach.o $(BUILDDIR)/tetrish-view/replay.o

# --- Binaries ---
BIN_NAMES := tetrish tetrisd tetrislogd tetrisctl tetrisu tetrish-view
BINS      := $(addprefix $(BINDIR)/,$(BIN_NAMES))

# --- Test harnesses (built by `make test`) ---
TEST_BINS := $(BUILDDIR)/tests/test_ring $(BUILDDIR)/tests/stress_ring \
             $(BUILDDIR)/tests/stub_client $(BUILDDIR)/tests/test_garbage \
             $(BUILDDIR)/tests/garbage_send $(BUILDDIR)/tests/test_htttp \
             $(BUILDDIR)/tests/fuzz_htttp $(BUILDDIR)/tests/test_control
TEST_OBJ  := $(BUILDDIR)/tests/test_ring.o $(BUILDDIR)/tests/stress_ring.o \
             $(BUILDDIR)/tests/stub_client.o $(BUILDDIR)/tests/test_garbage.o \
             $(BUILDDIR)/tests/garbage_send.o $(BUILDDIR)/tests/test_htttp.o \
             $(BUILDDIR)/tests/fuzz_htttp.o

# Sanitized test harnesses: source-to-binary in one step under asan/ubsan
# (no object files, no archive, so the sanitizer instruments everything),
# per the CLAUDE.md rule that owned unit tests build this way. Lives in its
# own build dir so plain (unsanitized) objects never mix with sanitized ones
# for the same source file.
#
# Auto-discovered: every tests/test_brain*.c and tests/test_view*.c gets a
# binary with no Makefile edit needed. A test that needs a companion source
# beyond its own .c file (e.g. test_brain needs the brain itself; a future
# ops-console test might need src/tetrish-view/repl.c) declares it as
# SAN_EXTRA_SRC_<stem> below; the pattern rule under "Tests" looks that up by
# stem, and an undefined one (the common case for a pure, self-contained
# test) is simply empty.
SANFLAGS      := -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_TEST_SRCS := $(wildcard tests/test_brain*.c) $(wildcard tests/test_view*.c)
SAN_TEST_BINS := $(patsubst tests/%.c,$(BUILDDIR)/tests-san/%,$(SAN_TEST_SRCS))

SAN_EXTRA_SRC_test_brain := src/libtetrisbrain/tetrisbrain.c
# dispatch.c only, deliberately not repl.c/control.c: this test is scoped to
# pure parsing/lookup, so it must not need libhtttp to build. See
# src/tetrish-view/dispatch.c's header comment.
SAN_EXTRA_SRC_test_view_repl := src/tetrish-view/dispatch.c
# jsonish.c and edit.c are both pure text/state processing with zero
# libhtttp/libtetrissh dependency (see their own header comments), so they
# build in the sanitized tier the same way dispatch.c does.
SAN_EXTRA_SRC_test_view_json := src/tetrish-view/jsonish.c
SAN_EXTRA_SRC_test_view_edit := src/tetrish-view/edit.c

# Every object we compile, so we can pull in its generated .d dependency file.
BIN_OBJ := $(foreach n,$(BIN_NAMES),$(BUILDDIR)/$(n)/main.o)
ALL_OBJ := $(SSH_OBJ) $(HTTTP_OBJ) $(BRAIN_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) \
           $(TETRISD_OBJ) $(TETRISU_OBJ) $(VIEW_OBJ) $(BIN_OBJ) $(TEST_OBJ) \
           $(COMPAT_OBJ)

.PHONY: all libs bins test test-san clean

all: libs bins

libs: $(LIBS)

bins: $(BINS)

# --- Generic compile rule: src/X/Y.c -> build/X/Y.o (+ build/X/Y.d) ---
# $(@D) is the target's directory; mkdir -p makes it (and build/) on demand.
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# --- Archive each library from its object ---
$(LIB_SSH): $(SSH_OBJ)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(LIB_HTTTP): $(HTTTP_OBJ)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(LIB_BRAIN): $(BRAIN_OBJ)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

# --- Link each binary ---
# Config consumers link rc.o; the two daemons also link daemon.o; the game
# clients (tetrisu, tetrish-view) link neither.
$(BINDIR)/tetrish: $(BUILDDIR)/tetrish/main.o $(COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

# -lrt: POSIX message queues (mq_open & friends) traditionally live in librt.
# Modern glibc has them in libc proper, but linking librt is harmless and
# keeps the build portable to older toolchains.
# tetrisd now links all three team libraries: tetrissh (session), htttp
# (protocol), tetrisbrain (game logic) - plus OpenSSL for tetrissh.
$(BINDIR)/tetrisd: $(TETRISD_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) $(COMPAT_OBJ) $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS) $(RTLIBS)

$(BINDIR)/tetrislogd: $(BUILDDIR)/tetrislogd/main.o $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

# tetrisctl speaks HTTTP now (Week 6), so it links libhtttp too.
$(BINDIR)/tetrisctl: $(BUILDDIR)/tetrisctl/main.o $(COMMON_OBJ) $(LIB_HTTTP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

# tetrisu links the session layer, the protocol and ncurses for its normal,
# server-authoritative path (the client holds no game state there and cannot
# disagree with the server) -- that wire protocol code lives in net.o
# (net.c/net.h), a thin adapter so protocol churn stays out of main.c's
# render code. It also links libtetrisbrain now, for the --local path only
# (tetrisu.c / local.h): a full offline game against an in-process tb_game,
# no sockets, kept behind a flag so the default path is unaffected.
$(BINDIR)/tetrisu: $(TETRISU_OBJ) $(COMMON_OBJ) $(COMPAT_OBJ) $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS) -lncurses

# tetrish-view replays a recorded session (replay.o: reconstructs boards by
# calling tb_tick, the same entry point tetrisd uses) and is the ops console
# (Week 9): control.o speaks tetrisd's control plane (plain HTTTP over a
# local AF_UNIX socket, same as tetrisctl); jsonish.o scans its JSON
# (rooms/players/kick, Week 10); edit.o is the hand-rolled line editor
# backing the repl's raw-mode history/editing (Week 10). Week 11 adds
# attach.o + net.o (borrowed from src/tetrisu, see the CFLAGS comment above)
# for `attach <room>`, which speaks the GAME plane (tetrissh + OpenSSL) to
# stream STATE frames as an ordinary late-joined participant -- unlike
# everything else here, that is why this binary now needs libtetrissh and
# OpenSSL too, on top of libhtttp/libtetrisbrain/ncurses.
$(BINDIR)/tetrish-view: $(VIEW_OBJ) $(BUILDDIR)/tetrisu/net.o $(COMMON_OBJ) $(COMPAT_OBJ) $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS) -lncurses

# --- Tests ---
# Test sources live in tests/ (not src/), so they get their own compile rule.
# -I src/tetrisd lets them include ring.h.
$(BUILDDIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I $(SRCDIR)/tetrisd -c $< -o $@

$(BUILDDIR)/tests/test_ring: $(BUILDDIR)/tests/test_ring.o $(RING_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/tests/stress_ring: $(BUILDDIR)/tests/stress_ring.o $(RING_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# The Week-6 integration harness: full handshake + JOIN/START/STATE/MOVE/LEAVE.
$(BUILDDIR)/tests/stub_client: $(BUILDDIR)/tests/stub_client.o $(COMMON_OBJ) $(LIB_SSH) $(LIB_HTTTP)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

# Week-9 Battle Royale. test_garbage is pure logic against libtetrisbrain;
# garbage_send posts a real event into tetrisd's queue (-lrt for mq_*).
$(BUILDDIR)/tests/test_garbage: $(BUILDDIR)/tests/test_garbage.o $(LIB_BRAIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/tests/garbage_send: $(BUILDDIR)/tests/garbage_send.o $(COMMON_OBJ) $(COMPAT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(RTLIBS)

# Edge-case tests for the protocol library. Pure parsing, no sockets.
$(BUILDDIR)/tests/test_htttp: $(BUILDDIR)/tests/test_htttp.o $(LIB_HTTTP)
	$(CC) $(CFLAGS) -o $@ $^

# Malformed-input fuzzer for the parser. -D_GNU_SOURCE for memmem().
$(BUILDDIR)/tests/fuzz_htttp: $(BUILDDIR)/tests/fuzz_htttp.o $(LIB_HTTTP)
	$(CC) $(CFLAGS) -o $@ $^

# Week 9 ops console: control_get() (src/tetrish-view/control.c) tested end
# to end -- request bytes sent, response parsed -- against a mock
# control-plane socket (tests/mocks/mock_control.c, a test-only mock; see
# its header comment). Needs libhtttp, like test_htttp/stub_client above, so
# unlike test_view_repl this is not part of the sanitized test-san tier.
$(BUILDDIR)/tests/test_control: tests/test_control.c tests/mocks/mock_control.c src/tetrish-view/control.c $(LIB_HTTTP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ tests/test_control.c tests/mocks/mock_control.c src/tetrish-view/control.c $(LIB_HTTTP)

# Sanitized tests (auto-discovered above): $* is the stem (e.g. test_brain),
# so SAN_EXTRA_SRC_$* pulls in that test's companion source, if any.
$(BUILDDIR)/tests-san/%: tests/%.c $(SAN_EXTRA_SRC_$*)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ $< $(SAN_EXTRA_SRC_$*)

# Sanitized owned tests only, independent of libtetrissh/libhtttp -- this is
# what tests/smoke.sh runs, so it can verify the owned tree even when a
# teammate path doesn't currently build (see REQUIRED_FILES.md).
test-san: $(SAN_TEST_BINS)
	@for t in $(SAN_TEST_BINS); do $$t || exit 1; echo ""; done

test: $(TEST_BINS) test-san
	$(BUILDDIR)/tests/test_ring
	@echo ""
	$(BUILDDIR)/tests/test_garbage
	@echo ""
	$(BUILDDIR)/tests/test_htttp
	@echo ""
	$(BUILDDIR)/tests/fuzz_htttp 20000
	@echo ""
	$(BUILDDIR)/tests/test_control
	@echo ""
	@echo "stress harness built: $(BUILDDIR)/tests/stress_ring"
	@echo "  1. start tetrislogd:  bin/tetrislogd <rc-file>"
	@echo "  2. blast it:          $(BUILDDIR)/tests/stress_ring <log_ipc path> [n_records]"
	@echo "  3. compare its 'delivered' number against: grep -c STRESS <log_path>"
	@echo ""
	@echo "integration harness built: $(BUILDDIR)/tests/stub_client"
	@echo "  1. generate keys once: bash auth/generate_keys.sh"
	@echo "  2. start tetrislogd + tetrisd with the same rc file"
	@echo "  3. run: $(BUILDDIR)/tests/stub_client <rc-file> [room] [seconds]"

clean:
	rm -rf $(BUILDDIR)
	rm -f $(BINS) $(LIBS)

# Pull in the auto-generated header dependencies (build/**/*.d) when they exist.
# The leading '-' means "don't error if a .d is missing" (e.g. on the first build).
-include $(ALL_OBJ:.o=.d)
