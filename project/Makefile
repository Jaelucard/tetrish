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
# (Literal path, not $(SRCDIR): CFLAGS uses := which expands IMMEDIATELY,
# and SRCDIR is defined further down - $(SRCDIR) here would be empty.)
CFLAGS   := -Wall -Wextra -g -pthread -I include -I src/libtetrisbrain
# OpenSSL: libtetrissh uses the EVP API, so every binary that links it needs
# libssl/libcrypto at the END of the link line (static .a resolution order).
SSLLIBS  := -lssl -lcrypto
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

# libtetrissh is now two objects: the standardised corestack implementation
# (shared verbatim with the 50.003 mini-gh-tracker app -- real handshake
# crypto, no OpenSSL SSL_*/TLS) plus a thin shim exposing it under
# tetrisd/tetrisu/tetrisctl's original tetrissh_* names.
SSH_OBJ   := $(BUILDDIR)/corestack/secure_session.o $(BUILDDIR)/libtetrissh/tetrissh.o
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
               $(BUILDDIR)/tetrisd/clients.o $(BUILDDIR)/tetrisd/rooms.o \
               $(BUILDDIR)/tetrisd/admin.o

# --- Binaries ---
BIN_NAMES := tetrish tetrisd tetrislogd tetrisctl tetrisu tetrish-view
BINS      := $(addprefix $(BINDIR)/,$(BIN_NAMES))

# --- Test harnesses (built by `make test`) ---
TEST_BINS := $(BUILDDIR)/tests/test_ring $(BUILDDIR)/tests/stress_ring \
             $(BUILDDIR)/tests/stub_client $(BUILDDIR)/tests/test_handshake \
             $(BUILDDIR)/tests/fuzz_malformed $(BUILDDIR)/tests/test_admin \
             $(BUILDDIR)/tests/fuzz_week12
TEST_OBJ  := $(BUILDDIR)/tests/test_ring.o $(BUILDDIR)/tests/stress_ring.o \
             $(BUILDDIR)/tests/stub_client.o $(BUILDDIR)/tests/test_handshake.o \
             $(BUILDDIR)/tests/fuzz_malformed.o $(BUILDDIR)/tests/test_admin.o \
             $(BUILDDIR)/tests/fuzz_week12.o

# Every object we compile, so we can pull in its generated .d dependency file.
BIN_OBJ := $(foreach n,$(BIN_NAMES),$(BUILDDIR)/$(n)/main.o)
ALL_OBJ := $(SSH_OBJ) $(HTTTP_OBJ) $(BRAIN_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) $(TETRISD_OBJ) $(BIN_OBJ) $(TEST_OBJ)

.PHONY: all libs bins test clean

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
$(BINDIR)/tetrisd: $(TETRISD_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS) -lrt

$(BINDIR)/tetrislogd: $(BUILDDIR)/tetrislogd/main.o $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

# tetrisctl speaks HTTTP now (Week 6), so it links libhtttp too.
$(BINDIR)/tetrisctl: $(BUILDDIR)/tetrisctl/main.o $(COMMON_OBJ) $(LIB_HTTTP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisu: $(BUILDDIR)/tetrisu/main.o $(COMMON_OBJ) $(LIB_SSH) $(LIB_HTTTP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

$(BINDIR)/tetrish-view: $(BUILDDIR)/tetrish-view/main.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

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

# The Week-5 deliverable: socketpair() handshake harness (happy path + the
# three required failure paths), both ends of the handshake in one process.
$(BUILDDIR)/tests/test_handshake: $(BUILDDIR)/tests/test_handshake.o $(LIB_SSH)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

# Week-8 deliverable: fuzz a LIVE tetrisd with malformed input, confirm no crash.
$(BUILDDIR)/tests/fuzz_malformed: $(BUILDDIR)/tests/fuzz_malformed.o $(COMMON_OBJ) $(LIB_SSH)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

# Week-9/10 deliverable: admin protocol role gating + concurrent ops consoles.
$(BUILDDIR)/tests/test_admin: $(BUILDDIR)/tests/test_admin.o $(COMMON_OBJ) $(LIB_SSH) $(LIB_HTTTP)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

# Week-12 deliverable: broader malformed-HTTTP fuzzing against a live tetrisd.
$(BUILDDIR)/tests/fuzz_week12: $(BUILDDIR)/tests/fuzz_week12.o $(COMMON_OBJ) $(LIB_SSH)
	$(CC) $(CFLAGS) -o $@ $^ $(SSLLIBS)

test: $(TEST_BINS)
	$(BUILDDIR)/tests/test_ring
	@echo ""
	@echo "handshake harness built: $(BUILDDIR)/tests/test_handshake"
	@echo "  run: $(BUILDDIR)/tests/test_handshake"
	@echo "  under valgrind: valgrind --leak-check=full --show-leak-kinds=all $(BUILDDIR)/tests/test_handshake"
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
