#ifndef _MARKOV_CHAIN_H
#define _MARKOV_CHAIN_H

#include <linux/types.h>

#define MC_MAX_STATES 64  /* должно гарантировать вместимость */

struct dlc_state;
struct dlc_const_state;

struct markov_chain {
    struct dlc_state* states;
    u32 num_states;
    u32 curr_state;

    /* Вероятности scaled на 0..10000 (для точности 0.01%) */
    u16 transition_probs[MC_MAX_STATES][MC_MAX_STATES];

    /* начальное распределение состояний scaled на 0..10000 (0.01%) */
    u16 init_distribution[MC_MAX_STATES];
};

void markov_chain_init(struct markov_chain *mc, u32 num_states, 
                       struct dlc_state *states_array, 
                       u16 transition_probs[][MC_MAX_STATES],
                       u16 init_distribution[MC_MAX_STATES]);

struct dlc_state* markov_chain_step(struct markov_chain *mc);

void markov_chain_destroy(struct markov_chain *mc);


/* Used in dlc_queue_state_v2 to deal with cyclic dependency.*/
struct markov_chain_const {
    struct dlc_const_state* states;
    u32 num_states;
    u32 curr_state;

    u16 transition_probs[MC_MAX_STATES][MC_MAX_STATES];
    u16 init_distribution[MC_MAX_STATES];
};

void markov_chain_const_init(struct markov_chain_const *mc, u32 num_states, 
    struct dlc_const_state *states_array, 
    u16 transition_probs[][MC_MAX_STATES],
    u16 init_distribution[MC_MAX_STATES]);

struct dlc_const_state* markov_chain_const_step(struct markov_chain_const *mc);

void markov_chain_const_destroy(struct markov_chain_const *mc);

#endif
