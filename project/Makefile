# tetriSH build system.
# A single `make` from a clean checkout builds all libraries and binaries.
# Object files (.o) and header-dependency files (.d) live under build/,
# mirroring the src/ tree.

CC       := gcc
# -pthread: tetrisd now has the logshipper thread (and the test harnesses
# spawn threads too). It is both a compile flag (thread-safe libc) and a link
# flag (libpthread), so it lives in CFLAGS which we use for both steps.
CFLAGS   := -Wall -Wextra -g -pthread -I include
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
COMMON_OBJ := $(BUILDDIR)/common/rc.o
DAEMON_OBJ := $(BUILDDIR)/common/daemon.o
RING_OBJ   := $(BUILDDIR)/tetrisd/ring.o

# --- Binaries ---
BIN_NAMES := tetrish tetrisd tetrislogd tetrisctl tetrisu tetrish-view
BINS      := $(addprefix $(BINDIR)/,$(BIN_NAMES))

# --- Test harnesses (built by `make test`) ---
TEST_BINS := $(BUILDDIR)/tests/test_ring $(BUILDDIR)/tests/stress_ring
TEST_OBJ  := $(BUILDDIR)/tests/test_ring.o $(BUILDDIR)/tests/stress_ring.o

# Every object we compile, so we can pull in its generated .d dependency file.
BIN_OBJ := $(foreach n,$(BIN_NAMES),$(BUILDDIR)/$(n)/main.o)
ALL_OBJ := $(SSH_OBJ) $(HTTTP_OBJ) $(BRAIN_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) $(RING_OBJ) $(BIN_OBJ) $(TEST_OBJ)

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
$(BINDIR)/tetrisd: $(BUILDDIR)/tetrisd/main.o $(RING_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ -lrt

$(BINDIR)/tetrislogd: $(BUILDDIR)/tetrislogd/main.o $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisctl: $(BUILDDIR)/tetrisctl/main.o $(COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisu: $(BUILDDIR)/tetrisu/main.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

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

test: $(TEST_BINS)
	$(BUILDDIR)/tests/test_ring
	@echo ""
	@echo "stress harness built: $(BUILDDIR)/tests/stress_ring"
	@echo "  1. start tetrislogd:  bin/tetrislogd <rc-file>"
	@echo "  2. blast it:          $(BUILDDIR)/tests/stress_ring <log_ipc path> [n_records]"
	@echo "  3. compare its 'delivered' number against: grep -c STRESS <log_path>"

clean:
	rm -rf $(BUILDDIR)
	rm -f $(BINS) $(LIBS)

# Pull in the auto-generated header dependencies (build/**/*.d) when they exist.
# The leading '-' means "don't error if a .d is missing" (e.g. on the first build).
-include $(ALL_OBJ:.o=.d)
