/*
 * register_map.h - declarative Modbus register map and poll scheduler.
 *
 * Every field device is described once in a static table: which slave address,
 * which function code, which register range, how the raw words are scaled and
 * how often the range must be refreshed. The poll scheduler then walks that
 * table and asks the codec for the next request, which keeps the acquisition
 * loop free of per-device special cases.
 *
 * Adding a device is therefore a data change, not a code change - which is how
 * the field engineer can commission a new meter without a firmware rebuild.
 */
#ifndef GATEWAY_REGISTER_MAP_H
#define GATEWAY_REGISTER_MAP_H

#include <stddef.h>
#include <stdint.h>

#define REGMAP_MAX_ENTRIES   64u
#define REGMAP_MAX_DEVICES   16u

typedef enum {
    REG_U16 = 0,        /* unsigned 16 bit */
    REG_S16,            /* signed 16 bit (two's complement) */
    REG_U32_BE,         /* 32 bit, high word first */
    REG_U32_WORD_SWAP,  /* 32 bit, low word first (common on energy meters) */
    REG_FLOAT_ABCD,     /* IEEE754, A B C D word order */
    REG_FLOAT_CDAB      /* IEEE754, C D A B word order */
} reg_type_t;

typedef enum {
    POLL_FAST   = 0,    /* every scheduler pass, e.g. 1 s  */
    POLL_NORMAL,        /* e.g. 5 s  */
    POLL_SLOW           /* e.g. 30 s */
} poll_class_t;

typedef struct {
    const char   *name;         /* human readable tag, used in logs and SQLite */
    uint8_t       slave;        /* Modbus slave address 1..247 */
    uint8_t       function;     /* 0x03 or 0x04 */
    uint16_t      start_addr;   /* first register */
    uint16_t      quantity;     /* number of 16 bit registers */
    uint8_t       word_offset;  /* offset into the response payload */
    reg_type_t    type;
    float         scale;        /* engineering value = raw * scale + offset */
    float         offset;
    const char   *unit;
    poll_class_t  poll_class;
    uint16_t      device_id;    /* logical device this point belongs to */
} reg_point_t;

typedef struct {
    reg_point_t points[REGMAP_MAX_ENTRIES];
    uint32_t    count;
    uint32_t    last_poll_ms[REGMAP_MAX_ENTRIES]; /* per point, scheduler bookkeeping */
    uint32_t    rotation;                          /* round robin cursor */
} reg_map_t;

typedef struct {
    uint32_t due[3];            /* per poll class: number of points due */
    uint32_t generated;         /* total requests generated */
    uint32_t skipped_not_due;
    uint32_t skipped_backoff;   /* device is currently isolated */
} poll_stats_t;

void reg_map_init(reg_map_t *m);

/* Clear the scheduler bookkeeping (call once before the acquisition loop). */
int poll_scheduler_reset(reg_map_t *m);

/* Returns 0 on success, -1 when the table is full or the entry is invalid. */
int reg_map_add(reg_map_t *m, const reg_point_t *pt);

/* Decode one engineering value from a response payload. */
int reg_map_decode(const reg_point_t *pt, const uint16_t *words, uint16_t word_count,
                   float *out);

/*
 * Pick the next register range to poll. Devices whose `isolated` flag is set
 * are skipped so a dead device cannot stall the rotation. Returns 1 when a
 * range was produced, 0 when nothing is due.
 */
int poll_scheduler_next(const reg_map_t *m, uint32_t now_ms, const uint8_t *isolated,
                        uint8_t *slave_out, uint8_t *function_out,
                        uint16_t *start_out, uint16_t *quantity_out,
                        poll_stats_t *stats);

/* Classify a decoded value against the point's plausible engineering range. */
int reg_map_value_plausible(const reg_point_t *pt, float value);

#endif /* GATEWAY_REGISTER_MAP_H */
