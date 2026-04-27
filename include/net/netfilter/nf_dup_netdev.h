/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NF_DUP_NETDEV_H_
#define _NF_DUP_NETDEV_H_

#include <net/netfilter/nf_tables.h>
#include <linux/netdevice.h>
#include <linux/sched.h>

void nf_dup_netdev_egress(const struct nft_pktinfo *pkt, int oif);
void nf_fwd_netdev_egress(const struct nft_pktinfo *pkt, int oif);

#define NF_RECURSION_LIMIT	2

static inline u8 *nf_get_nf_dup_skb_recursion(void)
{
#ifndef CONFIG_PREEMPT_RT
	return this_cpu_ptr(&softnet_data.xmit.nf_dup_skb_recursion);
#else
	return &current->net_xmit.nf_dup_skb_recursion;
#endif
}

struct nft_offload_ctx;
struct nft_flow_rule;

int nft_fwd_dup_netdev_offload(struct nft_offload_ctx *ctx,
			       struct nft_flow_rule *flow,
			       enum flow_action_id id, int oif);
#endif
