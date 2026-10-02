/*
 * reconnect_fsm.c - uplink reconnect / re-drain state machine.
 *
 * Backoff is exponential with a cap and a reset on success. The state machine
 * keeps no transport state of its own: the caller reports link_up and the
 * backlog size, which makes the whole thing testable without any sockets.
 */
#include "reconnect_fsm.h"

void reconnect_fsm_init(reconnect_fsm_t *f, uint32_t backoff_min_ms, uint32_t backoff_max_ms)
{
    if (f == 0) {
        return;
    }
    f->state           = UPLINK_IDLE;
    f->backoff_min_ms  = (backoff_min_ms == 0u) ? 1000u : backoff_min_ms;
    f->backoff_max_ms  = (backoff_max_ms < f->backoff_min_ms) ? f->backoff_min_ms : backoff_max_ms;
    f->backoff_ms      = f->backoff_min_ms;
    f->elapsed_ms      = 0u;
    f->connect_ok      = 0u;
    f->connect_fail    = 0u;
    f->drained_records = 0u;
    f->online_since_ms = 0u;
}

void reconnect_fsm_on_connect_result(reconnect_fsm_t *f, int ok)
{
    if (f == 0) {
        return;
    }
    if (ok) {
        f->connect_ok++;
        f->backoff_ms = f->backoff_min_ms;
    } else {
        f->connect_fail++;
        /* Exponential backoff, saturating at the cap. */
        if (f->backoff_ms < f->backoff_max_ms) {
            f->backoff_ms *= 2u;
            if (f->backoff_ms > f->backoff_max_ms) {
                f->backoff_ms = f->backoff_max_ms;
            }
        }
    }
    f->elapsed_ms = 0u;
}

void reconnect_fsm_on_link_lost(reconnect_fsm_t *f)
{
    if (f == 0) {
        return;
    }
    f->state      = UPLINK_IDLE;
    f->elapsed_ms = 0u;
}

reconnect_action_t reconnect_fsm_tick(reconnect_fsm_t *f, uint32_t dt_ms,
                                      int link_up, uint32_t backlog, uint32_t fresh)
{
    if (f == 0) {
        return FSM_ACT_NONE;
    }
    f->elapsed_ms += dt_ms;

    /* A lost link always wins, whatever we were doing. */
    if (!link_up && f->state != UPLINK_IDLE) {
        reconnect_fsm_on_link_lost(f);
        return FSM_ACT_WAIT;
    }

    switch (f->state) {
    case UPLINK_IDLE:
        if (!link_up) {
            return FSM_ACT_WAIT;
        }
        f->state      = UPLINK_CONNECTING;
        f->elapsed_ms = 0u;
        return FSM_ACT_TRY_CONNECT;

    case UPLINK_CONNECTING:
        if (f->elapsed_ms >= f->backoff_ms) {
            return FSM_ACT_TRY_CONNECT;
        }
        return FSM_ACT_WAIT;

    case UPLINK_DRAINING:
        if (backlog > 0u) {
            f->drained_records++;
            return FSM_ACT_DRAIN_ONE;
        }
        f->state           = UPLINK_ONLINE;
        f->online_since_ms = 0u;
        return FSM_ACT_NONE;

    case UPLINK_ONLINE:
        f->online_since_ms += dt_ms;
        if (fresh > 0u) {
            return FSM_ACT_SEND_FRESH;
        }
        return FSM_ACT_NONE;

    default:
        f->state = UPLINK_IDLE;
        return FSM_ACT_WAIT;
    }
}

const char *uplink_state_str(uplink_state_t s)
{
    switch (s) {
    case UPLINK_IDLE:       return "IDLE";
    case UPLINK_CONNECTING: return "CONNECTING";
    case UPLINK_DRAINING:   return "DRAINING";
    case UPLINK_ONLINE:     return "ONLINE";
    default:                return "?";
    }
}

const char *reconnect_action_str(reconnect_action_t a)
{
    switch (a) {
    case FSM_ACT_NONE:        return "NONE";
    case FSM_ACT_TRY_CONNECT: return "TRY_CONNECT";
    case FSM_ACT_DRAIN_ONE:   return "DRAIN_ONE";
    case FSM_ACT_SEND_FRESH:  return "SEND_FRESH";
    case FSM_ACT_WAIT:        return "WAIT";
    default:                  return "?";
    }
}
