// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Google LLC */
#include <linux/bpf.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <test_progs.h>
#include <cgroup_helpers.h>
#include "cgroup_skb_direct_packet_access.skel.h"

#define OLD_QUERY_SIZE		offsetofend(union bpf_attr, query.prog_cnt)
#define FULL_QUERY_SIZE		offsetofend(union bpf_attr, query.revision)

static void test_query_size_boundaries(void)
{
	struct cgroup_skb_direct_packet_access *skel;
	struct bpf_link *link = NULL;
	union bpf_attr attr;
	int cg_fd = -1;
	int err;

	skel = cgroup_skb_direct_packet_access__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel_load"))
		return;

	cg_fd = test__join_cgroup("/attr_size_cg");
	if (!ASSERT_GE(cg_fd, 0, "join_cgroup"))
		goto cleanup;

	link = bpf_program__attach_cgroup(skel->progs.direct_packet_access,
					  cg_fd);
	if (!ASSERT_OK_PTR(link, "cg_attach"))
		goto cleanup;

	memset(&attr, 0, sizeof(attr));
	attr.query.target_fd = cg_fd;
	attr.query.attach_type = BPF_CGROUP_INET_INGRESS;
	attr.query.revision = 0xdeadbeefdeadbeefULL;

	err = syscall(__NR_bpf, BPF_PROG_QUERY, &attr, OLD_QUERY_SIZE);
	if (ASSERT_OK(err, "query_old_size")) {
		ASSERT_EQ(attr.query.prog_cnt, 1, "prog_cnt_written_old");
		ASSERT_EQ(attr.query.revision, 0xdeadbeefdeadbeefULL,
			  "revision_not_written_old");
	}

	memset(&attr, 0, sizeof(attr));
	attr.query.target_fd = cg_fd;
	attr.query.attach_type = BPF_CGROUP_INET_INGRESS;

	err = syscall(__NR_bpf, BPF_PROG_QUERY, &attr, FULL_QUERY_SIZE);
	if (!ASSERT_OK(err, "query_full_size"))
		goto cleanup;

	ASSERT_EQ(attr.query.prog_cnt, 1, "prog_cnt_written");
	ASSERT_GT(attr.query.revision, 0, "revision_written");

cleanup:
	if (link)
		bpf_link__destroy(link);
	if (cg_fd >= 0)
		close(cg_fd);
	cgroup_skb_direct_packet_access__destroy(skel);
}

void test_bpf_attr_size(void)
{
	if (test__start_subtest("query_size_boundaries"))
		test_query_size_boundaries();
}
