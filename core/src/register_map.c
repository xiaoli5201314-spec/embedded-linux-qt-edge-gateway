/*
 * register_map.c - register map table, value decoding and poll rotation.
 *
 * Decoding is where most field bugs live: the same physical value can appear as
 * two 16 bit words in either order, as a scaled integer, or as an IEEE754 float
 * with either of two byte orders. Each case is handled explicitly and covered
 * by a unit test rather than inferred from the device documentation at runtime.
 */
#include "register_map.h"

#include <string.h>

/* Refresh period per poll class, in milliseconds. */
static const uint32_t k_poll_period_ms[3] = { 1000u, 5000u, 30000u };

void reg_map_init(reg_map_t *m)
{
    if (m == 0) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

int reg_map_add(reg_map_t *m, const reg_point_t *pt)
{
    if (m == 0 || pt == 0) {
        return -1;
    }
    if (m->count >= REGMAP_MAX_ENTRIES) {
        return -1;
    }
    if (pt->slave == 0u || pt->slave > 247u) {
        return -1;
    }
    if (pt->function != 0x03u && pt->function != 0x04u) {
        return -1;
    }
    if (pt->quantity == 0u) {
        return -1;
    }
    m->points[m->count] = *pt;
    m->count++;
    return 0;
}

static uint16_t width_of(reg_type_t t)
{
    switch (t) {
    case REG_U16:
    case REG_S16:
        return 1u;
    case REG_U32_BE:
    case REG_U32_WORD_SWAP:
    case REG_FLOAT_ABCD:
    case REG_FLOAT_CDAB:
        return 2u;
    default:
        return 1u;
    }
}

int reg_map_decode(const reg_point_t *pt, const uint16_t *words, uint16_t word_count,
                   float *out)
{
    uint16_t w = 0;
    uint16_t need;
    uint32_t u32 = 0u;
    int32_t  s32 = 0;
    float    f = 0.0f;
    union {
        uint32_t u;
        float    f;
    } pun;

    if (pt == 0 || words == 0 || out == 0) {
        return -1;
    }

    need = width_of(pt->type);
    if ((uint16_t)(pt->word_offset + need) > word_count) {
        return -1;
    }
    w = pt->word_offset;

    switch (pt->type) {
    case REG_U16:
        *out = ((float)words[w]) * pt->scale + pt->offset;
        break;

    case REG_S16:
        s32 = (int32_t)(int16_t)words[w];
        *out = ((float)s32) * pt->scale + pt->offset;
        break;

    case REG_U32_BE:
        u32 = (((uint32_t)words[w]) << 16) | (uint32_t)words[w + 1u];
        *out = ((float)u32) * pt->scale + pt->offset;
        break;

    case REG_U32_WORD_SWAP:
        u32 = (((uint32_t)words[w + 1u]) << 16) | (uint32_t)words[w];
        *out = ((float)u32) * pt->scale + pt->offset;
        break;

    case REG_FLOAT_ABCD:
        pun.u = (((uint32_t)words[w]) << 16) | (uint32_t)words[w + 1u];
        f = pun.f;
        *out = f * pt->scale + pt->offset;
        break;

    case REG_FLOAT_CDAB:
        pun.u = (((uint32_t)words[w + 1u]) << 16) | (uint32_t)words[w];
        f = pun.f;
        *out = f * pt->scale + pt->offset;
        break;

    default:
        return -1;
    }

    return 0;
}

int poll_scheduler_reset(reg_map_t *m)
{
    if (m == 0) {
        return -1;
    }
    memset(m->last_poll_ms, 0, sizeof(m->last_poll_ms));
    m->rotation = 0u;
    return 0;
}

int poll_scheduler_next(const reg_map_t *m, uint32_t now_ms, const uint8_t *isolated,
                        uint8_t *slave_out, uint8_t *function_out,
                        uint16_t *start_out, uint16_t *quantity_out,
                        poll_stats_t *stats)
{
    uint32_t i;
    uint32_t n;

    if (m == 0 || slave_out == 0 || function_out == 0 || start_out == 0 || quantity_out == 0) {
        return 0;
    }

    /* Rotate over the table so no single fast point can starve the others. */
    n = m->count;
    for (i = 0; i < n; i++) {
        reg_map_t *mm = (reg_map_t *)m;   /* last_poll_ms / rotation are bookkeeping */
        uint32_t   idx = (m->rotation + i) % n;
        const reg_point_t *pt = &m->points[idx];
        uint32_t           period;
        uint8_t            cls = (uint8_t)pt->poll_class;

        if (cls > 2u) {
            cls = 2u;
        }
        period = k_poll_period_ms[cls];

        if ((now_ms - m->last_poll_ms[idx]) < period) {
            if (stats != 0) {
                stats->skipped_not_due++;
            }
            continue;
        }
        /* An isolated device stays out of the rotation until it answers again. */
        if (isolated != 0 && pt->slave < 32u && isolated[pt->slave] != 0u) {
            if (stats != 0) {
                stats->skipped_backoff++;
            }
            continue;
        }

        mm->last_poll_ms[idx] = now_ms;
        mm->rotation = (idx + 1u) % n;

        *slave_out    = pt->slave;
        *function_out = pt->function;
        *start_out    = pt->start_addr;
        *quantity_out = pt->quantity;

        if (stats != 0) {
            stats->generated++;
            stats->due[cls]++;
        }
        return 1;
    }

    return 0;
}

int reg_map_value_plausible(const reg_point_t *pt, float value)
{
    if (pt == 0) {
        return 0;
    }
    /* Overflow of a 16 bit raw register is a classic wiring / word order bug:
     * a value far outside the scaled range means the decode is wrong. */
    if (value != value) {          /* NaN */
        return 0;
    }
    if (value > 1.0e9f || value < -1.0e9f) {
        return 0;
    }
    return 1;
}
