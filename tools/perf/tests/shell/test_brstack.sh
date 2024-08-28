#!/bin/sh
# Check branch stack sampling

# SPDX-License-Identifier: GPL-2.0
# German Gomez <german.gomez@arm.com>, 2022

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

# skip the test if the hardware doesn't support branch stack sampling
# and if the architecture doesn't support filter types: any,save_type,u
if ! perf record -o- --no-buildid --branch-filter any,save_type,u -- true > /dev/null 2>&1 ; then
	echo "skip: system doesn't support filter types: any,save_type,u"
	exit 2
fi

skip_test_missing_symbol brstack_bench

TMPDIR=$(mktemp -d /tmp/__perf_test.program.XXXXX)

cleanup() {
	rm -rf $TMPDIR
}

trap cleanup EXIT TERM INT

is_arm64() {
	uname -m | grep -q aarch64
}

if is_arm64; then
	TESTPROG="perf test -w brstack 5000"
else
	TESTPROG="perf test -w brstack"
fi

test_user_branches() {
	echo "Testing user branch stack sampling"

	perf record -o $TMPDIR/perf.data --branch-filter any,save_type,u -- ${TESTPROG} > /dev/null 2>&1
	perf script -i $TMPDIR/perf.data --fields brstacksym | tr ' ' '\n' > $TMPDIR/perf.script

	# example of branch entries:
	# 	brstack_foo+0x14/brstack_bar+0x40/P/-/-/0/CALL

	set -x
	grep -E -m1 "^brstack_bench\+[^ ]*/brstack_foo\+[^ ]*/IND_CALL/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack_foo\+[^ ]*/brstack_bar\+[^ ]*/CALL/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack_bench\+[^ ]*/brstack_foo\+[^ ]*/CALL/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack_bench\+[^ ]*/brstack_bar\+[^ ]*/CALL/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack_bar\+[^ ]*/brstack_foo\+[^ ]*/RET/.*$"		$TMPDIR/perf.script
	grep -E -m1 "^brstack_foo\+[^ ]*/brstack_bench\+[^ ]*/RET/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack_bench\+[^ ]*/brstack_bench\+[^ ]*/COND/.*$"	$TMPDIR/perf.script
	grep -E -m1 "^brstack\+[^ ]*/brstack\+[^ ]*/UNCOND/.*$"		$TMPDIR/perf.script

	if is_arm64; then
		# in arm64 with BRBE, we get IRQ entries that correspond
		# to any point in the process
		grep -m1 "/IRQ/"					$TMPDIR/perf.script
	fi
	set +x

	# some branch types are still not being tested:
	# IND COND_CALL COND_RET SYSCALL SYSRET IRQ SERROR NO_TX
}

test_arm64_trap_eret_branches() {
	echo "Testing trap & eret branches (arm64 brbe)"
	perf record -o $TMPDIR/perf.data --branch-filter any,save_type,u -- \
		perf test -w traploop 250
	perf script -i $TMPDIR/perf.data --fields brstacksym | tr ' ' '\n' > $TMPDIR/perf.script
	set -x
	# BRBINF<n>.TYPE == TRAP are mapped to PERF_BR_SYSCALL by the BRBE driver
	grep -E -m1 "^trap_bench\+[^ ]*/\[unknown\][^ ]*/SYSCALL/" $TMPDIR/perf.script
	grep -E -m1 "^\[unknown\][^ ]*/trap_bench\+[^ ]*/ERET/"	$TMPDIR/perf.script
	set +x
}

test_arm64_kernel_branches() {
	echo "Testing kernel branches (arm64 brbe)"
	# skip if perf doesn't have enough privileges
	if ! perf record --branch-filter any,k -o- -- true > /dev/null; then
		echo "[skipped: not enough privileges]"
		return 0
	fi
	perf record -o $TMPDIR/perf.data --branch-filter any,k -- uname -a
	perf script -i $TMPDIR/perf.data --fields brstack | tr ' ' '\n' > $TMPDIR/perf.script
	grep -E -m1 "0xffff[0-9a-f]{12}" $TMPDIR/perf.script
	! egrep -E -m1 "0x0000[0-9a-f]{12}" $TMPDIR/perf.script
}

# first argument <arg0> is the argument passed to "--branch-stack <arg0>,save_type,u"
# second argument are the expected branch types for the given filter
test_filter() {
	test_filter_filter=$1
	test_filter_expect=$2

	echo "Testing branch stack filtering permutation ($test_filter_filter,$test_filter_expect)"

	perf record -o $TMPDIR/perf.data --branch-filter $test_filter_filter,save_type,u -- ${TESTPROG} > /dev/null 2>&1
	perf script -i $TMPDIR/perf.data --fields brstack | tr ' ' '\n' | sed '/^[[:space:]]*$/d' > $TMPDIR/perf.script

	# fail if we find any branch type that doesn't match any of the expected ones
	# also consider UNKNOWN branch types (-)
	if grep -E -vm1 "^[^ ]*/($test_filter_expect|-|( *))/.*$" $TMPDIR/perf.script; then
		return 1
	fi
}

set -e

test_user_branches

if is_arm64; then
	test_arm64_trap_eret_branches
	test_arm64_kernel_branches
fi

test_filter "any_call"	"CALL|IND_CALL|COND_CALL|SYSCALL|IRQ|FAULT_DATA|FAULT_INST"
test_filter "call"	"CALL|SYSCALL"
test_filter "cond"	"COND"
test_filter "any_ret"	"RET|COND_RET|SYSRET|ERET"

test_filter "call,cond"		"CALL|SYSCALL|COND"
test_filter "any_call,cond"		"CALL|IND_CALL|COND_CALL|IRQ|SYSCALL|COND|FAULT_DATA|FAULT_INST"
test_filter "cond,any_call,any_ret"	"COND|CALL|IND_CALL|COND_CALL|SYSCALL|IRQ|RET|COND_RET|SYSRET|ERET|FAULT_DATA|FAULT_INST"
