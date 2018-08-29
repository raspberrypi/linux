#!/usr/bin/python
#
# Use netem, network namespaces, and veth virtual NICs
# to run a multi-flow TCP test on a single Linux machine.
#
# There is one network namespace for each emulated host.
# The emulated hosts are as follows:
#
#   srv: server (sender)
#   srt: server router
#   mid: middle host to emulate delays and bandwidth constraints
#   crt: client router
#   cli: client (receiver)
#
# Most hosts have both a left ("l") and right ("r") virtual NIC.
# The server has only an "r" NIC and the client has only an "l" NIC.
#
# The topology is as follows:
#
#   +-------+ +-------+ +-------+ +-------+ +-------+
#   |  srv  | |  srt  | |  mid  | |  crt  | |  cli  |
#   |     r +-+ l   r +-+ l   r +-+ l   r +-+ l     |
#   +-------+ +-------+ +-------+ +-------+ +-------+
#
# Authors:
#  Neal Cardwell
#  Soheil Hassas Yeganeh
#  Kevin (Yudong) Yang
#  Arjun Roy

import os
import os.path
import socket
import sys
import threading
import time

HOSTS = ['cli', 'crt', 'mid', 'srt', 'srv']
IP_MODE = socket.AF_INET6
SS_INTERVAL_SECONDS = 0.1  # gather 'ss' stats each X seconds
FIRST_PORT = 10000         # first TCP port to use

# On Ubuntu 18.04.2 LTS, there are issues with the iproute2 binaries:
#  (1) the 'tc' binary  has a bug and cannot parse netem random loss rates
#  (2) the 'ss' tool is missing recent socket stats
# So to use this testing tool you may need to build your own iproute2 tools
# from the latest iproute2 sources:
#   sudo su -
#   apt install pkg-config bison flex
#   mkdir -p /root/iproute2/
#   cd /root/iproute2
#   git clone git://git.kernel.org/pub/scm/network/iproute2/iproute2.git
#   cd iproute2/
#   ./configure
#   make
SS_PATH = '/root/iproute2/iproute2/misc/ss'
TC_PATH = '/root/iproute2/iproute2/tc/tc'

def netperf():
    if os.path.isfile('./netperf'):
        return './netperf'
    else:
        return '/usr/bin/netperf'

def netserver():
    if os.path.isfile('./netserver'):
        return './netserver'
    else:
        return '/usr/bin/netserver'

def log_dir():
    return '/tmp/'

def run(cmd, verbose=True):
    if verbose:
        print('running: |%s|' % (cmd))
    status = os.system(cmd)
    if status != 0:
        sys.stderr.write('error %d executing: %s' % (status, cmd))

def cleanup():
    """Delete all veth pairs and all network namespaces."""
    for host in HOSTS:
        run('( ip netns exec %(host)s ip link del dev %(host)s.l; '
            '  ip netns exec %(host)s ip link del dev %(host)s.r; '
            '  ip netns del %(host)s ) 2> /dev/null'  % {'host' : host})

def setup_logging():
    """Set up all logging."""
    # Zero out /var/log/kern-debug.log so that we only get our test logs.
    run('logrotate -f /etc/logrotate.conf')
    # Set up BBR to log with printk to /var/log/kern-debug.log.
    run('echo Y > /sys/module/tcp_bbr2/parameters/debug_with_printk')
    run('echo 3 > /sys/module/tcp_bbr2/parameters/flags')

def setup_namespaces():
    """Set up all network namespaces."""
    for host in HOSTS:
        run('ip netns add %(host)s'  % {'host' : host})

def setup_loopback():
    """Set up loopback devices for all namespaces."""
    for host in HOSTS:
        run('ip netns exec %(host)s ifconfig lo up' % {'host' : host})

def setup_veth():
    """Set up all veth interfaces."""
    c = ''
    c += 'ip link add srv.r type veth peer name srt.l\n'
    c += 'ip link add srt.r type veth peer name mid.l\n'
    c += 'ip link add mid.r type veth peer name crt.l\n'
    c += 'ip link add crt.r type veth peer name cli.l\n'

    c += 'ip link set dev srv.r netns srv\n'
    c += 'ip link set dev srt.r netns srt\n'
    c += 'ip link set dev srt.l netns srt\n'
    c += 'ip link set dev mid.r netns mid\n'
    c += 'ip link set dev mid.l netns mid\n'
    c += 'ip link set dev crt.l netns crt\n'
    c += 'ip link set dev crt.r netns crt\n'
    c += 'ip link set dev cli.l netns cli\n'

    c += 'ip netns exec srv ip link set srv.r up\n'
    c += 'ip netns exec srt ip link set srt.r up\n'
    c += 'ip netns exec srt ip link set srt.l up\n'
    c += 'ip netns exec mid ip link set mid.r up\n'
    c += 'ip netns exec mid ip link set mid.l up\n'
    c += 'ip netns exec crt ip link set crt.r up\n'
    c += 'ip netns exec crt ip link set crt.l up\n'
    c += 'ip netns exec cli ip link set cli.l up\n'

    # Disable TSO, GSO, GRO, or else netem limit is interpreted per
    # multi-MSS skb, not per packet on the emulated wire.
    c += 'ip netns exec srt ethtool -K srt.r tso off gso off gro off\n'
    c += 'ip netns exec mid ethtool -K mid.l tso off gso off gro off\n'
    c += 'ip netns exec mid ethtool -K mid.r tso off gso off gro off\n'
    c += 'ip netns exec srt ethtool -K crt.l tso off gso off gro off\n'

    # server
    c += 'ip netns exec srv ip addr add 192.168.0.1/24 dev srv.r\n'

    # server router
    c += 'ip netns exec srt ip addr add 192.168.0.100/24 dev srt.l\n'
    c += 'ip netns exec srt ip addr add 192.168.1.1/24   dev srt.r\n'

    # mid
    c += 'ip netns exec mid ip addr add 192.168.1.100/24 dev mid.l\n'
    c += 'ip netns exec mid ip addr add 192.168.2.1/24   dev mid.r\n'

    # client router
    c += 'ip netns exec crt ip addr add 192.168.2.100/24 dev crt.l\n'
    c += 'ip netns exec crt ip addr add 192.168.3.1/24   dev crt.r\n'

    # client
    c += 'ip netns exec cli ip addr add 192.168.3.100/24 dev cli.l\n'

    run(c)

def setup_routes():
    """Set up all routes."""
    c = ''

    # server
    c += 'h=srv\n'
    c += 'ip netns exec $h tc qdisc add dev $h.r root fq\n'
    c += 'ip netns exec $h ip route add default via 192.168.0.100 dev $h.r\n'

    # server router
    c += 'h=srt\n'
    c += 'ip netns exec $h ip route add default via 192.168.1.100 dev $h.r\n'

    # mid
    c += 'h=mid\n'
    c += 'ip netns exec $h ip route add 192.168.3.0/24 via 192.168.2.100\n'
    c += 'ip netns exec $h ip route add default via 192.168.1.1 dev $h.l\n'

    # client router
    c += 'h=crt\n'
    c += 'ip netns exec $h ip route add default via 192.168.2.1 dev $h.l\n'

    # cli
    c += 'h=cli\n'
    c += 'ip netns exec $h ip route add default via 192.168.3.1 dev $h.l\n'

    run(c)

def setup_forwarding():
    """Enable forwarding in each namespace."""
    for host in HOSTS:
        run('ip netns exec %(host)s sysctl -q -w '
            'net.ipv4.ip_forward=1 '
            'net.ipv6.conf.all.forwarding=1'  % {'host' : host})

def netem_limit(rate, delay, buf):
    """Get netem limit in packets.

    Needs to hold the packets in emulated pipe and emulated buffer.
    """
    bdp_bits = (rate * 1000000.0) * (delay / 1000.0)
    bdp_bytes = bdp_bits / 8.0
    bdp = int(bdp_bytes / 1500.0)
    limit = bdp + buf
    return limit

# Parse string like 'cubic:1,bbr:2' and return an array like:
# ['cubic', 'bbr', 'bbr']
def parse_cc_param(param_string):
    cc_list = []
    groups = param_string.split(',')
    for group in groups:
        (cc_name, count) = group.split(':')
        count = int(count)
        for i in range(0, count):
            cc_list.append(cc_name)
    return cc_list

def get_params():
    # Invocations of this tool should set the following parameters as
    # environment variables.
    params = {
        'bw':          -1, # input bottleneck bw in Mbit/sec; required
        'rtt':         -1, # RTT in ms; required
        'buf':         -1, # input bottleneck buffer in packets; required
        'loss':         0, # input bottleneck loss rate in percent; optional
        'policer':      0, # input bottleneck policer rate, Mbit/sec; optional
        'cc':          '', # congestion control algorithm: required
        'interval':     0, # interval between flow starts, in secs; optional
        'dur':         -1, # length of test in secs: required
        'outdir':      '', # output directory for results
        'qdisc':       '', # qdisc at downstream bottleneck (empty for FIFO)
        'cmd':         '', # command to run (e.g. set sysctl values)
        'pcap':         0, # bytes per packet to capture; 0 for no tracing
    }

    for key in params.keys():
        print('parsing key %s' % key)
        if key in os.environ:
           print('looking at env var with key %s, val %s' % (key, os.environ[key]))
        else:
           print('no env var with key %s' % (key))
        if key not in os.environ:
            if params[key] != 0:
              sys.stderr.write('missing %s in environment variables\n' % key)
              sys.exit(1)
        elif key == 'cc':
            params[key] = parse_cc_param(os.environ[key])
        elif type(params[key]) == str:
            params[key] = os.environ[key]
        else:
            params[key] = float(os.environ[key])

    print(params)
    params['netperf'] = netperf()
    params['receiver_ip'] = '192.168.3.100'
    # 10Gbit/sec * 100ms is 125MBytes, so to tolerate
    # high loss rates and lots of SACKed data, we use
    # 512MByte socket send and receive buffers:
    params['mem'] = 536870912
    return params

# Put bandwidth rate limiting using HTB, tied to user-specified
# queuing discipline at that bottleneck, on traffic coming in the cli.l device.
def setup_htb_and_qdisc(d):
    """Set up HTB for rate limiting, and user-specified qdisc for the queue."""

    c = ''

    # First load the necessary modules.
    c += ('rmmod ifb\n'
          'modprobe ifb numifbs=10\n'
          'modprobe act_mirred\n')

    # Clear old queuing disciplines (qdisc) on the interfaces
    d['ext']         = 'cli.l'
    d['ext_ingress'] = 'cli.ifb0'
    d['host'] = 'cli'
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc del dev %(ext)s root\n') % d
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc del dev %(ext)s ingress\n') % d
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc del dev %(ext_ingress)s root\n') % d
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc del dev %(ext_ingress)s ingress\n') % d

    # Create ingress ifb0 on client interface.
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc add dev %(ext)s handle ffff: ingress\n') % d
    c += ('ip netns exec %(host)s '
          'ip link add %(ext_ingress)s type ifb\n') % d
    c += ('ip netns exec %(host)s '
          'ip link set dev %(ext_ingress)s up\n') % d
    c += ('ip netns exec %(host)s '
          'ifconfig %(ext_ingress)s txqueuelen 128000\n') % d
    c += ('ip netns exec %(host)s '
          'ifconfig %(ext_ingress)s\n') % d

    # Forward all ingress traffic to the IFB device.
    c += ('ip netns exec %(host)s '
          '%(tc)s filter add dev %(ext)s parent ffff: protocol all u32 '
          'match u32 0 0 action mirred egress redirect '
          'dev %(ext_ingress)s\n') % d

    # Create an egress filter on the IFB device.
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc add dev %(ext_ingress)s root handle 1: '
          'htb default 11\n') % d

    # Add root class HTB with rate limiting.
    c += ('ip netns exec %(host)s '
          '%(tc)s class add dev %(ext_ingress)s parent 1: classid 1:11 '
          '  htb rate %(IRATE)sMbit ceil %(IRATE)sMbit\n') % d

    # Add qdisc for downstream bottleneck.
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc add dev %(ext_ingress)s parent 1:11 handle 20: '
          '%(QDISC)s\n') % d

    c += ('ip netns exec %(host)s %(tc)s -stat qdisc show\n') % d

    return c

def setup_netem(params):
    """Set up netem on the crt (client router) host."""

    d = {}

    # Parameters for data direction.
    d['IRATE']   = params['bw']      # Mbit/sec
    d['IDELAY']  = params['rtt'] / 2 # ms
    d['IBUF']    = params['buf']     # packets
    d['ILOSS']   = params['loss']
    d['IREO']    = 0  # TODO: not implemented yet
    d['ILIMIT'] = netem_limit(rate=d['IRATE'], delay=d['IDELAY'], buf=d['IBUF'])
    d['POLICER'] = params['policer'] # Mbit/sec
    d['QDISC']   = params['qdisc']

    # Parameters for ACK direction.
    d['ORATE']  = 1000 # Mbit/sec; TODO: not implemented yet
    d['ODELAY'] = params['rtt'] / 2 # ms
    d['OBUF']   = 1000 # packets; TODO: not implemented yet
    d['OLOSS']  = 0  # TODO: not implemented yet
    d['OREO']   = 0  # TODO: not implemented yet
    d['OLIMIT'] = netem_limit(rate=d['ORATE'], delay=d['ODELAY'], buf=d['OBUF'])

    d['tc'] = TC_PATH

    c = ''

    # TODO: fix the policer mechanism to actually work...
    if params['policer'] > 0:
        d['host'] = 'mid'
        c = ('ip netns exec %(host)s '
             '%(tc)s filter list dev  %(host)s.r\n'%
             d)
        run(c)

        c = ('ip netns exec %(host)s '
             '%(tc)s qdisc add dev %(host)s.l ingress\n' %
             d)
        run(c)

        c = ('ip netns exec %(host)s '
             '%(tc)s filter add dev %(host)s.l '
             'parent 1: protocol ip prio 10 u32 '
             'match ip src 192.168.0.1/32 flowid 1:2 '
             'action police rate %(POLICER)sMbit burst 100k drop\n' %
             d)
        run(c)
        c = ''

    if d['QDISC'] == '':
        # If the user doesn't need a fancy qdisc, and FIFO will do,
        # then use netem for rate limiting and buffering,
        # since netem seems more accurate than HTB.
        d['INETEM_RATE'] = 'rate %(IRATE)sMbit' % d
    else:
        d['INETEM_RATE'] = ''
        d['ILIMIT'] = '%d' % (2*1000*1000*1000) # buffer is in user's qdisc

    # Inbound from sender -> receiver. Downstream rate limiting is on cli.l.
    d['host'] = 'crt'
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc add dev %(host)s.r root netem '
          'limit %(ILIMIT)s delay %(IDELAY)sms %(IREO)sms '
          'loss random %(ILOSS)s%% %(INETEM_RATE)s\n') % d

    # Outbound from receiver -> sender.
    d['host'] = 'crt'
    c += ('ip netns exec %(host)s '
          '%(tc)s qdisc add dev %(host)s.l root netem '
          'limit %(OLIMIT)s delay %(ODELAY)sms %(OREO)sms '
          'loss random %(OLOSS)s%% '
          'rate %(ORATE)sMbit\n') % d

    c += ('ip netns exec %(host)s %(tc)s -stat qdisc show\n') % d

    if (d['QDISC'] != ''):
        c += setup_htb_and_qdisc(d)

    run(c)

def ss_log_thread(params):
    """Repeatedly run ss command and append log to file."""
    dur = params['dur']
    outdir = params['outdir']
    ss_log_path = os.path.join(outdir, 'ss.log')
    receiver_ip = params['receiver_ip']
    num_conns = len(params['cc'])

    t0 = time.time()
    t = t0
    port_cnt = num_conns
    f = open(ss_log_path, 'w')
    f.truncate()
    f.close()
    if IP_MODE == socket.AF_INET6:
        ss_ip = '[%s]'
    else:
        ss_ip = '%s'
    ss_ip %= receiver_ip
    ss_cmd = ('ip netns exec srv '
              '%s -tinm "dport >= :%d and dport < :%d and dst %s" >> %s' % (
                  SS_PATH,
                  FIRST_PORT, FIRST_PORT + port_cnt, ss_ip, ss_log_path))

    while t < t0 + dur:
        f = open(ss_log_path, 'a')
        f.write('# %f\n' % (time.time(),))
        f.close()
        run(ss_cmd, verbose=False)
        t += SS_INTERVAL_SECONDS
        to_sleep = t - time.time()
        if to_sleep > 0:
            time.sleep(to_sleep)

def launch_ss(params):
    t = threading.Thread(target=ss_log_thread, args=(params,))
    t.start()
    return t

def run_test(params):
    """Run one test case."""
    print('command: %s' % (sys.argv))
    run('uname -a; date; uptime')
    run('grep . /sys/module/tcp_bbr2/parameters/*')
    run('sysctl net.ipv4.tcp_ecn')

    # Configure sender namespaces.
    run('ip netns exec srv bash -c "%s"' % params['cmd'])

    # Configure receiver namespace.
    run('ip netns exec cli bash -c "%s"' % params['cmd'])

    # Set up receiver process.
    run('pkill -f netserver')
    run('ip netns exec cli %s -N' % (netserver()))

    # Set up output directory.
    outdir = params['outdir']
    run('mkdir -p %s' % outdir)

    # Set up sender-side packet capture.
    if params['pcap'] > 0:
        snaplen = params['pcap']
        path = os.path.join(outdir, 'out.pcap')
        run('ip netns exec srv tcpdump -i srv.r -s %(snaplen)d -w %(path)s &' %
            {'path': path, 'snaplen': snaplen})

    # Set up periodic sender-side 'ss' stat capture.
    ss_thread = launch_ss(params)

    if sys.argv[1] == 'stream':
        num_conns = len(params['cc'])
        print('num_conns = %d' % (num_conns))
        t0 = time.time()
        t = t0
        for i in range(0, num_conns):
            conn_params = params.copy()
            if i != num_conns - 1:
                conn_params['bg'] = '&'  # all but the last in the background
            else:
                conn_params['bg'] = ''
            conn_params['cc'] = params['cc'][i]
            conn_params['port'] = FIRST_PORT + i
            conn_params['outfile'] = '%s/netperf.out.%d.txt' % (outdir, i)
            run('ip netns exec srv %(netperf)s '
                '-l %(dur)d -H %(receiver_ip)s -- -k THROUGHPUT '
                '-s %(mem)s,%(mem)s -S %(mem)s,%(mem)s '
                '-K %(cc)s -P %(port)s '
                '> %(outfile)s '
                '%(bg)s' % conn_params)
            t += params['interval']
            to_sleep = t - time.time()
            if to_sleep > 0:
                time.sleep(to_sleep)
    elif sys.argv[1] == 'rr':
        params['request_size'] = (10 + 20 + 40 + 80 + 160) * 1448
        params['test'] = sys.argv[2]
        conn_params['port'] = FIRST_PORT
        run('ip netns exec srv %(netperf)s '
            ' -P 0 -t %(test)s -H %(receiver_ip)s -- '
            '-K %(cc)s -P %(port)s '
            '-r %(request_size)d,1 '
            '-o P50_LATENCY,P90_LATENCY,P99_LATENCY,MAX_LATENCY,'
            'TRANSACTION_RATE,'
            'LOCAL_TRANSPORT_RETRANS,REMOTE_TRANSPORT_RETRANS' % params)
    else:
        sys.stderr.write('unknown test type argument: %s\n' % sys.argv[1])
        sys.exit(1)

    ss_thread.join()
    run('killall tcpdump')

    run('ls -l /tmp/*.gz')
    run('cp -af /var/log/kern-debug.log ' + outdir)
    run('rm -f ' + outdir + '/*.gz')
    run('ls -l /tmp/*.gz')
    run('gzip '  + outdir + '/kern-debug.log')
    run('gzip  ' + outdir + '/out.pcap')
    run('ls -l /tmp/*gz')

def main():
    """Main function to run everything."""
    params = get_params()
    cleanup()
    setup_logging()
    setup_namespaces()
    setup_loopback()
    setup_veth()
    setup_routes()
    setup_forwarding()
    setup_netem(params)
    run_test(params)
    cleanup()
    return 0


if __name__ == '__main__':
    sys.exit(main())
