// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include "cpufreq_ext.skel.h"

static int exit_req;

static void err_exit(int err)
{
	exit_req = 1;
}

int main(int argc, char **argv)
{
	struct bpf_link *link;
	struct cpufreq_ext *skel;
	int idx = 0;
	int exit_stat;

	signal(SIGINT, err_exit);
	signal(SIGKILL, err_exit);
	signal(SIGTERM, err_exit);

	skel = cpufreq_ext__open_and_load();
	bpf_map__set_autoattach(skel->maps.cpufreq_ext_demo_ops, false);
	link = bpf_map__attach_struct_ops(skel->maps.cpufreq_ext_demo_ops);

	while (!exit_req) {
		exit_stat = 0;
		bpf_map_lookup_elem(bpf_map__fd(skel->maps.exit_stat), &idx, &exit_stat);
		if (exit_stat)
			break;
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	cpufreq_ext__destroy(skel);
	return 0;
}
