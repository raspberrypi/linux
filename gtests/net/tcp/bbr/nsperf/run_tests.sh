#!/bin/bash
#
# Run a set of tests with bbr2, bbr, cubic, dctcp.
# By default, runs all tests:
#   ./run_tests.sh
# But you can also run a subset of tests by setting the "tests"
# environment variable:
#   tests="coexist shallow" ./run_tests.sh
#

# By default run all tests.
# To run a subset of tests, set the environment variable: tests="foo bar".
if [ "$tests" = "" ]; then
    tests="coexist random_loss shallow bufferbloat ecn_bulk"
fi

# Module parameters for the alpha research release of bbr2 are here:
MOD_PARAM_DIR=/sys/module/tcp_bbr2/parameters/

# Disable ECN support:
function disable_bbr_ecn() {
    echo 0 > $MOD_PARAM_DIR/ecn_enable
    egrep . $MOD_PARAM_DIR/* | grep ecn_enable
    echo 5000 > $MOD_PARAM_DIR/ecn_max_rtt_us
    egrep . $MOD_PARAM_DIR/* | grep ecn_max_rtt_us
}

# Enable ECN support, with the understanding that all ECN signals we get
# here will be DCTCP/L4S ECN signals:
function enable_bbr_ecn() {
    echo 1 > $MOD_PARAM_DIR/ecn_enable
    egrep . $MOD_PARAM_DIR/* | grep ecn_enable
    echo 0 > $MOD_PARAM_DIR/ecn_max_rtt_us
    egrep . $MOD_PARAM_DIR/* | grep ecn_max_rtt_us
}

# Make sure send and receive buffers can grow quite large, e.g. for
# bw=1G, rtt=100ms or larger.
sysctl -w net.core.rmem_max=250000000 net.ipv4.tcp_rmem='4096 131072 250000000'
sysctl -w net.core.wmem_max=250000000 net.ipv4.tcp_wmem='4096  16384 250000000'
disable_bbr_ecn

function get_buf_pkts() {
    buf_pkts=`echo | awk -v bw=$bw -v rtt=$rtt -v bdp_of_buf=$bdp_of_buf '{bdp_pkts = int(bw*1000*1000*rtt/1000.0 / (1514 * 8) * bdp_of_buf); print bdp_pkts;}'`
}

if [[ $tests == *"coexist"* ]]; then
    # show acceptable coexistence w/ cubic:
    # graph tput of 1 cubic, 1 bbr2 at a range of buffer depths:
    # (bw=50M, rtt=30ms, buf={...}xBDP)
    # [run for a very long time, 10minutes, to find convergence...]
    for cc_combo in cubic:1,bbr:1 cubic:1,bbr2:1; do
	for bdp_of_buf in  0.1  1 2 4 8 16; do
	    cmd=""
	    cc=$cc_combo     # mix of CCs in this experiment
	    interval=2       # interval between flow starts, in secs
	    bw=50            # Mbit/sec
	    rtt=30           # ms
	    qdisc=''         # use netem FIFO
	    loss=0           # loss in percent
	    dur=180          # test duration in secs
	    outdir="out/coexist/${cc}/$bdp_of_buf/"
	    # Create output directory:
	    mkdir -p $outdir
	    get_buf_pkts
	    set +e
	    cc=$cc bw=$bw rtt=$rtt buf=$buf_pkts qdisc=$qdisc loss=$loss \
	      dur=$dur cmd=$cmd outdir=$outdir interval=$interval \
	      ./nsperf.py stream | tee ${outdir}/nsperf.out.txt
	    set -e
	done
    done
fi

if [[ $tests == *"random_loss"* ]]; then
    # show high throughput with random loss up to design parameter:
    # graph tput of cubic, bbr2 at a range of random loss rates
    # (bw=1G, rtt=100ms, loss={....}
    for rep in `seq 1 10`; do
	for cc_name in cubic bbr2 bbr; do
	    loss_rates="0.00001 0.0001 0.001 0.01 0.1 0.2 0.5 1 2 3 10 15 20"
	    for loss_rate in $loss_rates; do
		cmd=""
		cc=${cc_name}:1  # 1 flow
		interval=0       # interval between flow starts, in secs
		bw=1000          # Mbit/sec
		rtt=100          # ms
		bdp_of_buf=1     # buffer = 100% of BDP, or 100ms
		qdisc=''         # use netem FIFO
		loss=$loss_rate  # loss in percent
		dur=60           # test duration in secs
		outdir="out/random_loss/${cc}/${loss}/rep-${rep}/"
		# Create output directory:
		mkdir -p $outdir
		get_buf_pkts
		set +e
		cc=$cc bw=$bw rtt=$rtt buf=$buf_pkts qdisc=$qdisc loss=$loss \
		  dur=$dur cmd=$cmd outdir=$outdir interval=$interval \
		  ./nsperf.py stream | tee ${outdir}/nsperf.out.txt
		set -e
	    done
	done
    done
fi

if [[ $tests == *"shallow"* ]]; then
    # show reasonably low loss rates in shallow buffers:
    # graph retransmit rate for range of flow counts
    # (bw=1G, rtt=100ms, buf=1ms, num_flows={...})
    # BDP is 1G*100ms = 8256 packets
    for cc_name in cubic bbr2 bbr; do
	for num_flows in 1 10 30 60 100; do
	    cmd=""
	    cc=${cc_name}:${num_flows}  # all flows bbr2
	    interval=.139    # interval between flow starts, in secs
	    bw=1000          # Mbit/sec
	    rtt=100          # ms
	    bdp_of_buf=0.02  # buffer = 2% of BDP, or 2ms
	    qdisc=''         # use netem FIFO
	    loss=0           # loss in percent
	    dur=300          # test duration in secs
	    outdir="out/shallow/${cc}/${num_flows}/"
	    # Create output directory:
	    mkdir -p $outdir
	    get_buf_pkts
	    set +e
	    cc=$cc bw=$bw rtt=$rtt buf=$buf_pkts qdisc=$qdisc loss=$loss \
	      dur=$dur cmd=$cmd outdir=$outdir interval=$interval \
	      ./nsperf.py stream | tee ${outdir}/nsperf.out.txt
	    set -e
	done
    done
fi

if [[ $tests == *"bufferbloat"* ]]; then
    # show low delay in deep buffers, even without ECN signal:
    # graph p50 RTT for two flows using either cubic or bbr2,
    # at a range of buffer depths.
    # (bw=50M, rtt=30ms, buf={...}xBDP)
    for cc_name in cubic bbr2 bbr; do
	for bdp_of_buf in 1 10 50 100; do
	    cmd=""
	    cc=${cc_name}:2  # 2 flows
	    interval=2       # interval between flow starts, in secs
	    bw=50            # Mbit/sec
	    rtt=30           # ms
	    qdisc=''         # use netem FIFO
	    loss=0           # loss in percent
	    dur=120          # test duration in secs
	    outdir="out/bufferbloat/${cc}/${bdp_of_buf}/"
	    # Create output directory:
	    mkdir -p $outdir
	    get_buf_pkts
	    set +e
	    cc=$cc bw=$bw rtt=$rtt buf=$buf_pkts qdisc=$qdisc loss=$loss \
	      dur=$dur cmd=$cmd outdir=$outdir interval=$interval \
	      ./nsperf.py stream | tee ${outdir}/nsperf.out.txt
	    set -e
	done
    done
fi


if [[ $tests == *"ecn_bulk"* ]]; then
    # show ECN support can keep queues very low:
    # graph p50 and p95 RTT (and retx, tput, fairness) for range of flow counts
    # (bw=1G, rtt=1ms, num_flows={...})
    enable_bbr_ecn
    for rep in `seq 1 10`; do
	for cc_name in dctcp bbr2 bbr; do
	    for num_flows in 1 4 10 40 100; do
		# Inside the child/test namespaces, enable ECN for
		# both active and passive connections:
		cmd='sysctl net.ipv4.tcp_ecn=1'
		cc=${cc_name}:${num_flows}  # all flows bbr2
		interval=.005    # interval between flow starts, in secs
		bw=1000          # Mbit/sec
		rtt=1            # ms
		buf_pkts=0       # not using netem buffer
		# We set the limit to 1000 packets, or 12ms at 1Gbit/sec.
		# We configure the target to be far higher, to disable
		# Codel-based drops.
		qdisc='codel ce_threshold 242us limit 1000 target 100ms'
		loss=0           # loss in percent
		dur=10           # test duration in secs
		outdir="out/ecn_bulk/${cc_name}/${num_flows}/rep-${rep}/"
		# Create output directory:
		mkdir -p $outdir
		get_buf_pkts
		set +e
		cc=$cc bw=$bw rtt=$rtt buf=$buf_pkts qdisc=$qdisc loss=$loss \
		  dur=$dur cmd=$cmd outdir=$outdir interval=$interval \
		  ./nsperf.py stream | tee ${outdir}/nsperf.out.txt
		set -e
	    done
	done
    done
    disable_bbr_ecn
fi

echo "done running all tests: $tests"
