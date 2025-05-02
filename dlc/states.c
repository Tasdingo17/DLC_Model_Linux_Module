#include "states.h"
#include <linux/slab.h>
#include <linux/mm.h>

int dlc_const_state_init(struct dlc_const_state *state, s64 delay){
    state->delay = delay;
    return 0;
}

struct dlc_packet_state dlc_const_state_step(struct dlc_const_state *state) {
    struct dlc_packet_state res = {
        .delay = state->delay,
        .loss = false
    };
    return res;
}


int dlc_simple_state_init(struct dlc_simple_state *state, 
                           s64 delay_mean,
                           s64 jitter,
                           struct disttable* distr
                        )
{
    state->delay_mean = delay_mean;
    state->jitter = jitter;
    state->distr = distr;
    return 0;
}

struct dlc_packet_state dlc_simple_state_step(struct dlc_simple_state *state) {
    struct dlc_packet_state res = {
        .delay = tabledist(state->delay_mean, state->jitter, state->distr),
        .loss = false
    };
    return res;
}


int dlc_loss_state_init(struct dlc_loss_state *state, s64 max_delay) {
    state->max_delay = max_delay;
    return 0;
}

struct dlc_packet_state dlc_loss_state_step(struct dlc_loss_state *state) {
    struct dlc_packet_state res = {
        .delay = state->max_delay,
        .loss = true
    };
    return res;
}


int dlc_queue_state_v2_init(struct dlc_queue_state_v2 *state, u32 num_steps, s64 delay, s64 jitter, s64 rho){
    int i = 0;
    s64 p_min, p_plus;
    s64 delay_step = jitter / num_steps;
    u32 k_states = num_steps + 1;
    struct dlc_const_state* const_states;
    u32* init_probs;
    u32 (*trans_probs)[MC_MAX_STATES];  

    // allocate memory since kernel stack is too small
    const_states = kvmalloc(sizeof(struct dlc_const_state) * MC_MAX_STATES, GFP_KERNEL);
    if (!const_states)
        return -ENOMEM;
    init_probs = kvmalloc(sizeof(u32) * MC_MAX_STATES, GFP_KERNEL);
    if (!init_probs){
        kvfree(const_states);
        return -ENOMEM;
    }
    trans_probs = kvmalloc(sizeof(u32[MC_MAX_STATES][MC_MAX_STATES]), GFP_KERNEL);
    if (!trans_probs) {
        kvfree(const_states);
        kvfree(init_probs);
        return -ENOMEM;
    }
    // struct dlc_const_state const_states[MC_MAX_STATES];
    // u32 init_probs[MC_MAX_STATES];
    // u32 trans_probs[MC_MAX_STATES][MC_MAX_STATES]; 

    for (i = 0; i < k_states; i++) {
        dlc_const_state_init(&(const_states[i]), delay + i * delay_step);
    }

    p_min = ((s64) DLC_PROB_SCALE * DLC_PROB_SCALE) / (DLC_PROB_SCALE + rho);    // todo: check scaling just in case
    p_plus = ((s64) DLC_PROB_SCALE * rho) / (DLC_PROB_SCALE + rho);    
    printk(KERN_DEBUG "DLC: state_queue p_min=%lld, p_plus=%lld\n", p_min, p_plus);
    trans_probs[0][0] = DLC_PROB_SCALE - p_plus;
    trans_probs[0][1] = p_plus;
    trans_probs[k_states - 1][k_states - 1] = DLC_PROB_SCALE - p_min;
    trans_probs[k_states - 1][k_states - 2] = p_min;
    for (i = 1; i < k_states - 1; i++){
        trans_probs[i][i-1] = p_min;
        trans_probs[i][i+1] = p_plus;
    }

    init_probs[0] = DLC_PROB_SCALE;
    markov_chain_const_init(&state->mm1k_chain, k_states, const_states, trans_probs, init_probs);  // note: memcpy on arrays

    // cleanup since markov_chain_init copy arrays to itself
    kvfree(const_states);
    kvfree(init_probs);
    kvfree(trans_probs);
    return 0;
}

struct dlc_packet_state dlc_queue_state_v2_step(struct dlc_queue_state_v2 *state) {
    struct dlc_const_state *curr_state = markov_chain_const_step(&state->mm1k_chain);
    return dlc_const_state_step(curr_state);
}
