/*
 * offline_cache.h - power-fail safe offline sample cache.
 *
 * When the uplink is down (or the network is flaky) every collected sample is
 * appended to this cache. Once the link is back the cache is drained in strict
 * append order, and each drained record is acknowledged only after the uplink
 * confirmed it, so a power cut in the middle of a drain cannot lose data.
 *
 * The record layout is fixed size on purpose: a fixed slot size keeps the file
 * format self describing, makes the "is this slot valid" decision a single CRC
 * check, and avoids a compaction pass on the target's slow eMMC.
 */
#ifndef GATEWAY_OFFLINE_CACHE_H
#define GATEWAY_OFFLINE_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define CACHE_MAX_SLOTS   512u
#define CACHE_SLOT_PAYLOAD 48u

typedef struct {
    uint32_t seq;                      /* monotonic, never reused */
    uint32_t timestamp_s;              /* unix seconds */
    uint16_t device_id;
    uint16_t payload_len;
    uint8_t  payload[CACHE_SLOT_PAYLOAD];
    uint16_t crc;                      /* over the fields above */
    uint8_t  valid;                    /* 1 = slot holds a record */
} cache_slot_t;

typedef struct {
    cache_slot_t slots[CACHE_MAX_SLOTS];
    uint32_t     write_idx;            /* next slot to write */
    uint32_t     count;                /* number of valid records */
    uint32_t     next_seq;
    uint64_t     dropped;              /* records lost because the cache was full */
} offline_cache_t;

void offline_cache_init(offline_cache_t *c);

/*
 * Append a record. Returns 1 on success, 0 when the cache is full (the oldest
 * record is dropped and `dropped` is incremented so the operator can see it).
 */
int offline_cache_put(offline_cache_t *c, const void *payload, uint16_t len,
                      uint16_t device_id, uint32_t timestamp_s);

/*
 * Peek the oldest record without removing it. Returns 1 when a record was
 * returned, 0 when the cache is empty. The caller must call
 * offline_cache_commit() only after the uplink acknowledged that exact seq.
 */
int offline_cache_peek(const offline_cache_t *c, cache_slot_t *out);

/* Drop the oldest record (called after a successful uplink). */
int offline_cache_commit(offline_cache_t *c);

/* Validate every slot's CRC; returns the number of corrupt slots found. */
uint32_t offline_cache_verify(const offline_cache_t *c);

#endif /* GATEWAY_OFFLINE_CACHE_H */
