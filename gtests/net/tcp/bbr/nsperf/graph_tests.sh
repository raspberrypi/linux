#!/bin/bash
# For all test results, generate png graphs and an HTML page linking to them.
#
# By default, graphs all tests:
#   ./graph_tests.sh
# But you can also graph a subset of tests by setting the "tests"
# environment variable:
#   tests="coexist shallow" ./graph_tests.sh
#
# fancier usage:
#          indir=out.180.sec/ outdir=graphs/ ./graph_tests.sh

if [ "$indir" = "" ]; then
    indir="out/"
fi

if [ "$outdir" = "" ]; then
    outdir="graphs/"
fi

# By default graph all tests.
# To graph a subset of tests, set the environment variable: tests="foo bar".
if [ "$tests" = "" ]; then
    tests="coexist random_loss shallow bufferbloat ecn_bulk"
fi

format=png
if [ "$format" = "png" ]; then
  PNG_SIZE="1024,768"
  TERMINAL="set terminal pngcairo noenhanced size $PNG_SIZE"
else
  TERMINAL="set terminal wxt noenhanced size 1024,768"
fi

mkdir -p $outdir

# Start HTML for a web page showing all png graphs we generate.
TITLE="bbr v2 alpha upstream core tests"
HTML_PATH="${outdir}/index.html"
echo > $HTML_PATH
echo "<html><title>$TITLE</title><body> <b> $TITLE </b> <br>\n" >> $HTML_PATH

if [[ $tests == *"coexist"* ]]; then
    #######
    # show acceptable coexistence w/ cubic:
    # graph tput of 1 cubic, 1 BBR at a range of buffer depths:
    # (bw=50M, rtt=30ms, buf={...}xBDP)
    rm -f $outdir/coexist.*
    for cc_combo in cubic:1,bbr:1 cubic:1,bbr2:1; do
	for bdp_of_buf in  0.1  1 2 4 8 16; do
	    echo -n "$bdp_of_buf " >> $outdir/coexist.${cc_combo}
	    grep THROUGHPUT $indir/coexist/${cc_combo}/${bdp_of_buf}/netperf.out.1.txt | \
		cut -d= -f2 >> $outdir/coexist.${cc_combo}
	done
    done

    OUTPNG="$outdir/coexist_1xcubic_1xbbr2_50M_30ms_varybuf.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top left\n\
	set ytics nomirror\n\
	set grid\n\
	set title  'cubic vs BBR throughput'\n\
        set xlabel 'buffer size (as a multiple of BDP)'\n\
        set ylabel 'throughput in Mbit/sec'\n\
	set yrange [0:50]\n\
	plot '$outdir/coexist.cubic:1,bbr:1'  u 1:2 t 'bbr'  w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/coexist.cubic:1,bbr2:1' u 1:2 t 'bbr2' w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/coexist.gnuplot

    gnuplot -persist $outdir/coexist.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH
fi


if [[ $tests == *"random_loss"* ]]; then
    #######
    # show high throughput with random loss up to design parameter:
    # graph tput of cubic, bbr2 at a range of random loss rates
    # (bw=1G, rtt=100ms, loss={...}
    rm -f $outdir/random_loss.*
    loss_rates="0.00001 0.0001 0.001 0.01 0.1 0.2 0.5 1 2 3 10 15 20"
    for loss_rate in $loss_rates; do
	for cc_name in cubic bbr bbr2; do
	    cc="${cc_name}:1"
	    sumd="$indir/random_loss/${cc}/${loss_rate}/summary/"
	    mkdir -p $sumd
	    rm -f "${sumd}/*txt"
	    for rep in `seq 1 10`; do
		d="$indir/random_loss/${cc}/${loss_rate}/rep-${rep}"
		grep THROUGHPUT ${d}/netperf.out.0.txt | cut -d= -f2 >> ${sumd}/THROUGHPUT.samples.txt
	    done
	    infile="${sumd}/THROUGHPUT.samples.txt" ./median.py > \
		  ${sumd}/THROUGHPUT.median.txt
	    echo -n "$loss_rate " >> $outdir/random_loss.${cc}
	    cat ${sumd}/THROUGHPUT.median.txt >> $outdir/random_loss.${cc}
	done
    done

    OUTPNG="$outdir/random_loss_1G_100ms_varyloss.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top right\n\
	set ytics nomirror\n\
	set grid\n\
        set logscale x\n\
	set title  'cubic, bbr, and bbr2 throughput with random loss'\n\
        set xlabel 'random loss rate, in percent'\n\
        set ylabel 'throughput in Mbit/sec'\n\
	set yrange [0:1000]\n\
        set xrange [:20]\n\
	plot '$outdir/random_loss.cubic:1' u 1:2 t 'cubic' w lp lw 2 pt 7 lt rgb \"#d7191c\",\
	     '$outdir/random_loss.bbr:1'   u 1:2 t 'bbr' w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/random_loss.bbr2:1'  u 1:2 t 'bbr2'  w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/random_loss.gnuplot

    gnuplot -persist $outdir/random_loss.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH
fi


if [[ $tests == *"shallow"* ]]; then
    #######
    # show reasonably low loss rates in shallow buffers:
    # graph retransmit rate for range of flow counts
    # (bw=1G, rtt=100ms, buf=1ms, num_flows={...})
    # BDP is 1G*100ms = 8256 packets
    rm -f $outdir/shallow_buf.*
    for num_flows in 1 10 30 60 100; do
	for cc_name in cubic bbr bbr2; do
	    echo -n "$num_flows " >> $outdir/shallow_buf.${cc_name}
	    d="$indir/shallow/${cc_name}:${num_flows}/${num_flows}"
	    infile=${d}/ss.log outdir=${d}/ ./ss_log_parser.py
	    cat ${d}/retrans.out.total.txt >> $outdir/shallow_buf.${cc_name}
	done
    done

    OUTPNG="$outdir/shallow_buf_1G_100ms_varynumflows.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top left\n\
	set ytics nomirror\n\
	set grid\n\
        set logscale x\n\
	set title  'cubic, bbr, and bbr2 retransmit rate in shallow buffers'\n\
        set xlabel 'number of flows'\n\
        set ylabel 'retransmit rate (percent)'\n\
	set yrange [0:15]\n\
        set xrange [:]\n\
	plot '$outdir/shallow_buf.cubic' u 1:2 t 'cubic' w lp lw 2 pt 7 lt rgb \"#d7191c\",\
	     '$outdir/shallow_buf.bbr'   u 1:2 t 'bbr' w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/shallow_buf.bbr2'  u 1:2 t 'bbr2'  w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/shallow_buf.gnuplot

    gnuplot -persist $outdir/shallow_buf.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH
fi


if [[ $tests == *"bufferbloat"* ]]; then
    #######
    # show low delay in deep buffers, even without ECN signal:
    # graph p50 RTT for two flows using either cubic or bbr2,
    # at a range of buffer depths.
    # (bw=50M, rtt=30ms, buf={...}xBDP)
    rm -f $outdir/bufferbloat.*
    for bdp_of_buf in 1 10 50 100; do
	for cc_name in cubic bbr bbr2; do
	    echo -n "$bdp_of_buf " >> $outdir/bufferbloat.${cc_name}
	    num_flows=2
	    d="$indir/bufferbloat/${cc_name}:${num_flows}/${bdp_of_buf}"
	    infile=${d}/ss.log outdir=${d}/ ./ss_log_parser.py
	    cat ${d}/rtt_p50.out.total.txt >> $outdir/bufferbloat.${cc_name}
	done
    done

    OUTPNG="$outdir/bufferbloat_50M_30ms_varybuf.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top left\n\
	set ytics nomirror\n\
	set grid\n\
	set title  'cubic, bbr, and bbr2 median RTT'\n\
        set xlabel 'buffer size (as a multiple of BDP)'\n\
        set ylabel 'median srtt sample (ms)'\n\
	set yrange [0:]\n\
        set xrange [1:100]\n\
	plot '$outdir/bufferbloat.cubic' u 1:2 t 'cubic' w lp lw 2 pt 7 lt rgb \"#d7191c\",\
	     '$outdir/bufferbloat.bbr'   u 1:2 t 'bbr' w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/bufferbloat.bbr2'  u 1:2 t 'bbr2'  w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/bufferbloat.gnuplot

    gnuplot -persist $outdir/bufferbloat.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH
fi

if [[ $tests == *"ecn_bulk"* ]]; then
    rm -f $outdir/ecn_bulk.*

    #######
    # show ECN support can keep queues very low:
    # graph p50 for range of flow counts.
    # (bw=1G, rtt=1ms, num_flows={...})
    # For each CC and flow count, show the median of the p50 RTT from N trials.
    for cc_name in dctcp bbr2 bbr; do
	for num_flows in 1 4 10 40 100; do
	    sumd="$indir/ecn_bulk/${cc_name}/${num_flows}/summary/"
	    mkdir -p $sumd
	    rm -f "${sumd}/*txt"
	    for rep in `seq 1 10`; do
		# Find median srtt for this rep, and add it to list
		# of all samples.
		d="$indir/ecn_bulk/${cc_name}/${num_flows}/rep-${rep}"
		infile=${d}/ss.log outdir=${d}/ ./ss_log_parser.py
		cat ${d}/rtt_p50.out.total.txt >> ${sumd}/rtt_p50.out.samples.txt
	    done
	    infile="${sumd}/rtt_p50.out.samples.txt" ./median.py > \
		  ${sumd}/rtt_p50.out.median.txt
	    echo -n "$num_flows " >> $outdir/ecn_bulk.${cc_name}
	    cat ${sumd}/rtt_p50.out.median.txt >> $outdir/ecn_bulk.${cc_name}
	done
    done

    OUTPNG="$outdir/ecn_bulk_1G_1ms_rtt_varynumflows.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top left\n\
	set ytics nomirror\n\
	set grid\n\
        set logscale x\n\
	set title  'dctcp, bbr, and bbr2 median RTT'\n\
        set xlabel 'number of flows'\n\
        set ylabel 'median srtt sample (ms)'\n\
	set yrange [0:]\n\
        set xrange [1:100]\n\
	plot '$outdir/ecn_bulk.dctcp'       u 1:2 t 'dctcp'           w lp lw 2 pt 7 lt rgb \"#d7191c\",\
	     '$outdir/ecn_bulk.bbr'         u 1:2 t 'bbr'             w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/ecn_bulk.bbr2'  u 1:2 t 'bbr2' w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/ecn_bulk_rtt.gnuplot

    gnuplot -persist $outdir/ecn_bulk_rtt.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH


    #######
    # show ECN support can keep queues very low:
    # graph median of retrans rates across N trials:
    for cc_name in dctcp bbr2 bbr; do
	for num_flows in 1 4 10 40 100; do
	    sumd="$indir/ecn_bulk/${cc_name}/${num_flows}/summary/"
	    mkdir -p $sumd
	    rm -f "${sumd}/*txt"
	    for rep in `seq 1 10`; do
		# Find overall retrans rate for this rep, and add it to list
		# of all samples.
		d="$indir/ecn_bulk/${cc_name}/${num_flows}/rep-${rep}"
		cat ${d}/retrans.out.total.txt >> ${sumd}/retrans.out.samples.txt
	    done
	    infile="${sumd}/retrans.out.samples.txt" ./median.py > \
		  ${sumd}/retrans.out.median.txt
	    echo -n "$num_flows " >> $outdir/ecn_bulk.retrans.${cc_name}
	    cat ${sumd}/retrans.out.median.txt >> $outdir/ecn_bulk.retrans.${cc_name}
	done
    done

    OUTPNG="$outdir/ecn_bulk_1G_1ms_retrans_varynumflows.png"
    OUTPUT="\n\
set output '$OUTPNG'"

    echo -e "set y2tics\n\
	$TERMINAL $OUTPUT\n\
	set key top left\n\
	set grid\n\
        set logscale x\n\
        set logscale y\n\
	set title  'dctcp, bbr, and bbr2 retransmit rate'\n\
        set xlabel 'number of flows'\n\
        set ylabel 'retransmit rate (percent)'\n\
	set yrange [:]\n\
        set xrange [1:100]\n\
	plot '$outdir/ecn_bulk.retrans.dctcp'       u 1:2 t 'dctcp' axis x1y1 w lp lw 2 pt 7 lt rgb \"#d7191c\",\
	     '$outdir/ecn_bulk.retrans.bbr'         u 1:2 t 'bbr'   axis x1y1 w lp lw 2 pt 7 lt rgb \"#abd9e9\",\
	     '$outdir/ecn_bulk.retrans.bbr2'  u 1:2 t 'bbr2'  axis x1y1 w lp lw 2 pt 7 lt rgb \"#2c7bb6\"\
             \n" > $outdir/ecn_bulk_retrans.gnuplot

    gnuplot -persist $outdir/ecn_bulk_retrans.gnuplot
    echo -e "<img src='$OUTPNG'>\n" >> $HTML_PATH

fi

echo "done graphing all tests: $tests"
