#include "dlc_random.h"

#define NETEM_DIST_SCALE    8192

/* tabledist - return a pseudo-randomly distributed value with mean mu and
 * std deviation sigma.  Uses table lookup to approximate the desired
 * distribution, and a uniformly-distributed pseudo-random source.
 */
s64 tabledist(s64 mu, s32 sigma, const struct disttable *dist)
{
    s64 x;
    long t;
    u32 rnd;

    if (sigma == 0)
        return mu;

    rnd = prandom_u32();

    /* default uniform distribution */
    if (dist == NULL)
        return ((rnd % (2 * (u32)sigma)) + mu) - sigma;

    t = dist->table[rnd % dist->size];
    x = (sigma % NETEM_DIST_SCALE) * t;
    if (x >= 0)
        x += NETEM_DIST_SCALE/2;
    else
        x -= NETEM_DIST_SCALE/2;

    return  x / NETEM_DIST_SCALE + (sigma / NETEM_DIST_SCALE) * t + mu;
}