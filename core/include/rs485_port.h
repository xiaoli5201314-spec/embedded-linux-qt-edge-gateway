/*
 * rs485_port.h - non blocking RS485 half duplex port abstraction.
 *
 * A half duplex RS485 bus needs the driver to know when the last bit has left
 * the shift register before it may release the DE (driver enable) line.
 * Releasing DE too early truncates the trailing bytes, releasing it too late
 * makes the slave answer into a bus that is still driven. The port therefore
 * exposes an explicit drain phase instead of hiding it inside write().
 *
 * The port is poll() based: the acquisition loop never blocks on a single
 * device, which is what keeps one offline meter from stalling the rotation.
 */
#ifndef GATEWAY_RS485_PORT_H
#define GATEWAY_RS485_PORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct rs485_port rs485_port_t;

typedef enum {
    RS485_OK = 0,
    RS485_ERR_OPEN,
    RS485_ERR_IO,
    RS485_ERR_TIMEOUT,
    RS485_ERR_AGAIN
} rs485_status_t;

typedef struct {
    const char *device;      /* e.g. "/dev/ttyS3" */
    uint32_t    baudrate;    /* 9600 / 19200 / 115200 ... */
    uint8_t     data_bits;   /* 8 */
    uint8_t     stop_bits;   /* 1 or 2 */
    uint8_t     parity;      /* 0 none, 1 odd, 2 even */
    int         de_gpio;     /* libgpiod line offset driving DE/RE, -1 if auto */
    uint32_t    de_guard_us; /* extra hold time after the last byte */
} rs485_config_t;

rs485_port_t *rs485_open(const rs485_config_t *cfg);
void          rs485_close(rs485_port_t *p);

/* Queue a frame; the DE line is asserted here. Returns bytes queued or <0. */
int rs485_write(rs485_port_t *p, const uint8_t *data, size_t len);

/* True once every queued byte has been shifted out (TC bit / tcdrain). */
int rs485_tx_complete(rs485_port_t *p, uint32_t timeout_ms);

/* Non blocking read: returns >0 bytes, 0 when nothing is available. */
int rs485_read(rs485_port_t *p, uint8_t *buf, size_t cap, uint32_t timeout_ms);

/* Flush both directions, used before starting a new transaction on a bus that
 * previously timed out (a late answer from the old request must not be seen as
 * the answer to the new one). */
void rs485_reset_queues(rs485_port_t *p);

/*
 * Dependency injection for host tests: when `fd` >= 0 the port uses this file
 * descriptor instead of opening `device`.
 */
rs485_port_t *rs485_open_fd(int fd, const rs485_config_t *cfg);

#endif /* GATEWAY_RS485_PORT_H */
