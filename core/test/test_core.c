/*
 * test_core.c - unit tests for the gateway core library.
 *
 * The tests run on the host with plain C: no framework, no external
 * dependencies. Every assertion is executed and the summary is printed so the
 * output can be pasted straight into the README's measured-results section.
 */
#include "crc16.h"
#include "modbus_rtu.h"
#include "offline_cache.h"
#include "reconnect_fsm.h"
#include "register_map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Implemented in test_rs485_port.c; it keeps its own counters and reports
 * through the shared globals below. */
extern int run_rs485_tests(void);
extern int rs485_checks(void);
extern int rs485_failed(void);

static int g_checks = 0;
static int g_failed = 0;
static const char *g_suite = "";

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

static void suite(const char *name)
{
    g_suite = name;
    printf("\n== %s ==\n", name);
}

/* ------------------------------------------------------------------ CRC16 */
static void test_crc16(void)
{
    /* Reference vector: 01 03 00 00 00 0A -> CRC 0xCDC5 */
    const uint8_t req[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    uint8_t       wire[2];
    uint16_t      crc_tab;
    uint16_t      crc_bit;
    uint8_t       framed[8];
    uint8_t       corrupt[8];

    suite("crc16");

    crc_tab = crc16_modbus(req, sizeof(req));
    crc_bit = crc16_modbus_bitwise(req, sizeof(req));
    CHECK_EQ_I(crc_tab, crc_bit);              /* table and bitwise must agree */

    crc16_to_wire(crc_tab, wire);
    CHECK_EQ_I(crc16_from_wire(wire), crc_tab);/* wire round trip */

    memcpy(framed, req, sizeof(req));
    crc16_to_wire(crc_tab, &framed[sizeof(req)]);
    CHECK_EQ_I(crc16_check_frame(framed, sizeof(framed)), 1);

    memcpy(corrupt, framed, sizeof(framed));
    corrupt[3] ^= 0x01u;
    CHECK_EQ_I(crc16_check_frame(corrupt, sizeof(corrupt)), 0);

    /* Incremental API must match the one shot API. */
    {
        uint16_t inc = CRC16_MODBUS_INIT;
        crc16_modbus_update(&inc, &req[0], 2u);
        crc16_modbus_update(&inc, &req[2], 4u);
        CHECK_EQ_I(inc, crc_tab);
    }

    /* Empty input is the init value. */
    CHECK_EQ_I(crc16_modbus(req, 0u), CRC16_MODBUS_INIT);
    CHECK_EQ_I(crc16_modbus(0, 5u), CRC16_MODBUS_INIT);
}

/* -------------------------------------------------------------- Modbus RTU */
static void test_modbus_rtu(void)
{
    modbus_request_t req;
    uint8_t          adu[MODBUS_MAX_ADU];
    int              n;
    modbus_response_t resp;
    int               rc;
    uint16_t          regs[4] = { 0x1122u, 0x3344u, 0x5566u, 0x7788u };

    suite("modbus_rtu");

    /* --- function 03 read holding registers --- */
    memset(&req, 0, sizeof(req));
    req.slave = 0x01; req.function = 0x03; req.start_addr = 0x0000; req.quantity = 4;
    n = modbus_build_request(&req, adu, sizeof(adu));
    CHECK_EQ_I(n, 8);
    CHECK_EQ_I(adu[0], 0x01);
    CHECK_EQ_I(adu[1], 0x03);
    CHECK_EQ_I(crc16_check_frame(adu, (size_t)n), 1);

    /* --- function 06 write single register ---
     * ADU: [0]=slave [1]=fc [2..3]=addr [4..5]=value [6..7]=crc            */
    memset(&req, 0, sizeof(req));
    req.slave = 0x05; req.function = 0x06; req.start_addr = 0x0004; req.value = 0x00FF;
    n = modbus_build_request(&req, adu, sizeof(adu));
    CHECK_EQ_I(n, 8);
    CHECK_EQ_I(adu[2], 0x00);
    CHECK_EQ_I(adu[3], 0x04);
    CHECK_EQ_I(adu[4], 0x00);
    CHECK_EQ_I(adu[5], 0xFF);
    CHECK_EQ_I(crc16_check_frame(adu, (size_t)n), 1);

    /* --- function 16 write multiple registers --- */
    memset(&req, 0, sizeof(req));
    req.slave = 0x02; req.function = 0x10; req.start_addr = 0x0010;
    req.quantity = 4; req.values = regs;
    n = modbus_build_request(&req, adu, sizeof(adu));
    CHECK_EQ_I(n, 9 + (2 * 4));
    CHECK_EQ_I(adu[6], 8);           /* byte count */
    CHECK_EQ_I(crc16_check_frame(adu, (size_t)n), 1);

    /* --- malformed requests must be rejected --- */
    memset(&req, 0, sizeof(req));
    req.slave = 0x00; req.function = 0x03; req.quantity = 1;
    CHECK_EQ_I(modbus_build_request(&req, adu, sizeof(adu)), -1);   /* broadcast */
    req.slave = 0xF8;                                                /* 248 > 247 */
    CHECK_EQ_I(modbus_build_request(&req, adu, sizeof(adu)), -1);
    req.slave = 0x01; req.quantity = 200;                            /* > 125 */
    CHECK_EQ_I(modbus_build_request(&req, adu, sizeof(adu)), -1);
    req.function = 0x99;                                             /* unsupported */
    CHECK_EQ_I(modbus_build_request(&req, adu, sizeof(adu)), -1);

    /* --- parse a well formed read response --- */
    {
        uint8_t rsp[16];
        rsp[0] = 0x01; rsp[1] = 0x03; rsp[2] = 8;
        rsp[3] = 0x11; rsp[4] = 0x22;
        rsp[5] = 0x33; rsp[6] = 0x44;
        rsp[7] = 0x55; rsp[8] = 0x66;
        rsp[9] = 0x77; rsp[10] = 0x88;
        crc16_to_wire(crc16_modbus(rsp, 11u), &rsp[11]);
        memset(&resp, 0, sizeof(resp));
        rc = modbus_parse_response(rsp, 13u, 0x01, 0x03, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_NONE);
        CHECK_EQ_I(resp.quantity, 4);
        CHECK_EQ_I(resp.values[0], 0x1122);
        CHECK_EQ_I(resp.values[3], 0x7788);
    }

    /* --- CRC error is detected locally --- */
    {
        uint8_t rsp[8];
        rsp[0] = 0x01; rsp[1] = 0x06; rsp[2] = 0x00; rsp[3] = 0x04;
        rsp[4] = 0x00; rsp[5] = 0xFF; rsp[6] = 0xAA; rsp[7] = 0xBB;
        rc = modbus_parse_response(rsp, 8u, 0x01, 0x06, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_BAD_CRC);
    }

    /* --- exception responses are surfaced to the caller --- */
    {
        uint8_t exc[5];
        exc[0] = 0x01; exc[1] = 0x83; exc[2] = MODBUS_EX_ILLEGAL_DATA_ADDR;
        crc16_to_wire(crc16_modbus(exc, 3u), &exc[3]);
        rc = modbus_parse_response(exc, 5u, 0x01, 0x03, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_ILLEGAL_DATA_ADDR);
        CHECK(strcmp(modbus_exception_str(rc), "ILLEGAL_DATA_ADDRESS") == 0);
    }

    /* --- unexpected slave is rejected --- */
    {
        uint8_t rsp[8];
        rsp[0] = 0x07; rsp[1] = 0x06; rsp[2] = 0x00; rsp[3] = 0x04;
        rsp[4] = 0x00; rsp[5] = 0x01;
        crc16_to_wire(crc16_modbus(rsp, 6u), &rsp[6]);
        rc = modbus_parse_response(rsp, 8u, 0x01, 0x06, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_BAD_SLAVE);
    }

    /* --- truncated frame is rejected --- */
    {
        uint8_t rsp[8];
        rsp[0] = 0x01; rsp[1] = 0x06; rsp[2] = 0x00;
        rc = modbus_parse_response(rsp, 3u, 0x01, 0x06, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_BAD_LENGTH);
    }

    /* --- write-multiple response echo --- */
    {
        uint8_t rsp[8];
        rsp[0] = 0x02; rsp[1] = 0x10; rsp[2] = 0x00; rsp[3] = 0x10;
        rsp[4] = 0x00; rsp[5] = 0x04;
        crc16_to_wire(crc16_modbus(rsp, 6u), &rsp[6]);
        memset(&resp, 0, sizeof(resp));
        rc = modbus_parse_response(rsp, 8u, 0x02, 0x10, &resp);
        CHECK_EQ_I(rc, MODBUS_EX_NONE);
        CHECK_EQ_I(resp.start_addr, 0x0010);
        CHECK_EQ_I(resp.quantity, 4);
    }
}

/* ----------------------------------------------------------- offline cache */
static void test_offline_cache(void)
{
    offline_cache_t c;
    cache_slot_t    s;
    int             i;
    uint8_t         payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    suite("offline_cache");

    offline_cache_init(&c);
    CHECK_EQ_I(c.count, 0);
    CHECK_EQ_I(offline_cache_peek(&c, &s), 0);          /* empty */

    /* Append order must be preserved on drain. */
    for (i = 0; i < 10; i++) {
        payload[0] = (uint8_t)i;
        CHECK_EQ_I(offline_cache_put(&c, payload, sizeof(payload), 0x0001, 1000u + (uint32_t)i), 1);
    }
    CHECK_EQ_I(c.count, 10);

    for (i = 0; i < 10; i++) {
        CHECK_EQ_I(offline_cache_peek(&c, &s), 1);
        CHECK_EQ_I(s.payload[0], i);                    /* FIFO order */
        CHECK_EQ_I(s.seq, (uint32_t)(i + 1));
        CHECK_EQ_I(offline_cache_commit(&c), 1);
    }
    CHECK_EQ_I(c.count, 0);
    CHECK_EQ_I(offline_cache_peek(&c, &s), 0);

    /* Full cache drops the oldest and counts the loss. */
    offline_cache_init(&c);
    for (i = 0; i < (int)CACHE_MAX_SLOTS + 5; i++) {
        payload[0] = (uint8_t)(i & 0xFF);
        offline_cache_put(&c, payload, sizeof(payload), 0x0002, (uint32_t)i);
    }
    CHECK_EQ_I(c.count, CACHE_MAX_SLOTS);
    CHECK_EQ_I(c.dropped, 5);
    CHECK_EQ_I(offline_cache_peek(&c, &s), 1);
    CHECK_EQ_I(s.payload[0], 5);                        /* oldest five were dropped */

    /* Corrupt slot is detected by CRC. */
    offline_cache_init(&c);
    offline_cache_put(&c, payload, sizeof(payload), 0x0003, 42u);
    c.slots[0].payload[0] ^= 0xFFu;
    CHECK_EQ_I(offline_cache_peek(&c, &s), 0);          /* corrupt head not returned */
    CHECK_EQ_I(offline_cache_verify(&c), 1);            /* and it is reported */

    /* Oversized payload is refused. */
    offline_cache_init(&c);
    CHECK_EQ_I(offline_cache_put(&c, payload, CACHE_SLOT_PAYLOAD + 1u, 1u, 0u), 0);
}

/* ---------------------------------------------------------- reconnect FSM */
static void test_reconnect_fsm(void)
{
    reconnect_fsm_t f;
    reconnect_action_t a;

    suite("reconnect_fsm");

    reconnect_fsm_init(&f, 1000u, 8000u);
    CHECK(strcmp(uplink_state_str(f.state), "IDLE") == 0);

    /* Link up -> connects immediately. */
    a = reconnect_fsm_tick(&f, 100u, 1, 0u, 0u);
    CHECK(strcmp(reconnect_action_str(a), "TRY_CONNECT") == 0);
    CHECK(strcmp(uplink_state_str(f.state), "CONNECTING") == 0);

    /* A failed connect doubles the backoff and does not connect early. */
    reconnect_fsm_on_connect_result(&f, 0);
    CHECK_EQ_I(f.backoff_ms, 2000);
    a = reconnect_fsm_tick(&f, 500u, 1, 0u, 0u);
    CHECK(strcmp(reconnect_action_str(a), "WAIT") == 0);
    a = reconnect_fsm_tick(&f, 1600u, 1, 0u, 0u);
    CHECK(strcmp(reconnect_action_str(a), "TRY_CONNECT") == 0);

    /* Backoff is capped. */
    reconnect_fsm_on_connect_result(&f, 0);
    reconnect_fsm_on_connect_result(&f, 0);
    reconnect_fsm_on_connect_result(&f, 0);
    CHECK(f.backoff_ms <= 8000u);

    /* DRAINING must flush the backlog before fresh samples go out. */
    reconnect_fsm_init(&f, 1000u, 8000u);
    (void)reconnect_fsm_tick(&f, 100u, 1, 0u, 0u);
    reconnect_fsm_on_connect_result(&f, 1);
    f.state = UPLINK_DRAINING;
    a = reconnect_fsm_tick(&f, 10u, 1, 3u, 5u);
    CHECK(strcmp(reconnect_action_str(a), "DRAIN_ONE") == 0);   /* backlog first */
    (void)reconnect_fsm_tick(&f, 10u, 1, 2u, 5u);
    (void)reconnect_fsm_tick(&f, 10u, 1, 1u, 5u);
    a = reconnect_fsm_tick(&f, 10u, 1, 0u, 5u);
    CHECK(strcmp(reconnect_action_str(a), "NONE") == 0);
    CHECK(strcmp(uplink_state_str(f.state), "ONLINE") == 0);

    /* Online: fresh samples are sent. */
    a = reconnect_fsm_tick(&f, 10u, 1, 0u, 2u);
    CHECK(strcmp(reconnect_action_str(a), "SEND_FRESH") == 0);

    /* Link loss resets to IDLE. */
    a = reconnect_fsm_tick(&f, 10u, 0, 0u, 0u);
    CHECK(strcmp(uplink_state_str(f.state), "IDLE") == 0);
    CHECK(strcmp(reconnect_action_str(a), "WAIT") == 0);

    /* Statistics are maintained. */
    CHECK(f.connect_ok > 0u);
    CHECK(f.drained_records >= 3u);
}

/* ------------------------------------------------------------- reg map */
static void test_register_map(void)
{
    reg_map_t m;
    reg_point_t pt;
    uint16_t words[4];
    float v;
    uint8_t isolated[32];

    suite("register_map");

    reg_map_init(&m);
    CHECK_EQ_I(m.count, 0);

    /* --- entry validation --- */
    memset(&pt, 0, sizeof(pt));
    pt.name = "bad_slave"; pt.slave = 0; pt.function = 0x03; pt.quantity = 1;
    CHECK_EQ_I(reg_map_add(&m, &pt), -1);
    pt.slave = 0x01; pt.function = 0x99;
    CHECK_EQ_I(reg_map_add(&m, &pt), -1);
    pt.function = 0x03; pt.quantity = 0;
    CHECK_EQ_I(reg_map_add(&m, &pt), -1);

    /* --- unsigned 16 bit with scaling --- */
    memset(&pt, 0, sizeof(pt));
    pt.name = "voltage"; pt.slave = 1; pt.function = 0x03; pt.quantity = 1;
    pt.start_addr = 0; pt.word_offset = 0; pt.type = REG_U16;
    pt.scale = 0.1f; pt.offset = 0.0f; pt.unit = "V"; pt.poll_class = POLL_FAST; pt.device_id = 1;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);

    words[0] = 2201u;
    CHECK_EQ_I(reg_map_decode(&m.points[0], words, 1u, &v), 0);
    CHECK(fabsf(v - 220.1f) < 0.001f);

    /* --- signed 16 bit --- */
    memset(&pt, 0, sizeof(pt));
    pt.name = "temp"; pt.slave = 1; pt.function = 0x03; pt.quantity = 1;
    pt.type = REG_S16; pt.scale = 1.0f; pt.offset = 0.0f; pt.unit = "C"; pt.poll_class = POLL_NORMAL;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);
    words[0] = 0xFF9Cu;                                 /* -100 */
    CHECK_EQ_I(reg_map_decode(&m.points[1], words, 1u, &v), 0);
    CHECK(fabsf(v + 100.0f) < 0.001f);

    /* --- 32 bit big endian vs word swapped --- */
    memset(&pt, 0, sizeof(pt));
    pt.name = "energy_be"; pt.slave = 1; pt.function = 0x03; pt.quantity = 2;
    pt.type = REG_U32_BE; pt.scale = 1.0f; pt.poll_class = POLL_SLOW;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);
    pt.name = "energy_ws"; pt.type = REG_U32_WORD_SWAP;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);

    words[0] = 0x0001u; words[1] = 0x0002u;             /* BE -> 0x00010002 */
    CHECK_EQ_I(reg_map_decode(&m.points[2], words, 2u, &v), 0);
    CHECK(fabsf(v - 65538.0f) < 0.5f);
    CHECK_EQ_I(reg_map_decode(&m.points[3], words, 2u, &v), 0);   /* WS -> 0x00020001 */
    CHECK(fabsf(v - 131073.0f) < 0.5f);

    /* --- IEEE754 float, both word orders --- */
    memset(&pt, 0, sizeof(pt));
    pt.name = "f_abcd"; pt.slave = 1; pt.function = 0x03; pt.quantity = 2;
    pt.type = REG_FLOAT_ABCD; pt.scale = 1.0f; pt.poll_class = POLL_NORMAL;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);
    pt.name = "f_cdab"; pt.type = REG_FLOAT_CDAB;
    CHECK_EQ_I(reg_map_add(&m, &pt), 0);

    /* 25.6f == 0x41CCCCCD */
    words[0] = 0x41CCu; words[1] = 0xCCCDu;
    CHECK_EQ_I(reg_map_decode(&m.points[4], words, 2u, &v), 0);
    CHECK(fabsf(v - 25.6f) < 0.01f);
    CHECK_EQ_I(reg_map_decode(&m.points[5], words, 2u, &v), 0);
    CHECK(v != 25.6f);                                   /* CDAB must differ */

    /* --- out of range access is refused --- */
    CHECK_EQ_I(reg_map_decode(&m.points[0], words, 0u, &v), -1);

    /* --- poll scheduler honours poll classes and isolated devices --- */
    {
        poll_stats_t st;
        uint8_t  slave, func;
        uint16_t start, qty;
        int      n_fast;
        int      n_isolated;

        memset(isolated, 0, sizeof(isolated));
        memset(&st, 0, sizeof(st));

        /* Fresh map: with a real time base the fast point is served first and
         * the rotation starts at index 0. */
        poll_scheduler_reset(&m);
        CHECK_EQ_I(poll_scheduler_next(&m, 5000u, isolated, &slave, &func, &start, &qty, &st), 1);
        CHECK_EQ_I(slave, 1);
        CHECK_EQ_I(func, 0x03);

        /* One second after a scheduler reset both the 1 s and the 5 s points are
         * due (their timers were never armed), but the 30 s points are not. So
         * the number served at t=6000 is smaller than the whole table. */
        n_fast = 0;
        {
            int served = 0;
            while (poll_scheduler_next(&m, 6000u, isolated, &slave, &func, &start, &qty, &st) == 1) {
                if (++served > 32) {
                    break;
                }
            }
            n_fast = served;
        }
        CHECK(n_fast > 0);
        CHECK(n_fast < (int)m.count);        /* the slow points are held back */

        /* Far in the future every class is due: the rotation must serve them all. */
        {
            int served = 0;
            while (poll_scheduler_next(&m, 600000u, isolated, &slave, &func, &start, &qty, &st) == 1) {
                if (++served > 32) {
                    break;
                }
            }
            CHECK_EQ_I(served, (int)m.count);
        }

        /* Isolating slave 1 removes exactly that slave's points from the rotation. */
        poll_scheduler_reset(&m);
        {
            int total = 0;
            int isolated_slave1_left = 0;
            memset(isolated, 0, sizeof(isolated));
            isolated[1] = 1u;
            while (poll_scheduler_next(&m, 600000u, isolated, &slave, &func, &start, &qty, &st) == 1) {
                total++;
                if (slave == 1u) {
                    isolated_slave1_left++;
                }
                if (total > 32) {
                    break;
                }
            }
            n_isolated = total;
            CHECK_EQ_I(isolated_slave1_left, 0);        /* slave 1 fully skipped */
            CHECK(n_isolated < (int)m.count);           /* fewer points than before */
            CHECK(st.skipped_backoff > 0u);
        }
    }

    /* --- plausibility guard --- */
    CHECK_EQ_I(reg_map_value_plausible(&m.points[0], 220.1f), 1);
    CHECK_EQ_I(reg_map_value_plausible(&m.points[0], 1.0e12f), 0);
}

int main(void)
{
    printf("gateway core unit tests\n");
    printf("=======================\n");

    test_crc16();
    test_modbus_rtu();
    test_offline_cache();
    test_reconnect_fsm();
    test_register_map();
    (void)run_rs485_tests();

    g_checks += rs485_checks();
    g_failed += rs485_failed();

    printf("\n=======================\n");
    printf("checks : %d\n", g_checks);
    printf("failed : %d\n", g_failed);
    printf("result : %s\n", (g_failed == 0) ? "ALL PASS" : "FAILURES PRESENT");

    return (g_failed == 0) ? 0 : 1;
}
