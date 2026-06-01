CC      = gcc # C compiler to use
CFLAGS  = -Wall -Wextra -g -I include # warnings, debug symbols and include path
AR      = ar # Archiver for static Libraries
ARFLAGS = rcs # r=insert, c=create, s=index

BINDIR = bin
LIBDIR = lib
SRCDIR = src

LIB_SSH   = $(LIBDIR)/libtetrissh.a
LIB_HTTTP = $(LIBDIR)/libhtttp.a
LIB_BRAIN = $(LIBDIR)/libtetrisbrain.a
LIBS = $(LIB_SSH) $(LIB_HTTTP) $(LIB_BRAIN)

BINS = $(BINDIR)/tetrish \
       $(BINDIR)/tetrisd \
       $(BINDIR)/tetrislogd \
       $(BINDIR)/tetrisctl \
       $(BINDIR)/tetrisu \
       $(BINDIR)/tetrish-view

.PHONY: all libs bins test clean

all: libs bins

libs: $(LIBS)

bins: $(BINS)

# --- Compile Libraries into Static Archives ---
$(LIB_SSH): $(SRCDIR)/libtetrissh/tetrissh.c
	$(CC) $(CFLAGS) -c $< -o $(SRCDIR)/libtetrissh/tetrissh.o
	$(AR) $(ARFLAGS) $@ $(SRCDIR)/libtetrissh/tetrissh.o

$(LIB_HTTTP): $(SRCDIR)/libhtttp/htttp.c
	$(CC) $(CFLAGS) -c $< -o $(SRCDIR)/libhtttp/htttp.o
	$(AR) $(ARFLAGS) $@ $(SRCDIR)/libhtttp/htttp.o

$(LIB_BRAIN): $(SRCDIR)/libtetrisbrain/tetrisbrain.c
	$(CC) $(CFLAGS) -c $< -o $(SRCDIR)/libtetrisbrain/tetrisbrain.o
	$(AR) $(ARFLAGS) $@ $(SRCDIR)/libtetrisbrain/tetrisbrain.o

# --- Binaries ---

$(BINDIR)/tetrish: $(SRCDIR)/tetrish/main.c
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisd: $(SRCDIR)/tetrisd/main.c
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrislogd: $(SRCDIR)/tetrislogd/main.c
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisctl: $(SRCDIR)/tetrisctl/main.c
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrisu: $(SRCDIR)/tetrisu/main.c
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR)/tetrish-view: $(SRCDIR)/tetrish-view/main.c
	$(CC) $(CFLAGS) -o $@ $^

# --- Test and clean ---

test:
	@echo "No tests yet"

clean:
	rm -f $(BINS) $(LIBS)
	find $(SRCDIR) -name "*.o" -delete
