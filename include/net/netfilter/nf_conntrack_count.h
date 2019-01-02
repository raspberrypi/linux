#ifndef _NF_CONNTRACK_COUNT_H
#define _NF_CONNTRACK_COUNT_H

unsigned int nf_conncount_lookup(struct net *net, struct hlist_head *head,
				 const struct nf_conntrack_tuple *tuple,
				 const struct nf_conntrack_zone *zone,
				 bool *addit);

bool nf_conncount_add(struct hlist_head *head,
		      const struct nf_conntrack_tuple *tuple,
		      const struct nf_conntrack_zone *zone);

void nf_conncount_cache_free(struct hlist_head *hhead);

#endif
