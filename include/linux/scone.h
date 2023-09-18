#ifndef __SCONE_H
#define __SCONE_H

#define FCRACKER
#ifdef FCRACKER
	#define FLOW_TABLE
	#define MULTI_FT
	#define SIMPLE_PATH
#endif

#ifdef FLOW_TABLE
	#define DST_PASS
#endif

#ifdef SIMPLE_PATH
    #ifndef SKIP_QOS
        #define SKIP_QOS
    #endif
#endif

struct scone_flow_table {
#ifdef MULTI_FT
        struct list_head ctable_list;
#endif
        unsigned long _skb_refdst;
#ifdef DST_PASS
        int			(*input)(struct sk_buff *);
        struct net_device *out_dev;
        int                 out_mtu;
#endif
        __be32 saddr;
        __be32 daddr;
        __u8 ip_protocol;
        struct neighbour * neigh;
        int netfilter;
        struct net_device	*dev;
        int	xmit_simple;
	int     count;
} ____cacheline_internodealigned_in_smp;

struct scone_flow_table* scone_init(struct sk_buff *skb);
//int bridge_simple_path(struct sk_buff *skb, bool dst);
#ifndef MULTI_FT
int find_ft(struct sk_buff *skb, struct scone_flow_table *ft);
#else
int find_ft(struct sk_buff *skb, struct scone_flow_table *ft, struct list_head *head);
#endif
/* scone netfilter */
int scone_simple_netfilter(struct sk_buff *skb);
#ifdef FLOW_TABLE
void probe_ft(struct sk_buff *skb);
#endif
void print_iph(struct sk_buff *skb);
#endif
