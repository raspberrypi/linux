#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Regression tests for the SO_TXTIME interface.

Test delivery time in FQ and ETF qdiscs.
"""

import time

from lib.py import ksft_exit, ksft_run, ksft_variants
from lib.py import KsftNamedVariant, KsftSkipEx
from lib.py import NetDrvEpEnv, bkg, cmd, defer, tc


def test_so_txtime(cfg, clockid, ipver, args_tx, args_rx, expect_success):
    """Main function. Run so_txtime as sender and receiver."""
    bin_path = cfg.test_dir / "so_txtime"

    tstart = time.time_ns() + 200_000_000

    cmd_addr = f"-S {cfg.addr_v[ipver]} -D {cfg.remote_addr_v[ipver]}"
    cmd_base = f"{bin_path} -{ipver} -c {clockid} -t {tstart} {cmd_addr}"
    cmd_rx = f"{cmd_base} {args_rx} -r"
    cmd_tx = f"{cmd_base} {args_tx}"

    with bkg(cmd_rx, host=cfg.remote, fail=expect_success,
             expect_fail=(not expect_success), exit_wait=True):
        cmd(cmd_tx)


def _qdisc_setup(ifname, qdisc, optargs=""):
    """Replace root qdisc. Restore the original after the test.

    If the original is mq, children will be of type default_qdisc.
    """
    orig = tc(f"qdisc show dev {ifname} root", json=True)[0].get("kind", None)
    defer(tc, f"qdisc replace dev {ifname} root {orig}")
    tc(f"qdisc replace dev {ifname} root {qdisc} {optargs}")


def _test_variants_mono():
    for ipver in ["4", "6"]:
        for testcase in [
            ["no_delay", "a,-1", "a,-1"],
            ["zero_delay", "a,0", "a,0"],
            ["one_pkt", "a,10", "a,10"],
            ["in_order", "a,10,b,20", "a,10,b,20"],
            ["reverse_order", "a,20,b,10", "b,20,a,20"],
        ]:
            name = f"v{ipver}_{testcase[0]}"
            yield KsftNamedVariant(name, ipver, testcase[1], testcase[2])


@ksft_variants(_test_variants_mono())
def test_so_txtime_mono(cfg, ipver, args_tx, args_rx):
    """Run all variants of monotonic (fq) tests."""
    _qdisc_setup(cfg.ifname, "fq")
    test_so_txtime(cfg, "mono", ipver, args_tx, args_rx, True)


def _test_variants_etf():
    for ipver in ["4", "6"]:
        for testcase in [
            ["no_delay", "a,-1", "a,-1", False],
            ["zero_delay", "a,0", "a,0", False],
            ["one_pkt", "a,10", "a,10", True],
            ["in_order", "a,10,b,20", "a,10,b,20", True],
            ["reverse_order", "a,20,b,10", "b,10,a,20", True],
        ]:
            name = f"v{ipver}_{testcase[0]}"
            yield KsftNamedVariant(
                name, ipver, testcase[1], testcase[2], testcase[3]
            )


@ksft_variants(_test_variants_etf())
def test_so_txtime_etf(cfg, ipver, args_tx, args_rx, expect_fail):
    """Run all variants of etf tests."""
    try:
        _qdisc_setup(cfg.ifname, "etf", "clockid CLOCK_TAI delta 400000")
    except Exception as e:
        raise KsftSkipEx("tc does not support qdisc etf. skipping") from e

    test_so_txtime(cfg, "tai", ipver, args_tx, args_rx, expect_fail)


def main() -> None:
    """Boilerplate ksft main."""
    with NetDrvEpEnv(__file__) as cfg:
        ksft_run([test_so_txtime_mono, test_so_txtime_etf], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
