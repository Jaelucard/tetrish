# tetriSH build system.
# one `make` from a clean checkout builds every library and binary.
# object files (.o) and header dep files (.d) go under build/, mirroring src/.

CC       := gcc
CFLAGS   := -Wall -Wextra -g -I include
# header dependency tracking, applied when compiling each .o:
#   -MMD  writes a .d file next to the .o listing the headers that .c pulls in
#   -MP   also adds an empty target per header, so DELETING a header later
#         doesn't break the build with a "no rule to make target" error
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
COMMON_OBJ := $(BUILDDIR)/common/rc.o
DAEMON_OBJ := $(BUILDDIR)/common/daemon.o

# --- Binaries ---
BIN_NAMES := tetrish tetrisd tetrislogd tetrisctl tetrisu tetrish-view
BINS      := $(addprefix $(BINDIR)/,$(BIN_NAMES))

# every object we compile, so we can -include its generated .d dep file below.
BIN_OBJ := $(foreach n,$(BIN_NAMES),$(BUILDDIR)/$(n)/main.o)
ALL_OBJ := $(SSH_OBJ) $(HTTTP_OBJ) $(BRAIN_OBJ) $(COMMON_OBJ) $(DAEMON_OBJ) $(BIN_OBJ)

.PHONY: all libs bins test clean

all: libs bins

libs: $(LIBS)

bins: $(BINS)

# --- generic compile rule: src/X/Y.c -> build/X/Y.o (+ build/X/Y.d) ---
# $(@D) is the target's directory. mkdir -p makes it (and build/) on demand.
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

# --- link each binary ---
# config consumers link rc.o. the two daemons also link daemon.o. the game
# clients (tetrisu, tetrish-view) link neither.
$(BINDIR)/tetrish: $(BUILDDIR)/tetrish/main.o $(COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisd: $(BUILDDIR)/tetrisd/main.o $(COMMON_OBJ) $(DAEMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

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

# --- Tests / clean ---
test:
	@echo "No tests yet"

clean:
	rm -rf $(BUILDDIR)
	rm -f $(BINS) $(LIBS)

# pull in the auto-generated header deps (build/**/*.d) when they exist.
# the leading '-' means don't error if a .d is missing, e.g. on the first build.
-include $(ALL_OBJ:.o=.d)
