/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM net
#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_NET_VH_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_NET_VH_H
#include <trace/hooks/vendor_hooks.h>

struct packet_type;
struct list_head;
DECLARE_HOOK(android_vh_ptype_head,
	TP_PROTO(const struct packet_type *pt, struct list_head *vendor_pt),
	TP_ARGS(pt, vendor_pt));

struct nf_conn;
struct sock;
struct msghdr;
struct sk_buff;
DECLARE_RESTRICTED_HOOK(android_rvh_nf_conn_alloc,
	TP_PROTO(struct nf_conn *nf_conn), TP_ARGS(nf_conn), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_nf_conn_free,
	TP_PROTO(struct nf_conn *nf_conn), TP_ARGS(nf_conn), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_sk_alloc,
	TP_PROTO(struct sock *sock), TP_ARGS(sock), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_sk_free,
	TP_PROTO(struct sock *sock), TP_ARGS(sock), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_tcp_sendmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len),
	TP_ARGS(sk, msg, len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_tcp_recvmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len),
	TP_ARGS(sk, msg, len, flags, addr_len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_udp_sendmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len),
	TP_ARGS(sk, msg, len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_udp_recvmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len),
	TP_ARGS(sk, msg, len, flags, addr_len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_udpv6_sendmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len),
	TP_ARGS(sk, msg, len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_udpv6_recvmsg,
	TP_PROTO(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len),
	TP_ARGS(sk, msg, len, flags, addr_len), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_tcp_select_window,
	TP_PROTO(struct sock *sk, u32 *new_win), TP_ARGS(sk, new_win), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_inet_sock_create,
	TP_PROTO(struct sock *sk), TP_ARGS(sk), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_inet_sock_release,
	TP_PROTO(struct sock *sk), TP_ARGS(sk), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_bpf_skb_load_bytes,
	TP_PROTO(const struct sk_buff *skb, u32 offset, void *to, u32 len,
		int *handled, int *err),
	TP_ARGS(skb, offset, to, len, handled, err), 1);
DECLARE_HOOK(android_vh_tcp_rtt_estimator,
	TP_PROTO(struct sock *sk, long mrtt_us), TP_ARGS(sk, mrtt_us));
DECLARE_HOOK(android_vh_udp_enqueue_schedule_skb,
	TP_PROTO(struct sock *sk, struct sk_buff *skb), TP_ARGS(sk, skb));
DECLARE_HOOK(android_vh_build_skb_around,
	TP_PROTO(struct sk_buff *skb), TP_ARGS(skb));
DECLARE_HOOK(android_vh_tcp_write_timeout_estab_retrans,
        TP_PROTO(struct sock *sk), TP_ARGS(sk));
/* macro versions of hooks are no longer required */

#endif /* _TRACE_HOOK_NET_VH_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
