# TCP BBR v2 Alpha/Preview Release

This document gives a quick overview of Google's TCP BBR v2
alpha/preview release for Linux, and how to download, build, install,
and test it.

The TCP BBR v2 alpha/preview release is intended to enable research
collaboration and wider testing.  We encourage researchers to dive in
and help evaluate/improve the BBR v2 algorithm and code. We welcome
patches with good solutions to issues.

This document shows how to download, build, install, and test
a Linux kernel running TCP BBR v2 alpha.

## License

Like Linux TCP BBR v1, the v2 code is dual-licensed as both GPLv2.0 (like the
Linux kernel) and BSD. You may use it under either license.

## Viewing the TCP BBR v2 alpha sources

You can view the current sources here:
[tcp_bbr2.c](https://github.com/google/bbr/blob/v2alpha/net/ipv4/tcp_bbr2.c)

## Obtaining kernel sources with TCP BBR v2 alpha

There are two main options for downloading the code:

1. To create a new git repo starting from a Linux kernel with TCP BBR v2 alpha,
you can run:

```
git clone -o google-bbr -b v2alpha  https://github.com/google/bbr.git
cd bbr/
```

2. To download the code into an existing git repo, you can use:

```
git remote add google-bbr https://github.com/google/bbr.git
git fetch google-bbr
git checkout google-bbr/v2alpha
```

Note that if you already have a git repo that has imported the Linux source
tree, then the second option will be much faster and use much less space, since
it will only need to download the small deltas relative to the mainline Linux
source distribution.

## Building and installing the kernel

To build a Linux kernel with TCP BBR v2 support, copy that kernel to a target
(Debian or Ubuntu) test machine (bare metal or GCE), and reboot that machine,
you can use the following script, included in the TCP BBR v2 distribution:

```
./gce-install.sh -m ${HOST}
```

## Checking the kernel installation

Once the target test machine has finished rebooting, then ssh to the target
test machine and become root with sudo or equivalent. First check that the
machine booted the kernel you built above:

```
uname -a
```

You should see the branch name SHA1 hash, and build time stamp from the kernel
you built above.


Then check what congestion control modules are available with:
```
sysctl net.ipv4.tcp_available_congestion_control
```

You should see something like:
```
net.ipv4.tcp_available_congestion_control = reno bbr bbr2 cubic dctcp
```

## Install test dependencies

Next, copy the test scripts to the target test machine with:

```
scp -r gtests/net/tcp/bbr/nsperf/ ${HOST}:/tmp/
```

Before running the tests for the first time, as a one-time step you'll need to
install the dependencies on the test machine, as root:

```
mv /tmp/nsperf /root/
apt-get install --yes python netperf gnuplot5-nox
```

The 'tc' and 'ss' binaries on some prominent distributions, including Ubuntu 18
LTS, are out of date and buggy. To run the TCP BBR v2 test scripts, you will
probably need to download and use the latest versions:

```
apt-get install pkg-config bison flex
mkdir -p /root/iproute2/
cd /root/iproute2
git clone git://git.kernel.org/pub/scm/network/iproute2/iproute2.git
cd iproute2/
./configure
make
```

## Running TCP BBR v2 tests and generating graphs

To run the tests, ssh to the target test machine and become root with sudo or
equivalent. Then run the tests and generate graphs with:

```
cd /root/nsperf
./run_tests.sh
./graph_tests.sh
```

This will run for hours, and place the graphs in the ./graphs/ directory.

You can run and graph a subset of the tests by specifying the test by name as
an environment variable. For example:

```
cd /root/nsperf
tests=random_loss ./run_tests.sh
tests=random_loss ./graph_tests.sh
```

Enjoy!

## Release Notes and Details

### Enabling ECN support

For lab testing, researchers can enable BBRv2 ECN support with the following
commands. This is for use when you know that any ECN marks that the connections
experience will be DCTCP/L4S-style ECN marks, rather than RFC3168 ECN marks:
```
# negotiate TCP ECN for active and passive connections:
sysctl net.ipv4.tcp_ecn=1
# enable BBRv2 ECN response:
echo 1 > /sys/module/tcp_bbr2/parameters/ecn_enable
# enable BBRv2 ECN response at any RTT:
echo 0 > /sys/module/tcp_bbr2/parameters/ecn_max_rtt_us
```
Production use of the BBRv2 ECN functionality depends on negotiation or
configuration that is outside the scope of the BBRv2 alpha release.

### Enabling experimental pacing approach discussed at IETF 106 ICCRG session

To try the experimental pacing approach described in our IETF 106 presentation,
you can check out the `v2alpha-experimental-pacing` branch from the Google
BBR github repository:
```
git remote add google-bbr https://github.com/google/bbr.git
git fetch google-bbr
git checkout google-bbr/v2alpha-experimental-pacing
```

## FAQ

If you have questions about BBR, check the [BBR FAQ](https://github.com/google/bbr/blob/master/Documentation/bbr-faq.md).
