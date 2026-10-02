/*
 * modbus_rtu.h - Modbus RTU master codec (function codes 03/06/16).
 *
 * The codec is deliberately split from the transport: it only builds request
 * buffers and parses response buffers, so the same code runs on the target
 * (behind poll()) and on the host (behind a socketpair in the unit tests).
 */
#ifndef GATEWAY_MODBUS_RTU_H
#define GATEWAY_MODBUS_RTU_H

#include <stddef.h>
#include <stdint.h>

#define MODBUS_MAX_ADU 256u
#define MODBUS_MAX_REGS 125u
#define MODBUS_MAX_WRITE_REGS 123u

/* Modbus exception codes we surface to the caller. */
typedef enum {
    MODBUS_EX_NONE               = 0x00,
    MODBUS_EX_ILLEGAL_FUNCTION   = 0x01,
    MODBUS_EX_ILLEGAL_DATA_ADDR  = 0x02,
    MODBUS_EX_ILLEGAL_DATA_VALUE = 0x03,
    MODBUS_EX_SLAVE_FAILURE      = 0x04,
    MODBUS_EX_ACKNOWLEDGE        = 0x05,
    MODBUS_EX_SLAVE_BUSY         = 0x06,
    MODBUS_EX_GATEWAY_FAILURE    = 0x0B,
    MODBUS_EX_BAD_CRC            = 0x80, /* local detection, not on the wire */
    MODBUS_EX_TIMEOUT            = 0x81, /* local detection, not on the wire */
    MODBUS_EX_BAD_LENGTH         = 0x82, /* local detection, not on the wire */
    MODBUS_EX_BAD_SLAVE          = 0x83  /* local detection, not on the wire */
} modbus_exception_t;

typedef struct {
    uint8_t  slave;
    uint8_t  function;
    uint16_t start_addr;
    uint16_t quantity;      /* register count for 03/16, value for 06 */
    uint16_t value;         /* used by 06 only */
    const uint16_t *values; /* used by 16 only */
} modbus_request_t;

typedef struct {
    uint8_t  slave;
    uint8_t  function;
    uint16_t start_addr;
    uint16_t quantity;
    uint16_t values[MODBUS_MAX_REGS];
} modbus_response_t;

/*
 * Build a request ADU into `out` (must hold MODBUS_MAX_ADU bytes).
 * Returns the ADU length on success, or -1 when the request is malformed
 * (bad slave address, quantity out of range, NULL pointer, ...).
 */
int modbus_build_request(const modbus_request_t *req, uint8_t *out, size_t out_cap);

/*
 * Parse a response ADU.
 * Returns 0 on success, or a positive modbus_exception_t on a decoded
 * exception / malformed frame.
 */
int modbus_parse_response(const uint8_t *adu, size_t len, uint8_t expect_slave,
                          uint8_t expect_function, modbus_response_t *out);

/* Human readable name for an exception code (for logs and test output). */
const char *modbus_exception_str(int code);

#endif /* GATEWAY_MODBUS_RTU_H */
