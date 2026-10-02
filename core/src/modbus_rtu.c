/*
 * modbus_rtu.c - Modbus RTU master codec.
 *
 * Frame layout (ADU):
 *
 *   +--------+----------+---------------+---------------+
 *   | slave  | function | payload ...   | CRC16 (2B LE) |
 *   +--------+----------+---------------+---------------+
 *     1 byte   1 byte     N bytes          2 bytes
 *
 * Request payloads:
 *   0x03 read holding registers   : start(2) quantity(2)
 *   0x06 write single register    : address(2) value(2)
 *   0x10 write multiple registers : start(2) quantity(2) byte_count(1) values(2N)
 *
 * Response payloads:
 *   0x03 : byte_count(1) values(2N)
 *   0x06 : address(2) value(2)              (echo)
 *   0x10 : start(2) quantity(2)
 *   exception (0x80 | fc) : exception_code(1)
 *
 * All multi-byte fields are big endian on the wire.
 */
#include "modbus_rtu.h"
#include "crc16.h"

#include <string.h>

#define FC_READ_HOLDING     0x03u
#define FC_WRITE_SINGLE     0x06u
#define FC_WRITE_MULTIPLE   0x10u
#define FC_EXCEPTION_MASK   0x80u

static void put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int finish_adu(uint8_t *out, size_t body_len)
{
    uint16_t crc = crc16_modbus(out, body_len);
    crc16_to_wire(crc, &out[body_len]);
    return (int)(body_len + 2u);
}

int modbus_build_request(const modbus_request_t *req, uint8_t *out, size_t out_cap)
{
    size_t body_len;

    if (req == 0 || out == 0 || out_cap < 4u) {
        return -1;
    }
    /* Addresses 0 (broadcast) and >247 are not legal unicast targets here. */
    if (req->slave == 0u || req->slave > 247u) {
        return -1;
    }

    out[0] = req->slave;
    out[1] = req->function;

    switch (req->function) {
    case FC_READ_HOLDING:
        if (req->quantity == 0u || req->quantity > MODBUS_MAX_REGS) {
            return -1;
        }
        put_u16_be(&out[2], req->start_addr);
        put_u16_be(&out[4], req->quantity);
        body_len = 6u;
        break;

    case FC_WRITE_SINGLE:
        if (req->quantity != 0u) {
            /* for 06 `quantity` is unused and must stay zero */
            return -1;
        }
        put_u16_be(&out[2], req->start_addr);
        put_u16_be(&out[4], req->value);
        body_len = 6u;
        break;

    case FC_WRITE_MULTIPLE:
        if (req->values == 0) {
            return -1;
        }
        if (req->quantity == 0u || req->quantity > MODBUS_MAX_WRITE_REGS) {
            return -1;
        }
        /* 1 + 1 + 2 + 2 + 1 + 2N + 2 must fit the caller's buffer */
        if (out_cap < (size_t)(9u + (2u * req->quantity))) {
            return -1;
        }
        put_u16_be(&out[2], req->start_addr);
        put_u16_be(&out[4], req->quantity);
        out[6] = (uint8_t)(req->quantity * 2u);
        {
            uint16_t i;
            for (i = 0; i < req->quantity; i++) {
                put_u16_be(&out[7u + (2u * i)], req->values[i]);
            }
        }
        body_len = 7u + (2u * (size_t)req->quantity);
        break;

    default:
        return -1;
    }

    if (out_cap < body_len + 2u) {
        return -1;
    }
    return finish_adu(out, body_len);
}

int modbus_parse_response(const uint8_t *adu, size_t len, uint8_t expect_slave,
                          uint8_t expect_function, modbus_response_t *out)
{
    uint8_t  slave;
    uint8_t  function;
    uint16_t byte_count;
    uint16_t i;

    if (adu == 0 || len < 5u) {
        return MODBUS_EX_BAD_LENGTH;
    }
    if (!crc16_check_frame(adu, len)) {
        return MODBUS_EX_BAD_CRC;
    }

    slave    = adu[0];
    function = adu[1];

    /* An exception frame is exactly 5 bytes: slave, fc|0x80, code, crc(2). */
    if (function & FC_EXCEPTION_MASK) {
        if (len != 5u) {
            return MODBUS_EX_BAD_LENGTH;
        }
        if ((function & ~FC_EXCEPTION_MASK) != expect_function) {
            return MODBUS_EX_BAD_LENGTH;
        }
        if (slave != expect_slave) {
            return MODBUS_EX_BAD_SLAVE;
        }
        return (int)adu[2];
    }

    if (slave != expect_slave) {
        return MODBUS_EX_BAD_SLAVE;
    }
    if (function != expect_function) {
        return MODBUS_EX_ILLEGAL_FUNCTION;
    }

    if (out == 0) {
        return 0;
    }
    out->slave    = slave;
    out->function = function;

    switch (function) {
    case FC_READ_HOLDING:
        if (len < 5u) {
            return MODBUS_EX_BAD_LENGTH;
        }
        byte_count = adu[2];
        if (byte_count == 0u || (byte_count & 1u) != 0u) {
            return MODBUS_EX_BAD_LENGTH;
        }
        if ((size_t)byte_count + 5u != len) {
            return MODBUS_EX_BAD_LENGTH;
        }
        out->quantity = (uint16_t)(byte_count / 2u);
        if (out->quantity > MODBUS_MAX_REGS) {
            return MODBUS_EX_BAD_LENGTH;
        }
        for (i = 0; i < out->quantity; i++) {
            out->values[i] = get_u16_be(&adu[3u + (2u * i)]);
        }
        break;

    case FC_WRITE_SINGLE:
        if (len != 8u) {
            return MODBUS_EX_BAD_LENGTH;
        }
        out->start_addr = get_u16_be(&adu[2]);
        out->quantity   = 1u;
        out->values[0]  = get_u16_be(&adu[4]);
        break;

    case FC_WRITE_MULTIPLE:
        if (len != 8u) {
            return MODBUS_EX_BAD_LENGTH;
        }
        out->start_addr = get_u16_be(&adu[2]);
        out->quantity   = get_u16_be(&adu[4]);
        if (out->quantity == 0u || out->quantity > MODBUS_MAX_WRITE_REGS) {
            return MODBUS_EX_BAD_LENGTH;
        }
        break;

    default:
        return MODBUS_EX_ILLEGAL_FUNCTION;
    }

    return MODBUS_EX_NONE;
}

const char *modbus_exception_str(int code)
{
    switch (code) {
    case MODBUS_EX_NONE:               return "OK";
    case MODBUS_EX_ILLEGAL_FUNCTION:   return "ILLEGAL_FUNCTION";
    case MODBUS_EX_ILLEGAL_DATA_ADDR:  return "ILLEGAL_DATA_ADDRESS";
    case MODBUS_EX_ILLEGAL_DATA_VALUE: return "ILLEGAL_DATA_VALUE";
    case MODBUS_EX_SLAVE_FAILURE:      return "SLAVE_DEVICE_FAILURE";
    case MODBUS_EX_ACKNOWLEDGE:        return "ACKNOWLEDGE";
    case MODBUS_EX_SLAVE_BUSY:         return "SLAVE_DEVICE_BUSY";
    case MODBUS_EX_GATEWAY_FAILURE:    return "GATEWAY_PATH_UNAVAILABLE";
    case MODBUS_EX_BAD_CRC:            return "LOCAL_BAD_CRC";
    case MODBUS_EX_TIMEOUT:            return "LOCAL_TIMEOUT";
    case MODBUS_EX_BAD_LENGTH:         return "LOCAL_BAD_LENGTH";
    case MODBUS_EX_BAD_SLAVE:          return "LOCAL_UNEXPECTED_SLAVE";
    default:                           return "UNKNOWN";
    }
}
