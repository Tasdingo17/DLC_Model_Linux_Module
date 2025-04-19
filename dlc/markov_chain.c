#include "markov_chain.h"
#include "states.h"
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/kernel.h>

static u32 select_initial_state(u32 num_states, u16 init_distribution[]) {
    u16 rnd = get_random_u32() % 10000;
    u16 cum_prob = 0;
    u32 i;

    for (i = 0; i < num_states; i++) {
        cum_prob += init_distribution[i];
        if (rnd < cum_prob)
            return i;
    }
    return num_states - 1; /* защита от ошибки округления */
}

static u32 calc_next_state_idx(u32 curr_state, u32 num_states, u16 transition_probs[][MC_MAX_STATES]){
    u16 rnd = get_random_u32() % 10000;
    u32 next_state = num_states;
    u16 cum_prob = 0;
    u32 i;

    for (i = 0; i < num_states; i++) {
        cum_prob += transition_probs[curr_state][i];
        if (rnd < cum_prob) {
            next_state = i;
            break;
        }
    }
    if (next_state == num_states) {
        pr_info("dlc_model: failed to calc next state, use 0\n");
        next_state = 0;
    }
    return next_state;
}

void markov_chain_init(struct markov_chain *mc, u32 num_states, 
                       struct dlc_state *states_array, 
                       u16 transition_probs[][MC_MAX_STATES],
                       u16 init_distribution[MC_MAX_STATES])
{
    u32 i, j;

    if (num_states > MC_MAX_STATES)
        pr_info("dlc_model: num_states (%d) too big, cut to %d\n", num_states, MC_MAX_STATES);
        num_states = MC_MAX_STATES;
    mc->num_states = num_states;
    mc->states = kvmalloc(sizeof(struct dlc_state) * num_states, GFP_KERNEL);
    if (!mc->states){
        pr_err("dlc_model: failed to allocate memory for states\n");
        return;
    }
    memcpy(mc->states, states_array, sizeof(struct dlc_state) * num_states);
    for (i = 0; i < num_states; i++) {
        for (j = 0; j < num_states; j++) {
            mc->transition_probs[i][j] = transition_probs[i][j];
        }
    }

    memcpy(mc->init_distribution, init_distribution, sizeof(u16) * num_states);

    /* выбор начального состояния согласно начальному распределению */
    mc->curr_state = select_initial_state(num_states, mc->init_distribution);
}

struct dlc_state* markov_chain_step(struct markov_chain *mc) {
    u32 next_state = calc_next_state_idx(mc->curr_state, mc->num_states, mc->transition_probs);
    mc->curr_state = next_state;
    return &mc->states[mc->curr_state];
}

void markov_chain_destroy(struct markov_chain *mc){
    kvfree(&(mc->states));
}

///////////////////////////

void markov_chain_const_init(struct markov_chain_const *mc, u32 num_states, 
    struct dlc_const_state *states_array, 
    u16 transition_probs[][MC_MAX_STATES],
    u16 init_distribution[MC_MAX_STATES])
{
    u32 i, j;

    if (num_states > MC_MAX_STATES)
    pr_info("dlc_model: num_states (%d) too big, cut to %d\n", num_states, MC_MAX_STATES);
    num_states = MC_MAX_STATES;
    mc->num_states = num_states;
    mc->states = kvmalloc(sizeof(struct dlc_const_state) * num_states, GFP_KERNEL);
    if (!mc->states){
        pr_err("dlc_model: failed to allocate memory for states\n");
        return;
    }
    memcpy(mc->states, states_array, sizeof(struct dlc_const_state) * num_states);

    for (i = 0; i < num_states; i++) {
        for (j = 0; j < num_states; j++) {
            mc->transition_probs[i][j] = transition_probs[i][j];
        }
    }
    memcpy(mc->init_distribution, init_distribution, sizeof(u16) * num_states);
    mc->curr_state = select_initial_state(num_states, mc->init_distribution);
}

struct dlc_const_state* markov_chain_const_step(struct markov_chain_const *mc) {
    u32 next_state = calc_next_state_idx(mc->curr_state, mc->num_states, mc->transition_probs);
    mc->curr_state = next_state;
    return &mc->states[mc->curr_state];
}

void markov_chain_const_destroy(struct markov_chain_const *mc){
    kvfree(&(mc->states));
}
