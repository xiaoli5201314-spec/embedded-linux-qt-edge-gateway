/*
 * test_rs485_port.c - host tests for the poll() based RS485 port.
 *
 * The tests drive the port over a socketpair, which exercises the same code
 * path that the target uses on a real tty (write + poll + drain) without
 * needing any hardware. The DE line is observed through the weak rs485_de_set()
 * hook, which this file overrides to record the call sequence.
 */
#include "rs485_port.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- override the weak DE hook so we can observe the line --------------- */
static int g_de_events[32];
static int g_de_count = 0;

void rs485_de_set(int gpio, int level)
{
    (void)gpio;
    if (g_de_count < (int)(sizeof(g_de_events) / sizeof(g_de_events[0]))) {
        g_de_events[g_de_count++] = level;
    }
}

/* ---- tiny assertion helpers -------------------------------------------- */
static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failed++;                                                      \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define CHECK_EQ_I(a, b)                                                     \
    do {                                                                     \
        long _va = (long)(a), _vb = (long)(b);                               \
        g_checks++;                                                          \
        if (_va != _vb) {                                                    \
            g_failed++;                                                      \
            printf("  [FAIL] %s:%d  %s=%ld expected %s=%ld\n",               \
                   __FILE__, __LINE__, #a, _va, #b, _vb);                    \
        }                                                                    \
    } while (0)

/* Exposed so the aggregate test runner can fold these counters into its own
 * summary and keep a single exit status for the whole suite. */
int rs485_checks(void) { return g_checks; }
int rs485_failed(void) { return g_failed; }

int run_rs485_tests(void)
{
    int          sv[2];
    rs485_config_t cfg;
    rs485_port_t *port;
    uint8_t      tx[6] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    uint8_t      rx[32];

    printf("\n== rs485_port ==\n");

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [SKIP] socketpair unavailable\n");
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.device      = "/dev/null";   /* not used: we inject the fd */
    cfg.baudrate    = 115200;
    cfg.data_bits   = 8;
    cfg.stop_bits   = 1;
    cfg.parity      = 0;
    cfg.de_gpio     = 7;
    cfg.de_guard_us = 0;

    port = rs485_open_fd(sv[0], &cfg);
    CHECK(port != 0);
    if (port == 0) {
        close(sv[0]);
        close(sv[1]);
        return 1;
    }

    /* --- write asserts DE, completes, then releases it ------------------ */
    g_de_count = 0;
    CHECK_EQ_I(rs485_write(port, tx, sizeof(tx)), (int)sizeof(tx));
    CHECK_EQ_I(g_de_count, 1);              /* DE asserted exactly once */
    CHECK_EQ_I(g_de_events[0], 1);

    CHECK_EQ_I(rs485_tx_complete(port, 1000u), 1);
    CHECK_EQ_I(g_de_count, 2);              /* and released once */
    CHECK_EQ_I(g_de_events[1], 0);

    /* The peer must have received exactly the bytes we sent, in order. */
    {
        ssize_t n = read(sv[1], rx, sizeof(rx));
        CHECK_EQ_I(n, (int)sizeof(tx));
        CHECK(memcmp(rx, tx, sizeof(tx)) == 0);
    }

    /* --- second write re-asserts DE (idempotent bookkeeping) ------------ */
    CHECK_EQ_I(rs485_write(port, tx, 3u), 3);
    CHECK_EQ_I(rs485_tx_complete(port, 1000u), 1);
    {
        ssize_t n = read(sv[1], rx, sizeof(rx));
        CHECK_EQ_I(n, 3);
    }

    /* --- read with a short timeout reports "nothing available" ---------- */
    CHECK_EQ_I(rs485_read(port, rx, sizeof(rx), 50u), 0);

    /* --- data arriving from the peer is returned ------------------------ */
    {
        uint8_t peer[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        ssize_t wrote = write(sv[1], peer, sizeof(peer));
        CHECK_EQ_I(wrote, (int)sizeof(peer));
        {
            int n = rs485_read(port, rx, sizeof(rx), 500u);
            CHECK_EQ_I(n, 4);
            CHECK(memcmp(rx, peer, 4) == 0);
        }
    }

    /* --- reset_queues releases DE if it was left asserted --------------- */
    (void)rs485_write(port, tx, 1u);        /* asserts DE, never completed */
    g_de_count = 0;
    rs485_reset_queues(port);
    CHECK(g_de_count >= 1);
    CHECK_EQ_I(g_de_events[g_de_count - 1], 0);

    /* --- arguments are validated --------------------------------------- */
    CHECK_EQ_I(rs485_write(port, 0, 4u), -1);
    CHECK_EQ_I(rs485_write(port, tx, 0u), -1);
    CHECK_EQ_I(rs485_read(port, 0, 4u, 10u), -1);

    rs485_close(port);
    close(sv[1]);

    /* Closing a port that still owns its fd must not leak or crash; the
     * invalid-fd path returns NULL instead of a half built port. */
    CHECK(rs485_open_fd(-1, &cfg) == 0);
    CHECK(rs485_open(0) == 0);

    return 0;
}
