#!/usr/bin/python
#
# Parse ss.log textual output written by ss_log_thread() in nsperf.py.
# Usage:
#    infile=foo/ss.log outdir=out/ ss_log_parser.py
#
# Author:
#  Neal Cardwell
# Based on code by:
#  Kevin (Yudong) Yang
#  Soheil Hassas Yeganeh

import os
import socket
import sys
import time

DEBUG = False   # enable debugging output?

def debug(s):
    if DEBUG:
        print('DEBUG: %s' % s)

def median(nums):
    """Return median of all numbers."""

    if len(nums) == 0:
        return 0
    sorted_nums = sorted(nums)
    n = len(sorted_nums)
    m = n - 1
    return (sorted_nums[n/2] + sorted_nums[m/2]) / 2.0

def read_file():
    """Read the ss.log file and parse into a dictionary."""
    all_data = {}   # data for all time:            <time>: time_data
    time_data = {}  # data for the current timestamp: <port>: { field: value }
    time_secs = -1
    ss_log_path = os.environ['infile']
    debug('reading path: %s' % (ss_log_path))
    f = open(ss_log_path)

    # Read a timestamp line, or per-flow tuple line, or EOF.
    line = f.readline()
    debug('readline 1 => %s' % (line))
    while True:
        debug('line => %s' % (line))

        # If the file is done or data for current time is done, save time data.
        if not line or line.startswith('# ') and len(time_data):
            debug('all_data time %d => time_data %s' %
                  (time_secs,  time_data))
            all_data[time_secs] = time_data
            time_data = {}

        if not line:
            return all_data

        # Check to see if we have data for a new point in time
        if line.startswith('# '):
            time_secs = float(line[2:])
            assert time_secs > 0, time_secs
            debug('time_secs = %s' % (time_secs))
            # Read ss column headers ("State...")
            line = f.readline()
            debug('readline column headers => %s' % (line))
            # Read next line
            line = f.readline()
            continue

        # Parse line with 4-tuple
        debug('readline for 4-tuple => %s' % (line))
        if not line or line.startswith('# '):
            continue   # No live sockets with ports maching the ss query...
        if len(line.split()) != 5:
            sys.stderr.write('unable to find 4-tuple in: %s' % (line))
            #print('unable to find 4-tuple in: %s' % (line))
            sys.exit()
        flow_data = {}
        port = line.strip()
        port = int(port[port.rfind(':') + 1:])
        flow_data['port'] = port

        # Read line with flow stats
        line = f.readline()
        debug('readline flow stats => %s' % (line))
        assert line, 'expected flow stats for port %d' % (port)
        stats = line.strip().split()
        debug('stats: %s' % (stats))
        for item in stats:
            if item.startswith('cwnd:'):
                flow_data['cwnd'] = int(item[item.rfind(':') + 1:])
            elif item.startswith('bytes_acked:'):
                flow_data['bytes_acked'] = int(item[item.rfind(':') + 1:])
            elif item.startswith('retrans:'):
                flow_data['retrans'] = int(item[item.rfind('/') + 1:])
            elif item.startswith('data_segs_out:'):
                flow_data['data_segs_out'] = int(item[item.rfind(':') + 1:])
            elif item.startswith('rtt:'):
                flow_data['rtt'] = (
                    float(item[item.find(':') + 1:item.rfind('/')]) / 1000
                )
            elif item.startswith('unacked:'):
                flow_data['unacked'] = int(item[item.find(':') + 1:])
        debug('time_data for time %s port %d: %s' %
              (time_secs, port, flow_data))
        if not 'cwnd' in flow_data:
            sys.stderr.write('unable to find cwnd in: %s' % (line))
            #print('unable to find cwnd in: %s' % (line))
            sys.exit()
        time_data[port] = flow_data
        # Move on to the next line:
        line = f.readline()

def log_retrans_rate(all_data):
    """Log average retransmit rate for each flow and globally."""
    outdir = os.environ['outdir']

    last_data_segs_out = {}  # last data_segs_out per port
    last_retrans =       {}  # last retransmitted packet count per port
    retrans_rates = {}      # maps port number to retrans rate
    for t in sorted(all_data.keys()):
        time_data = all_data[t]
        for port, flow_data in time_data.items():
            debug('port %d flow_data %s' % (port, flow_data))
            last_data_segs_out[port] = flow_data.get('data_segs_out', 0)
            debug('port %d last_data_segs_out=%s' %
                  (port, last_data_segs_out[port]))
            last_retrans[port] = flow_data.get('retrans', 0)
            debug('port %d last_retrans=' % last_retrans[port])

    total_retrans = 0
    total_data_segs_out = 0
    for port in sorted(last_data_segs_out):
        if last_data_segs_out[port] == 0:
            sys.stderr.write('outdir=%s port %d: last_data_segs_out==0\n' %
                             (outdir, port))
            retrans = 0
        else:
            retrans = float(last_retrans[port]) / float(last_data_segs_out[port])
        retrans_rates[port] = retrans
        total_retrans += last_retrans[port]
        total_data_segs_out += last_data_segs_out[port]
    if total_data_segs_out == 0:
        sys.stderr.write('outdir=%s total_data_segs_out==0\n' % (outdir))
        total_retrans_rate = 0
    else:
        total_retrans_rate = float(total_retrans) / float(total_data_segs_out)

    # Write average retx rate for each flow, in percent.
    i = 0
    for port, retrans_rate in retrans_rates.items():
        filename = 'retrans.out.%d.txt' % (i)
        f = open(os.path.join(outdir, filename), 'w')
        f.write('%.5f\n' % (retrans_rate * 100.0))
        f.close()
        i += 1

    # Write average retx rate across all flows, in percent.
    filename = 'retrans.out.total.txt'
    f = open(os.path.join(outdir, filename), 'w')
    f.write('%.5f\n' % (total_retrans_rate * 100.0))
    f.close()

def log_rtt(all_data):
    """Log median srtt for all srtt samples we took from periodic ss dumps."""
    rtts = []
    for t in sorted(all_data.keys()):
        time_data = all_data[t]
        for port, flow_data in time_data.items():
            debug('port %d flow_data %s' % (port, flow_data))
            if 'rtt' in flow_data:
                rtt = flow_data['rtt']
                rtts.append(rtt)

    p50_rtt = median(rtts)
    p50_rtt = p50_rtt * 1000.0   # convert to ms
    # Write p50 srtt sample (in secs) we took across all flows.
    outdir = os.environ['outdir']
    filename = 'rtt_p50.out.total.txt'
    f = open(os.path.join(outdir, filename), 'w')
    f.write('%s\n' % p50_rtt)   # RTT in ms
    f.close()

def main():
    """Main function to run everything."""
    all_data = read_file()
    log_retrans_rate(all_data)
    log_rtt(all_data)
    return 0

if __name__ == '__main__':
    sys.exit(main())
