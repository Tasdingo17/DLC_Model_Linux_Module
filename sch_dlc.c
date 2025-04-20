/*
    Modified copy of netem module
*/

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <linux/vmalloc.h>
#include <linux/rtnetlink.h>
#include <linux/reciprocal_div.h>
#include <linux/rbtree.h>

#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/inet_ecn.h>

#include "dlc/dlc_mod.h"
#include "dlc/dlc_tca_spec.h"


struct dlc_sched_data {
    /* internal t(ime)fifo qdisc uses t_root and sch->limit */
    struct rb_root t_root;

    /* a linear queue; reduces rbtree rebalancing when jitter is low */
    struct sk_buff  *t_head;
    struct sk_buff  *t_tail;

    u32 t_len;

    /* optional qdisc for classful handling (NULL at dlc init) */
    struct Qdisc  *qdisc;

    struct qdisc_watchdog watchdog;

    struct dlc_mod_data dlc_model;
    s64 latency;    // a.k.a delay
    s64 jitter;
    s64 mm1_rho;
    u32 jitter_steps;
    u32 loss;
    u32 mu;                     /* queue state prob */
    u32 mean_burst_len;         /* mean consequitive losses */
    u32 mean_good_burst_len;    /* mean consequitive queuined packets */

    u32 limit;
    u64 rate;

    struct disttable *delay_dist;
};

/* Time stamp put into socket buffer control block
* Only valid when skbs are in our internal t(ime)fifo queue.
*
* As skb->rbnode uses same storage than skb->next, skb->prev and skb->tstamp,
* and skb->next & skb->prev are scratch space for a qdisc,
* we save skb->tstamp value in skb->cb[] before destroying it.
*/
struct dlc_skb_cb {
    u64          time_to_send;
};

static inline struct dlc_skb_cb *dlc_skb_cb(struct sk_buff *skb)
{
    /* we assume we can use skb next/prev/tstamp as storage for rb_node */
    qdisc_cb_private_validate(skb, sizeof(struct dlc_skb_cb));
    return (struct dlc_skb_cb *)qdisc_skb_cb(skb)->data;
}


static u64 packet_time_ns(u64 len, const struct dlc_sched_data *q)
{
    return div64_u64(len * NSEC_PER_SEC, q->rate);
}

static void tfifo_reset(struct Qdisc *sch)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    struct rb_node *p = rb_first(&q->t_root);

    while (p) {
        struct sk_buff *skb = rb_to_skb(p);

        p = rb_next(p);
        rb_erase(&skb->rbnode, &q->t_root);
        rtnl_kfree_skbs(skb, skb);
    }

    rtnl_kfree_skbs(q->t_head, q->t_tail);
    q->t_head = NULL;
    q->t_tail = NULL;
    q->t_len = 0;
}

static void tfifo_enqueue(struct sk_buff *nskb, struct Qdisc *sch)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    u64 tnext = dlc_skb_cb(nskb)->time_to_send;

    if (!q->t_tail || tnext >= dlc_skb_cb(q->t_tail)->time_to_send) {
        if (q->t_tail)
            q->t_tail->next = nskb;
        else
            q->t_head = nskb;
        q->t_tail = nskb;
    } else {
        struct rb_node **p = &q->t_root.rb_node, *parent = NULL;

        while (*p) {
            struct sk_buff *skb;

            parent = *p;
            skb = rb_to_skb(parent);
            if (tnext >= dlc_skb_cb(skb)->time_to_send)
                p = &parent->rb_right;
            else
                p = &parent->rb_left;
        }
        rb_link_node(&nskb->rbnode, parent, p);
        rb_insert_color(&nskb->rbnode, &q->t_root);
    }
    q->t_len++;
    sch->q.qlen++;
}

/*
* Insert one skb into qdisc.
* Note: parent depends on return value to account for queue length.
*   NET_XMIT_DROP: queue length didn't change.
*      NET_XMIT_SUCCESS: one skb was queued.
*/
static int dlc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
            struct sk_buff **to_free)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    /* We don't fill cb now as skb_unshare() may invalidate it */
    struct dlc_skb_cb *cb;
    struct sk_buff *segs = NULL;
    int count = 1;
    u64 now = ktime_get_ns();

    struct dlc_packet_state pkt_state = dlc_mod_handle_packet(&(q->dlc_model), skb); // Call dlc_model
    s64 delay = pkt_state.delay;

    /* Do not fool qdisc_drop_all() */
    skb->prev = NULL;

    if (pkt_state.loss) {
        printk(KERN_DEBUG "Loss state, drop packet\n");
        --count;
    }
    if (count == 0) {
        qdisc_qstats_drop(sch);
        __qdisc_drop(skb, to_free);
        return NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
    }

    /* If a latency is expected, orphan the skb. (orphaning usually takes
    * place at TX completion time, so _before_ the link transit latency)
    */
    if (q->latency || q->jitter || q->rate)
        skb_orphan_partial(skb);


    if (unlikely(q->t_len >= sch->limit)) {
        /* re-link segs, so that qdisc_drop_all() frees them all */
        skb->next = segs;
        qdisc_drop_all(skb, sch, to_free);
        return NET_XMIT_DROP;
    }
    qdisc_qstats_backlog_inc(sch, skb);

    cb = dlc_skb_cb(skb);

    if (q->rate) {
        struct dlc_skb_cb *last = NULL;

        if (sch->q.tail)
            last = dlc_skb_cb(sch->q.tail);
        if (q->t_root.rb_node) {
            struct sk_buff *t_skb;
            struct dlc_skb_cb *t_last;

            t_skb = skb_rb_last(&q->t_root);
            t_last = dlc_skb_cb(t_skb);
            if (!last || t_last->time_to_send > last->time_to_send)
                last = t_last;
        }
        if (q->t_tail) {
            struct dlc_skb_cb *t_last = dlc_skb_cb(q->t_tail);

            if (!last || t_last->time_to_send > last->time_to_send)
                last = t_last;
        }

        if (last) {
            /*
            * Last packet in queue is reference point (now),
            * calculate this time bonus and subtract
            * from delay.
            */
            delay -= last->time_to_send - now;
            delay = max_t(s64, 0, delay);
            now = last->time_to_send;
        }

        delay += packet_time_ns(qdisc_pkt_len(skb), q);
    }

    cb->time_to_send = now + delay;
    tfifo_enqueue(skb, sch);

    return NET_XMIT_SUCCESS;
}

static struct sk_buff *dlc_peek(struct dlc_sched_data *q)
{
    struct sk_buff *skb = skb_rb_first(&q->t_root);
    u64 t1, t2;

    if (!skb)
        return q->t_head;
    if (!q->t_head)
        return skb;

    t1 = dlc_skb_cb(skb)->time_to_send;
    t2 = dlc_skb_cb(q->t_head)->time_to_send;
    if (t1 < t2)
        return skb;
    return q->t_head;
}

static void dlc_erase_head(struct dlc_sched_data *q, struct sk_buff *skb)
{
    if (skb == q->t_head) {
        q->t_head = skb->next;
        if (!q->t_head)
            q->t_tail = NULL;
    } else {
        rb_erase(&skb->rbnode, &q->t_root);
    }
}

static struct sk_buff *dlc_dequeue(struct Qdisc *sch)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    struct sk_buff *skb;

tfifo_dequeue:
    skb = __qdisc_dequeue_head(&sch->q);
    if (skb) {
deliver:
        qdisc_qstats_backlog_dec(sch, skb);
        qdisc_bstats_update(sch, skb);
        return skb;
    }
    skb = dlc_peek(q);
    if (skb) {
        u64 time_to_send;
        u64 now = ktime_get_ns();

        /* if more time remaining? */
        time_to_send = dlc_skb_cb(skb)->time_to_send;

        if (time_to_send <= now) {
            dlc_erase_head(q, skb);
            q->t_len--;
            skb->next = NULL;
            skb->prev = NULL;
            /* skb->dev shares skb->rbnode area,
            * we need to restore its value.
            */
            skb->dev = qdisc_dev(sch);

            if (q->qdisc) {
                unsigned int pkt_len = qdisc_pkt_len(skb);
                struct sk_buff *to_free = NULL;
                int err;

                err = qdisc_enqueue(skb, q->qdisc, &to_free);
                kfree_skb_list(to_free);
                if (err != NET_XMIT_SUCCESS) {
                    if (net_xmit_drop_count(err))
                        qdisc_qstats_drop(sch);
                    sch->qstats.backlog -= pkt_len;
                    sch->q.qlen--;
                    qdisc_tree_reduce_backlog(sch, 1, pkt_len);
                }
                goto tfifo_dequeue;
            }
            sch->q.qlen--;
            goto deliver;
        }

        if (q->qdisc) {
            skb = q->qdisc->ops->dequeue(q->qdisc);
            if (skb) {
                sch->q.qlen--;
                goto deliver;
            }
        }

        qdisc_watchdog_schedule_ns(&q->watchdog,time_to_send);
    }

    if (q->qdisc) {
        skb = q->qdisc->ops->dequeue(q->qdisc);
        if (skb) {
            sch->q.qlen--;
            goto deliver;
        }
    }
    return NULL;
}

static void dlc_reset(struct Qdisc *sch)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    qdisc_reset_queue(sch);
    tfifo_reset(sch);
    if (q->qdisc)
        qdisc_reset(q->qdisc);
    qdisc_watchdog_cancel(&q->watchdog);
}

static void dist_free(struct disttable *d)
{
    kvfree(d);
}

/*
* Distribution data is a variable size payload containing
* signed 16 bit values.
*/
static int get_dist_table(struct disttable **tbl, const struct nlattr *attr)
{
    size_t n = nla_len(attr)/sizeof(__s16);
    const __s16 *data = nla_data(attr);
    struct disttable *d;
    int i;

    if (!n || n > DLC_DIST_MAX)
        return -EINVAL;

    d = kvmalloc(sizeof(struct disttable) + n * sizeof(s16), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->size = n;
    for (i = 0; i < n; i++)
        d->table[i] = data[i];

    *tbl = d;
    return 0;
}


static int parse_attr(struct nlattr *tb[], int maxtype, struct nlattr *nla,
            const struct nla_policy *policy, int len)
{
    int nested_len = nla_len(nla) - NLA_ALIGN(len);

    if (nested_len < 0) {
        pr_info("dlc: invalid attributes len %d\n", nested_len);
        return -EINVAL;
    }

    if (nested_len >= nla_attr_size(0))
        return nla_parse_deprecated(tb, maxtype,
                        nla_data(nla) + NLA_ALIGN(len),
                        nested_len, policy, NULL);

    memset(tb, 0, sizeof(struct nlattr *) * (maxtype + 1));
    return 0;
}


static const struct nla_policy dlc_policy[TCA_DLC_MAX + 1] = {
    [TCA_DLC_RATE64]    = { .type = NLA_U64 },
    [TCA_DLC_LATENCY64] = { .type = NLA_S64 },
    [TCA_DLC_JITTER64]  = { .type = NLA_S64 },
};


/* Parse netlink message to set options */
static int dlc_change(struct Qdisc *sch, struct nlattr *opt,
            struct netlink_ext_ack *extack)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    struct nlattr *tb[TCA_DLC_MAX + 1];
    struct disttable *delay_dist = NULL;
    struct tc_dlc_qopt *qopt;
    int ret;

    printk(KERN_DEBUG "Dlc: parsing params from netlink message\n");

    if (opt == NULL)
        return -EINVAL;

    qopt = nla_data(opt);
    ret = parse_attr(tb, TCA_DLC_MAX, opt, dlc_policy, sizeof(*qopt));
    if (ret < 0)
        return ret;

    if (tb[TCA_DLC_DELAY_DIST]) {
        ret = get_dist_table(&delay_dist, tb[TCA_DLC_DELAY_DIST]);
        if (ret)
            goto table_free;
    }

    sch_tree_lock(sch);

    if (delay_dist)
        swap(q->delay_dist, delay_dist); // important
    sch->limit = qopt->limit;

    q->latency = PSCHED_TICKS2NS(qopt->latency);
    q->jitter = PSCHED_TICKS2NS(qopt->jitter);
    q->mm1_rho = (s64) qopt->mm1_rho;
    q->jitter_steps = qopt->jitter_steps;
    q->loss = qopt->loss;
    q->mu = qopt->mu;
    q->mean_burst_len = qopt->mean_burst_len;
    q->mean_good_burst_len = qopt->mean_good_burst_len;

    q->limit = qopt->limit;
    q->rate  = qopt->rate;

    if (tb[TCA_DLC_LATENCY64])
		q->latency = nla_get_s64(tb[TCA_DLC_LATENCY64]);
    if (tb[TCA_DLC_JITTER64])
        q->jitter = nla_get_s64(tb[TCA_DLC_JITTER64]);
    if (tb[TCA_DLC_RATE64])
        q->rate = max_t(u64, q->rate, nla_get_u64(tb[TCA_DLC_RATE64]));

    
    /* capping jitter to the range acceptable by tabledist() */
    q->jitter = min_t(s64, abs(q->jitter), INT_MAX);

    dlc_mod_init(&(q->dlc_model),
                 q->latency, q->jitter, q->mm1_rho, q->jitter_steps,
                 q->loss, q->mu, q->mean_burst_len, q->mean_good_burst_len,
                 q->delay_dist);

    sch_tree_unlock(sch);

table_free:
    dist_free(delay_dist);
    return ret;
}

static int dlc_init(struct Qdisc *sch, struct nlattr *opt,
            struct netlink_ext_ack *extack)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    int ret;

    qdisc_watchdog_init(&q->watchdog, sch);

    if (!opt)
        return -EINVAL;

    ret = dlc_change(sch, opt, extack);
    if (ret)
        pr_info("dlc: change failed\n");
    return ret;
}

static void dlc_destroy(struct Qdisc *sch)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    qdisc_watchdog_cancel(&q->watchdog);
    if (q->qdisc)
        qdisc_put(q->qdisc);
    dist_free(q->delay_dist);
    dlc_mod_destroy(&(q->dlc_model));
}

static int dump_markov_chain(const struct dlc_sched_data *q, struct sk_buff *skb)
{
    struct nlattr *nest;
    const struct dlc_mod_data* model = &(q->dlc_model);
    struct tc_dlc_simple_state simple_state;
    struct tc_dlc_queue_state queue_state;
    struct tc_dlc_loss_state loss_state;
    psched_time_t delay_ticks, jitter_ticks;

    nest = nla_nest_start_noflag(skb, TCA_MARKOV_CHAIN);
    if (nest == NULL)
        goto nla_put_failure;


    if (model->main_chain.states[0].type != DLC_STATE_SIMPLE){
        pr_warning("dlc: invalid state[0] type %d\n", model->main_chain.states[0].type);
        goto nla_put_failure;
    }
    delay_ticks = PSCHED_NS2TICKS(model->main_chain.states[0].simple.delay_mean);
    jitter_ticks = PSCHED_NS2TICKS(model->main_chain.states[0].simple.jitter);

    simple_state.delay = delay_ticks > UINT_MAX ? UINT_MAX : delay_ticks;
    simple_state.jitter = jitter_ticks > UINT_MAX ? UINT_MAX : jitter_ticks;
    simple_state.delaydist_size = sizeof(model->main_chain.states[0].simple.distr);
    memcpy(simple_state.trans_probs, model->main_chain.transition_probs[0], sizeof(simple_state.trans_probs));
    if (nla_put(skb, MC_STATE_SIMPLE, sizeof(simple_state), &simple_state))
        goto nla_put_failure;


    if (model->main_chain.states[1].type != DLC_STATE_QUEUE_V2){
        pr_warning("dlc: invalid state[1] type %d\n", model->main_chain.states[1].type);
        goto nla_put_failure;
    }
    queue_state.mm1k_num_states = model->main_chain.states[1].queue.mm1k_chain.num_states;
    memcpy(queue_state.trans_probs, model->main_chain.transition_probs[1], sizeof(queue_state.trans_probs));
    if (nla_put(skb, MC_STATE_QUEUE, sizeof(queue_state), &queue_state))
        goto nla_put_failure;


    if (model->main_chain.states[2].type != DLC_STATE_LOSS){
        pr_warning("dlc: invalid state[2] type %d\n", model->main_chain.states[2].type);
        goto nla_put_failure;
    }
    delay_ticks = PSCHED_NS2TICKS(model->main_chain.states[2].loss.max_delay);
    loss_state.max_delay = delay_ticks > UINT_MAX ? UINT_MAX : delay_ticks;
    memcpy(loss_state.trans_probs, model->main_chain.transition_probs[2], sizeof(loss_state.trans_probs));
    if (nla_put(skb, MC_STATE_LOSS, sizeof(loss_state), &loss_state))
        goto nla_put_failure;

    nla_nest_end(skb, nest);
    return 0;

nla_put_failure:
    nla_nest_cancel(skb, nest);
    return -1;
}

static int dump_dlc_model(const struct dlc_sched_data *q,
            struct sk_buff *skb)
{
    struct tc_dlc_model model = {
        .mc_num_states  = q->dlc_model.main_chain.num_states,
        .mc_curr_state  = q->dlc_model.main_chain.curr_state,
        .delaydist_size = sizeof(q->dlc_model.delay_dist)
    };

    if (nla_put(skb, TCA_DLC_MODEL, sizeof(model), &model))
        goto nla_put_failure;

    if (dump_markov_chain(q, skb) != 0)
        goto nla_put_failure;

    return 0;

nla_put_failure:
    return -1;
}

static int dlc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    const struct dlc_sched_data *q = qdisc_priv(sch);
    struct nlattr *nla = (struct nlattr *) skb_tail_pointer(skb);
    struct tc_dlc_qopt qopt;

    qopt.latency = min_t(psched_time_t, PSCHED_NS2TICKS(q->latency), UINT_MAX);
    qopt.jitter = min_t(psched_time_t, PSCHED_NS2TICKS(q->jitter), UINT_MAX);
    qopt.mm1_rho = q->mm1_rho;
    qopt.jitter_steps = q->jitter_steps;
    qopt.mu = q->mu;
    qopt.mean_burst_len = q->mean_burst_len;
    qopt.mean_good_burst_len = q-> mean_good_burst_len;
    qopt.loss = q->loss;
    qopt.limit = q->limit;

    if (nla_put(skb, TCA_NETEM_LATENCY64, sizeof(q->latency), &q->latency))
        goto nla_put_failure;
    if (nla_put(skb, TCA_NETEM_JITTER64, sizeof(q->jitter), &q->jitter))
        goto nla_put_failure;

    if (q->rate >= (1ULL << 32)) {
        if (nla_put_u64_64bit(skb, TCA_DLC_RATE64, q->rate, TCA_DLC_PAD))
            goto nla_put_failure;
        qopt.rate = ~0U;
    } else {
        qopt.rate = q->rate;
    }

    if (nla_put(skb, TCA_OPTIONS, sizeof(qopt), &qopt))
        goto nla_put_failure;

    if (dump_dlc_model(q, skb) != 0)
        goto nla_put_failure;

    return nla_nest_end(skb, nla);

nla_put_failure:
    nlmsg_trim(skb, nla);
    return -1;
}

static int dlc_dump_class(struct Qdisc *sch, unsigned long cl,
            struct sk_buff *skb, struct tcmsg *tcm)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    if (cl != 1 || !q->qdisc)   /* only one class */
        return -ENOENT;

    tcm->tcm_handle |= TC_H_MIN(1);
    tcm->tcm_info = q->qdisc->handle;

    return 0;
}

static int dlc_graft(struct Qdisc *sch, unsigned long arg, struct Qdisc *new,
            struct Qdisc **old, struct netlink_ext_ack *extack)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    *old = qdisc_replace(sch, new, &q->qdisc);
    return 0;
}

static struct Qdisc *dlc_leaf(struct Qdisc *sch, unsigned long arg)
{
    struct dlc_sched_data *q = qdisc_priv(sch);
    return q->qdisc;
}

static unsigned long dlc_find(struct Qdisc *sch, u32 classid)
{
    return 1;
}

static void dlc_walk(struct Qdisc *sch, struct qdisc_walker *walker)
{
    if (!walker->stop) {
        if (walker->count >= walker->skip)
            if (walker->fn(sch, 1, walker) < 0) {
                walker->stop = 1;
                return;
            }
        walker->count++;
    }
}

static const struct Qdisc_class_ops dlc_class_ops = {
    .graft    =  dlc_graft,
    .leaf    =  dlc_leaf,
    .find    =  dlc_find,
    .walk    =  dlc_walk,
    .dump    =  dlc_dump_class,
};

/* TODO: check ahahahah*/
static struct Qdisc_ops dlc_qdisc_ops __read_mostly = {
    .id    =  "dlc",
    .cl_ops    =  &dlc_class_ops,
    .priv_size  =  sizeof(struct dlc_sched_data),
    .enqueue  =  dlc_enqueue,
    .dequeue  =  dlc_dequeue,
    .peek    =  qdisc_peek_dequeued,
    .init    =  dlc_init,
    .reset    =  dlc_reset,
    .destroy  =  dlc_destroy,
    .change    =  dlc_change,
    .dump    =  dlc_dump,
    .owner    =  THIS_MODULE,
};


static int __init dlc_module_init(void)
{
    pr_info("dlc_model register \n");
    return register_qdisc(&dlc_qdisc_ops);
}
static void __exit dlc_module_exit(void)
{
    unregister_qdisc(&dlc_qdisc_ops);
}
module_init(dlc_module_init)
module_exit(dlc_module_exit)
MODULE_LICENSE("GPL");
