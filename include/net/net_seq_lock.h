#ifndef __NET_NET_SEQ_LOCK_H__
#define __NET_NET_SEQ_LOCK_H__

#ifdef CONFIG_PREEMPT_RT
# define net_seqlock_t			seqlock_t
# define net_seq_begin(__r)		read_seqbegin(__r)
# define net_seq_retry(__r, __s)	read_seqretry(__r, __s)

#else
# define net_seqlock_t			seqcount_t
# define net_seq_begin(__r)		read_seqcount_begin(__r)
# define net_seq_retry(__r, __s)	read_seqcount_retry(__r, __s)
#endif

#endif
