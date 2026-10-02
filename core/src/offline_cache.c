/*
 * offline_cache.c - ring buffer of fixed size cache slots.
 *
 * The cache is a ring: writes advance write_idx modulo CACHE_MAX_SLOTS and the
 * read position is always "write_idx - count". Losing the oldest record on a
 * full cache is the deliberate policy: a gateway that stalls its acquisition
 * because the uplink is down is worse than a gateway that loses the oldest
 * sample and keeps reporting the newest one.
 */
#include "offline_cache.h"
#include "crc16.h"

#include <string.h>

static uint16_t slot_crc(const cache_slot_t *s)
{
    /* The CRC covers everything except the crc and valid fields. */
    return crc16_modbus((const uint8_t *)s, offsetof(cache_slot_t, crc));
}

void offline_cache_init(offline_cache_t *c)
{
    if (c == 0) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->next_seq = 1u;
}

static uint32_t read_idx(const offline_cache_t *c)
{
    if (c->count == 0u) {
        return c->write_idx;
    }
    return (c->write_idx + CACHE_MAX_SLOTS - c->count) % CACHE_MAX_SLOTS;
}

int offline_cache_put(offline_cache_t *c, const void *payload, uint16_t len,
                      uint16_t device_id, uint32_t timestamp_s)
{
    cache_slot_t *slot;

    if (c == 0 || payload == 0 || len > CACHE_SLOT_PAYLOAD) {
        return 0;
    }
    if (c->count == CACHE_MAX_SLOTS) {
        /* Full: drop the oldest record, then reuse its slot. */
        c->count--;
        c->dropped++;
    }

    slot = &c->slots[c->write_idx];
    memset(slot, 0, sizeof(*slot));
    slot->seq         = c->next_seq++;
    slot->timestamp_s = timestamp_s;
    slot->device_id   = device_id;
    slot->payload_len = len;
    memcpy(slot->payload, payload, len);
    slot->crc   = slot_crc(slot);
    slot->valid = 1u;

    c->write_idx = (c->write_idx + 1u) % CACHE_MAX_SLOTS;
    c->count++;
    return 1;
}

int offline_cache_peek(const offline_cache_t *c, cache_slot_t *out)
{
    const cache_slot_t *slot;

    if (c == 0 || out == 0 || c->count == 0u) {
        return 0;
    }

    slot = &c->slots[read_idx(c)];
    if (slot->valid != 1u || slot->crc != slot_crc(slot)) {
        /* Corrupt head slot: report it as no record so the caller can run
         * offline_cache_verify() and decide what to do. */
        return 0;
    }

    *out = *slot;
    return 1;
}

int offline_cache_commit(offline_cache_t *c)
{
    cache_slot_t *slot;

    if (c == 0 || c->count == 0u) {
        return 0;
    }

    slot = &c->slots[read_idx(c)];
    slot->valid = 0u;
    c->count--;
    return 1;
}

uint32_t offline_cache_verify(const offline_cache_t *c)
{
    uint32_t bad = 0;
    uint32_t i;

    if (c == 0) {
        return 0;
    }

    for (i = 0; i < CACHE_MAX_SLOTS; i++) {
        const cache_slot_t *s = &c->slots[i];
        if (s->valid == 1u && s->crc != slot_crc(s)) {
            bad++;
        }
    }
    return bad;
}
