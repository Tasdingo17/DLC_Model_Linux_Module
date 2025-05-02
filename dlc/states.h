#ifndef _DLC_STATES_H
#define _DLC_STATES_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/random.h>

#include "dlc_random.h"
#include "markov_chain.h"


/* перечисление для типов состояний */
typedef enum {
    DLC_STATE_CONST,
    DLC_STATE_SIMPLE,
    DLC_STATE_QUEUE_V2,
    DLC_STATE_LOSS
} dlc_state_type_t;


/* {delay, loss} ~ {const, 0} */
struct dlc_const_state {
    s64 delay;
};

/* {delay, loss} ~ {norm(), 0} */
struct dlc_simple_state {
    s64 delay_mean;       /* среднее значение задержки в мкс */
    s64 jitter;           /* jitter в мкс */
    struct disttable* distr;    /* read-only */
};

/* {delay, loss} ~ {M/M/1/K(), 0} */
struct dlc_queue_state_v2 {
    /* 
    Could use pure markov_chain, but cyclic dependency require big amount of memory.
    If still want to use it, note that function stack size in kernel is small.
    Will require a little more effort on memory management: dynamic allocation and free.
    */
    struct markov_chain_const mm1k_chain;
};

/* {delay, loss} ~ {max_delay, 1} */
struct dlc_loss_state {
    s64 max_delay;
};

/* Общий union для всех состояний */
struct dlc_state {
    dlc_state_type_t type;
    union {
        struct dlc_const_state cnst;
        struct dlc_simple_state simple;
        struct dlc_queue_state_v2 queue;
        struct dlc_loss_state loss;
    };
};

/* Возвращаемые характеристики для пакета */
struct dlc_packet_state {
    s64 delay;
    bool loss;
};

int dlc_const_state_init(struct dlc_const_state *state, s64 delay);
struct dlc_packet_state dlc_const_state_step(struct dlc_const_state *state);

int dlc_simple_state_init(struct dlc_simple_state *state, 
                           s64 delay_mean,
                           s64 jitter,
                           struct disttable *delay_dist);
struct dlc_packet_state dlc_simple_state_step(struct dlc_simple_state *state);

int dlc_loss_state_init(struct dlc_loss_state *state, s64 max_delay);
struct dlc_packet_state dlc_loss_state_step(struct dlc_loss_state *state);

// rho = lambda/mu (general naming for M/M/1/k); scaled by 10000... (% 10000)
int dlc_queue_state_v2_init(struct dlc_queue_state_v2 *state, u32 num_steps, s64 delay, s64 jitter, s64 rho);
struct dlc_packet_state dlc_queue_state_v2_step(struct dlc_queue_state_v2 *state);

#endif
