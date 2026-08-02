// fuzz_htttp throws malformed HTTTP at the parser and checks it survives.
//
// The parser sits directly behind the decryption layer, so it is the first
// thing that touches attacker-controlled bytes after they have been decrypted.
// Everything it does must be bounded by the buf_len it was given, and no input
// may make it read past the end, loop forever, or crash.
//
// This is a dumb fuzzer on purpose: a deterministic PRNG mutates a corpus of
// valid messages and feeds the results through both parsers. Determinism
// matters more than cleverness here, because a crash that cannot be reproduced
// is not a bug report. Every run with the same seed produces the same inputs,
// and the seed is printed so a failure can be replayed exactly.
//
// Run it under valgrind to catch the reads that do not happen to segfault:
//   valgrind --error-exitcode=1 ./build/tests/fuzz_htttp 20000
//
// usage: fuzz_htttp [iterations] [seed]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "htttp.h"

// xorshift32, so the sequence is identical on every machine and every run.
// rand() is deliberately avoided: its sequence is implementation-defined, and
// a corpus that differs between machines defeats the point.
static uint32_t rng_state = 2463534242u;
static uint32_t rnd(void){
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static const char *corpus[] = {
    "JOIN /room/demo HTTTP/1.0\r\n\r\n",
    "LEAVE /room/demo HTTTP/1.0\r\nPlayer-Id: p1\r\n\r\n",
    "START /room/demo HTTTP/1.0\r\nPlayer-Id: p1\r\n\r\n",
    "MOVE /room/demo/player/p1 HTTTP/1.0\r\nPlayer-Id: p1\r\n"
        "Content-Type: application/tetris-command\r\nContent-Length: 4\r\n\r\nLEFT",
    "ROTATE /room/demo/player/p1 HTTTP/1.0\r\nPlayer-Id: p1\r\nContent-Length: 2\r\n\r\nCW",
    "DROP /room/demo/player/p1 HTTTP/1.0\r\nPlayer-Id: p1\r\nContent-Length: 4\r\n\r\nHARD",
    "STATE /room/demo HTTTP/1.0\r\nContent-Type: application/tetris-state\r\n"
        "Content-Length: 11\r\nTick: 42\r\n\r\nplayer p1\n.",
    "GET /status HTTTP/1.0\r\n\r\n",
    "HTTTP/1.0 200 OK\r\nContent-Length: 2\r\nDate: Sat, 02 Aug 2026 00:00:00 GMT\r\n\r\nhi",
    "HTTTP/1.0 409 Conflict\r\nContent-Length: 0\r\n\r\n",
};
#define NCORPUS (int)(sizeof corpus / sizeof corpus[0])

#define MAXBUF 4096

// Mutation strategies, each one modelling a different way a message gets
// mangled between a hostile client and our parser.
typedef enum {
    MUT_TRUNCATE,      // a short read, or a client that stopped talking
    MUT_FLIP_BYTE,     // corruption, or a deliberately malformed token
    MUT_INSERT_NUL,    // the classic C string-handling trap
    MUT_DUPLICATE,     // two messages in one buffer, or a repeated header
    MUT_STRIP_CRLF,    // line endings mangled, e.g. by a text-mode transfer
    MUT_HUGE_LENGTH,   // a Content-Length that lies about the body
    MUT_RANDOM_BYTES,  // not HTTTP at all: someone connected with the wrong tool
    MUT_COUNT
} mutation_t;

int main(int argc, char **argv){
    long iters = (argc > 1) ? atol(argv[1]) : 20000;
    if (argc > 2) rng_state = (uint32_t)strtoul(argv[2], NULL, 10);
    if (rng_state == 0) rng_state = 2463534242u;      // xorshift must not be 0
    uint32_t seed = rng_state;

    printf("fuzz_htttp: %ld iterations, seed %u\n", iters, seed);

    long ok = 0, rejected = 0;
    long by_mut[MUT_COUNT];
    memset(by_mut, 0, sizeof by_mut);

    char buf[MAXBUF];
    for (long i = 0; i < iters; i++){
        const char *base = corpus[rnd() % NCORPUS];
        size_t len = strlen(base);
        if (len >= MAXBUF) len = MAXBUF - 1;
        memcpy(buf, base, len);

        mutation_t mut = (mutation_t)(rnd() % MUT_COUNT);
        by_mut[mut]++;

        switch (mut){
        case MUT_TRUNCATE:
            if (len > 1) len = rnd() % len;
            break;
        case MUT_FLIP_BYTE:
            if (len > 0) buf[rnd() % len] = (char)(rnd() & 0xFF);
            break;
        case MUT_INSERT_NUL:
            if (len > 0) buf[rnd() % len] = '\0';
            break;
        case MUT_DUPLICATE:
            if (len * 2 < MAXBUF){ memcpy(buf + len, base, len); len *= 2; }
            break;
        case MUT_STRIP_CRLF:
            for (size_t k = 0; k < len; k++)
                if (buf[k] == '\r') buf[k] = ' ';
            break;
        case MUT_HUGE_LENGTH: {
            // Replace whatever Content-Length is there with an absurd one.
            const char *cl = "Content-Length: ";
            char *p = memmem(buf, len, cl, strlen(cl));
            if (p != NULL){
                size_t off = (size_t)(p - buf) + strlen(cl);
                if (off + 12 < MAXBUF) memcpy(buf + off, "999999999999", 12);
            }
            break;
        }
        case MUT_RANDOM_BYTES:
            len = 1 + (rnd() % 200);
            for (size_t k = 0; k < len; k++) buf[k] = (char)(rnd() & 0xFF);
            break;
        case MUT_COUNT: break;
        }

        // Both parsers, because the client feeds server-originated frames into
        // the request parser and its own replies into the response parser, so
        // both see hostile input from a compromised peer.
        htttp_msg_t m;
        htttp_err_t e1 = htttp_parse_request(buf, len, &m);
        if (e1 == HTTTP_OK){
            ok++;
            // If it claims a body, that body must lie inside the buffer we
            // supplied. A parser that hands back a pointer past the end is
            // exactly the bug this fuzzer exists to find, and the daemon would
            // then read attacker-chosen memory.
            if (m.body != NULL &&
                (m.body < buf || m.body + m.body_len > buf + len)){
                printf("fuzz_htttp: FAIL at iteration %ld (seed %u): "
                       "body escapes the input buffer\n", i, seed);
                return 1;
            }
        } else {
            rejected++;
        }
        htttp_parse_response(buf, len, &m);
    }

    static const char *names[MUT_COUNT] = {
        "truncate", "flip-byte", "insert-NUL", "duplicate",
        "strip-CR", "huge-length", "random-bytes"
    };
    printf("  mutations applied:\n");
    for (int k = 0; k < MUT_COUNT; k++)
        printf("    %-14s %ld\n", names[k], by_mut[k]);
    printf("  parsed as valid: %ld   rejected: %ld\n", ok, rejected);
    printf("fuzz_htttp: SURVIVED %ld iterations without a crash or an escaped body\n",
           iters);
    printf("  (run under valgrind to also catch reads that do not segfault)\n");
    return 0;
}
