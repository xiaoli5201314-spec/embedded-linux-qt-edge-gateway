/*
 * crc16.c - Modbus CRC-16 (polynomial 0xA001, reflected) implementation.
 *
 * Two implementations are provided:
 *   1. bitwise           - small code size, no table, used for code review clarity
 *   2. table driven      - 256-entry table generated at first use, used on the target
 *
 * Both are cross-checked against each other by the unit tests, which is how the
 * table generation itself is validated.
 */
#include "crc16.h"

static uint16_t s_table[256];
static int      s_table_ready = 0;

static void crc16_build_table(void)
{
    uint16_t i;
    int      bit;

    for (i = 0; i < 256u; i++) {
        uint16_t crc = i;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ CRC16_MODBUS_POLY);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
        s_table[i] = crc;
    }
    s_table_ready = 1;
}

uint16_t crc16_modbus_bitwise(const uint8_t *data, size_t len)
{
    uint16_t crc = CRC16_MODBUS_INIT;
    size_t   i;
    int      bit;

    if (data == 0) {
        return crc;
    }

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ CRC16_MODBUS_POLY);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = CRC16_MODBUS_INIT;
    size_t   i;

    if (data == 0) {
        return crc;
    }
    if (!s_table_ready) {
        crc16_build_table();
    }

    for (i = 0; i < len; i++) {
        crc = (uint16_t)((crc >> 8) ^ s_table[(crc ^ data[i]) & 0xFFu]);
    }
    return crc;
}

void crc16_modbus_update(uint16_t *crc, const uint8_t *data, size_t len)
{
    size_t i;

    if (crc == 0 || data == 0) {
        return;
    }
    if (!s_table_ready) {
        crc16_build_table();
    }

    for (i = 0; i < len; i++) {
        *crc = (uint16_t)((*crc >> 8) ^ s_table[(*crc ^ data[i]) & 0xFFu]);
    }
}

/* The wire format of Modbus RTU is little endian for the CRC field: the low
 * byte is transmitted first. Returning the CRC as a host-order integer and
 * serialising it here keeps call sites from scattering byte-order logic. */
void crc16_to_wire(uint16_t crc, uint8_t out[2])
{
    out[0] = (uint8_t)(crc & 0xFFu);
    out[1] = (uint8_t)((crc >> 8) & 0xFFu);
}

uint16_t crc16_from_wire(const uint8_t in[2])
{
    return (uint16_t)(((uint16_t)in[1] << 8) | (uint16_t)in[0]);
}

int crc16_check_frame(const uint8_t *frame, size_t len)
{
    uint16_t crc;

    if (frame == 0 || len < 2u) {
        return 0;
    }

    crc = crc16_modbus(frame, len - 2u);
    return (crc == crc16_from_wire(&frame[len - 2u])) ? 1 : 0;
}
