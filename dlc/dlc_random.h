#ifndef _DLC_RANDOM_H
#define _DLC_RANDOM_H

#include <linux/types.h>
#include <linux/random.h>

struct disttable {
    u32 size;
    s16 table[0];
};

s64 tabledist(s64 mu, s32 sigma, const struct disttable *dist);

// 100% = 1.0 = P(X) = 1 = dlc_prob_scale; 
#define DLC_PROB_SCALE 100000

#endif
