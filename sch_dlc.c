#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/pkt_sched.h>
#include <linux/kernel.h>

#include "dlc/dlc_mod.h"
#include "dlc/dlc_tca_spec.h"

struct dlc_sched_data {
    /* internal t(ime)fifo qdisc uses t_root and sch->limit */
    struct rb_root t_root;

    /* a linear queue; reduces rbtree rebalancing when jitter is low */
    struct sk_buff  *t_head;
    struct sk_buff  *t_tail;

    u32 t_len;

    /* optional qdisc for classful handling (NULL at netem init) */
    struct Qdisc *qdisc;
    struct qdisc_watchdog watchdog;

    struct dlc_mod_data dlc; // наша модель
    s64 delay;
    s64 jitter;
    s64 mm1_rho;
    u32 jitter_steps;
    u32 p_loss;
    u32 mu;
    u32 mean_burst_len;
    u32 mean_good_burst_len;
    struct disttable* dist;
    };

static int dlc_init(struct Qdisc *sch, struct nlattr *opt, struct netlink_ext_ack *extack)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    /* Здесь производится инициализация структуры dlc_mod_data q->dlc */
    /* Параметры, вероятности (markov), таблицы распределений получаем из netlink/tc */

    /* Просто пример (вероятности, таблицы delay должны передаваться извне!) */
    dlc_mod_init(&q->dlc, ...);

    return 0;
}

// static int dlc_init(struct Qdisc *sch, struct nlattr *opt, struct netlink_ext_ack *extack)
// {
//     struct dlc_sched_data *q = qdisc_priv(sch);
//     struct nlattr *tb[TCA_DLC_MAX + 1];
    
//     if (!opt)
//         return -EINVAL;

//     // парсим атрибуты, заданные из userspace
//     nla_parse_nested(tb, TCA_DLC_MAX, opt, dlc_policy, extack);
    
//     if (!tb[TCA_DLC_DELAY_MEAN] || !tb[TCA_DLC_JITTER])
//         return -EINVAL;

//     q->delay_mean = nla_get_u32(tb[TCA_DLC_DELAY_MEAN]);
//     q->jitter = nla_get_u32(tb[TCA_DLC_JITTER]);
//     // аналогично парсим другие параметры...

//     // после разбора вызываем инициализацию модели
//     dlc_mod_init(&q->dlc, ...);
//     return 0;
// }

static int dlc_enqueue(struct sk_buff *skb, struct Qdisc *sch, struct sk_buff **to_free)
{
    struct dlc_sched_data *q = qdisc_priv(sch);

    /* Обработка пакета (установим задержку или отбросим пакет) */
    dlc_mod_handle_packet(&q->dlc, skb);

    /* Помещаем пакет в очередь */
    return qdisc_enqueue_tail(skb, sch);
}

static struct sk_buff *dlc_dequeue(struct Qdisc *sch)
{
    struct sk_buff *skb;

    skb = qdisc_dequeue_head(sch);
    return skb;
}

static struct Qdisc_ops dlc_qdisc_ops __read_mostly = {
    .id            = "dlc_qdisc",
    //.cl_ops        = &dlc_class_ops,
    .priv_size     = sizeof(struct dlc_sched_data),
    .enqueue       = dlc_enqueue,
    .dequeue       = dlc_dequeue,
    .peek          = qdisc_peek_dequeued,
    .init          = dlc_init,
    .destroy       = dlc_destroy,
    .change        = dlc_change,
    .dump          = dlc_dump,
    .owner         = THIS_MODULE,
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
MODULE_DESCRIPTION("DLC Model qdisc kernel module");
