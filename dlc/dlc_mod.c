#include "dlc_mod.h"
#include <linux/random.h>
#include <linux/kernel.h>

void _set_dlc_init_probs(u16* init_probs){
    init_probs[0] = 10000;
    init_probs[1] = 0;
    init_probs[2] = 0;
    return;
}

/*probs are scaled to 10000*/
void _set_dlc_transition_probs(
        u16 transition_probs[MC_MAX_STATES][MC_MAX_STATES],
        u32 p_loss,
        u32 mu,
        u32 mean_burst_len,
        u32 mean_good_burst_len
){
    u16 p32 = 10000 / mean_burst_len;
    u16 t = (10000 / (10000 - p_loss) - 10000) / (mean_burst_len * (10000 - mu));
    u16 p23 = ((10000 - mu) / mu) * t;
    u16 p21 = 10000 / mean_good_burst_len - p23;
    u16 p12 = mu / ((10000 - mu) * mean_good_burst_len) - t;
    transition_probs[0][0] = 1 - p12;
    transition_probs[0][1] = p12;
    transition_probs[0][2] = 0;
    transition_probs[1][0] = p21;
    transition_probs[1][1] = 1 - p21 - p23;
    transition_probs[1][2] = p23;
    transition_probs[2][0] = 0;
    transition_probs[2][1] = p32;
    transition_probs[2][2] = 1 - p32;
    return;
};

int dlc_mod_init(
        struct dlc_mod_data *dlc_data,
        s64 delay,
        s64 jitter,
        s64 mm1_rho,
        u32 jitter_steps,
        u32 p_loss,
        u32 mu,
        u32 mean_burst_len,
        u32 mean_good_burst_len,
        struct disttable* dist
) {
    struct dlc_state* states;
    struct dlc_simple_state simple_state;
    struct dlc_queue_state_v2 queue_state;
    struct dlc_loss_state loss_state;
    u16 (*transition_probs)[MC_MAX_STATES]; 
    u16* init_probs;
    
    // allocate memory since kernel stack is too small
    states = kvmalloc(sizeof(struct dlc_state) * DLC_NUM_STATES, GFP_KERNEL);
    if (!states)
        return -ENOMEM;

    dlc_data->delay_dist.size = dist->size;
    memcpy(dlc_data->delay_dist.table, dist->table, sizeof(u16) * dist->size);

    dlc_simple_state_init(&simple_state, delay, jitter, &(dlc_data->delay_dist));
    states[0].type = DLC_STATE_SIMPLE;
    states[0].simple = simple_state;

    dlc_queue_state_v2_init(&queue_state, jitter_steps, delay, jitter, mm1_rho);
    states[1].type = DLC_STATE_QUEUE_V2;
    states[1].queue = queue_state;

    dlc_loss_state_init(&loss_state, delay + jitter);
    states[2].type = DLC_STATE_LOSS;
    states[2].loss = loss_state;

    init_probs = kvmalloc(sizeof(u16) * MC_MAX_STATES, GFP_KERNEL);
    if (!init_probs){
        kvfree(states);
        return -ENOMEM;
    }
    _set_dlc_init_probs(init_probs);
    transition_probs = kvmalloc(sizeof(u16[MC_MAX_STATES][MC_MAX_STATES]), GFP_KERNEL);
    if (!transition_probs) {
        kvfree(states);
        kvfree(init_probs);
        return -ENOMEM;
    }
    _set_dlc_transition_probs(transition_probs, p_loss, mu, mean_burst_len, mean_good_burst_len);

    markov_chain_init(&dlc_data->main_chain, DLC_NUM_STATES, states, transition_probs, init_probs); // note: memcpy on array
    printk(KERN_INFO "DLC module initialized\n");

    // cleanup since markov_chain_init copy arrays to itself
    kvfree(states);
    kvfree(init_probs);
    kvfree(transition_probs);
    return 0;
}

struct dlc_packet_state dlc_mod_handle_packet(struct dlc_mod_data *dlc_data, struct sk_buff *skb)
{ 
    struct dlc_state *state;
    struct dlc_packet_state pkt_state;

    state = markov_chain_step(&dlc_data->main_chain);
    switch (state->type) {
        case DLC_STATE_SIMPLE:
            pkt_state = dlc_simple_state_step(&state->simple);
            break;
        case DLC_STATE_QUEUE_V2:
            pkt_state = dlc_queue_state_v2_step(&state->queue);
            break;
        case DLC_STATE_LOSS:
            pkt_state = dlc_loss_state_step(&state->loss);
            break;
        default:
            printk(KERN_WARNING "Got inappropriate state type, mark packet as dropped\n");
            pkt_state.loss = true;
            break;
    }
    return pkt_state;
}

void dlc_mod_destroy(struct dlc_mod_data *dlc_data){
    int i;
    kvfree(&(dlc_data->delay_dist));
    for (i = 0; i < dlc_data->main_chain.num_states; i++){
        if (dlc_data->main_chain.states[i].type == DLC_STATE_QUEUE_V2){ // write here all states with internal chains
            markov_chain_const_destroy(&(dlc_data->main_chain.states[i].queue.mm1k_chain));
        }
    }
    markov_chain_destroy(&(dlc_data->main_chain));
}
