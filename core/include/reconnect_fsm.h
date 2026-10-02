/*
 * reconnect_fsm.h - uplink reconnect / re-drain state machine.
 *
 * The gateway never blocks acquisition on the uplink. Instead the uplink is a
 * state machine driven by a tick:
 *
 *   IDLE ──(link available)──> CONNECTING ──(ok)──> DRAINING ──(cache empty)──> ONLINE
 *     ^                            │                    │                        │
 *     └────────(link lost)─────────┴────────────────────┴────────────────────────┘
 *
 * DRAINING is a separate state from ONLINE on purpose: while the backlog is
 * being flushed the fresh samples must not overtake the backlog, otherwise the
 * server sees an out-of-order history.
 */
#ifndef GATEWAY_RECONNECT_FSM_H
#define GATEWAY_RECONNECT_FSM_H

#include <stdint.h>

typedef enum {
    UPLINK_IDLE = 0,
    UPLINK_CONNECTING,
    UPLINK_DRAINING,
    UPLINK_ONLINE
} uplink_state_t;

typedef struct {
    uplink_state_t state;
    uint32_t       backoff_ms;      /* current retry delay */
    uint32_t       backoff_min_ms;
    uint32_t       backoff_max_ms;
    uint32_t       elapsed_ms;      /* time spent in CONNECTING */
    uint32_t       connect_ok;      /* statistics */
    uint32_t       connect_fail;
    uint32_t       drained_records;
    uint32_t       online_since_ms;
} reconnect_fsm_t;

/* Actions the caller must perform after a tick. */
typedef enum {
    FSM_ACT_NONE = 0,
    FSM_ACT_TRY_CONNECT,
    FSM_ACT_DRAIN_ONE,
    FSM_ACT_SEND_FRESH,
    FSM_ACT_WAIT
} reconnect_action_t;

void reconnect_fsm_init(reconnect_fsm_t *f, uint32_t backoff_min_ms, uint32_t backoff_max_ms);

/*
 * Advance the state machine.
 *
 *   link_up   : transport reports the socket is connected
 *   backlog   : number of records still waiting in the offline cache
 *   fresh     : number of fresh samples waiting to be sent
 *
 * Returns the next action the caller should take.
 */
reconnect_action_t reconnect_fsm_tick(reconnect_fsm_t *f, uint32_t dt_ms,
                                      int link_up, uint32_t backlog, uint32_t fresh);

void reconnect_fsm_on_connect_result(reconnect_fsm_t *f, int ok);
void reconnect_fsm_on_link_lost(reconnect_fsm_t *f);

const char *uplink_state_str(uplink_state_t s);
const char *reconnect_action_str(reconnect_action_t a);

#endif /* GATEWAY_RECONNECT_FSM_H */
