#include "states.h"
#include <linux/slab.h>


void dlc_const_state_init(struct dlc_const_state *state, s64 delay){
    state->delay = delay;
}

struct dlc_packet_state dlc_const_state_step(struct dlc_const_state *state) {
    struct dlc_packet_state res = {
        .delay = state->delay,
        .loss = false
    };
    return res;
}


void dlc_simple_state_init(struct dlc_simple_state *state, 
                           s64 delay_mean,
                           s64 jitter,
                           struct disttable* distr
                        )
{
    state->delay_mean = delay_mean;
    state->jitter = jitter;
    state->distr = distr;
}

struct dlc_packet_state dlc_simple_state_step(struct dlc_simple_state *state) {
    struct dlc_packet_state res = {
        .delay = tabledist(state->delay_mean, state->jitter, state->distr),
        .loss = false
    };
    return res;
}


void dlc_loss_state_init(struct dlc_loss_state *state, s64 max_delay) {
    state->max_delay = max_delay;
}

struct dlc_packet_state dlc_loss_state_step(struct dlc_loss_state *state) {
    struct dlc_packet_state res = {
        .delay = state->max_delay,
        .loss = true
    };
    return res;
}


void dlc_queue_state_v2_init(struct dlc_queue_state_v2 *state, u32 num_steps, s64 delay, s64 jitter, s64 rho){
    s64 delay_step = jitter / num_steps;
    u32 k_states = num_steps + 1;

    u16 trans_probs[MC_MAX_STATES][MC_MAX_STATES];   // CHECK: kalloc maybe?
    
    s64 p_min = (10000 * 10000) / (10000 + rho);    // todo: check scaling just in case
    s64 p_plus = rho * 10000 / (10000 + rho);    
    trans_probs[0][0] = 10000 - p_plus;
    trans_probs[0][1] = p_plus;
    res[k_states - 1][k_states - 1] = 10000 - p_min;
    res[k_states - 1][k_states - 2] = p_min;
    for (int i = 1; i < k_states - 1; i++){
        trans_probs[i][i-1] = p_min;
        trans_probs[i][i+1] = p_plus;
    }

    for (int i = 0; i < k_states; i++) {
        state->mm1k_states[i].type = DLC_STATE_CONST;
        dlc_const_state_init(&state->mm1k_states[i].simple, delay + i * delay_step);
    }
    u16 init_probs[MC_MAX_STATES];
    init_probs[0] = 10000;  // TODO: general const for percentage scale
    markov_chain_init(&state->mm1k_chain, k_states, state->mm1k_states, trans_probs, init_probs);  // note: memcpy on prob arrays
}

struct dlc_packet_state dlc_queue_state_v2_step(struct dlc_queue_state_v2 *state) {
    struct dlc_state *curr_state = markov_chain_step(&state->mm1k_chain);
    return dlc_const_state_step(&curr_state->simple);
}
