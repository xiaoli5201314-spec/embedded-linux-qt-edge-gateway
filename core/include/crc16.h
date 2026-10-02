/*
 * crc16.h - Modbus CRC-16.
 *
 * Polynomial 0x8005 reflected -> 0xA001, init 0xFFFF, no final xor.
 * This is the CRC used by Modbus RTU on the wire.
 */
#ifndef GATEWAY_CRC16_H
#define GATEWAY_CRC16_H

#include <stddef.h>
#include <stdint.h>

#define CRC16_MODBUS_POLY 0xA001u
#define CRC16_MODBUS_INIT 0xFFFFu

/* Bitwise reference implementation (no table, easy to audit). */
uint16_t crc16_modbus_bitwise(const uint8_t *data, size_t len);

/* Table driven implementation used on the target. */
uint16_t crc16_modbus(const uint8_t *data, size_t len);

/* Incremental variant for streams that arrive in chunks. */
void crc16_modbus_update(uint16_t *crc, const uint8_t *data, size_t len);

/* Modbus RTU transmits the CRC low byte first. */
void     crc16_to_wire(uint16_t crc, uint8_t out[2]);
uint16_t crc16_from_wire(const uint8_t in[2]);

/* Returns 1 when the trailing two bytes match the CRC of the preceding bytes. */
int crc16_check_frame(const uint8_t *frame, size_t len);

#endif /* GATEWAY_CRC16_H */
