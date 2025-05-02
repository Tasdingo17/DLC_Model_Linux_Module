#ifndef _DLC_MOD_H
#define _DLC_MOD_H

#include "states.h"

#include <linux/types.h>
#include <linux/skbuff.h>

#define DLC_NUM_STATES 3

struct dlc_mod_data {
    /* Markov chain for overall DLC model */
    struct markov_chain main_chain;

    /* Pre-built distribution tables (from netem), passed from userspace setup */
    struct disttable* delay_dist;   /* read only */
};

/* Called on init, from tc/netlink or sysfs */
int dlc_mod_init(struct dlc_mod_data *dlc_data,
                 s64 delay,
                 s64 jitter,
                 s64 mm1_rho,
                 u32 jitter_steps,
                 u32 p_loss,
                 u32 mu,
                 u32 mean_burst_len,
                 u32 mean_good_burst_len,
                 struct disttable* dist
);

/* Called per packet */
struct dlc_packet_state dlc_mod_handle_packet(struct dlc_mod_data *dlc_data, struct sk_buff *skb);

void dlc_mod_destroy(struct dlc_mod_data *dlc_data);

#endif
