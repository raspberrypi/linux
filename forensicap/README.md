# ForensicAP Kernel Overlay

This directory contains the **only** ForensicAP-specific additions on top of
the upstream `raspberrypi/linux` kernel.  Everything else in this fork stays
a clean mirror of `rpi-6.12.y` so upstream rebases remain trivial.

## Files

| File | Purpose |
|---|---|
| `v8.config` | Config overlay merged on top of `bcm2711_defconfig` (Pi 4 / CM4 / Zero 2 W) |
| `v8-16k.config` | Config overlay merged on top of `bcm2712_defconfig` (Pi 5 / CM5) |
| `version` | Monotonically incremented integer (`1`, `2`, …). Bumped when the overlay changes, *not* when upstream advances. Used as the `KDEB_PKGVERSION` suffix. |

## Build

The CI workflow `.github/workflows/forensicap-release.yml` produces the
official ForensicAP kernel `.deb` packages.  Triggered by:

- `workflow_dispatch` (manual via GitHub UI)
- Tag push matching `kernel-v*` (e.g. `kernel-v6.12.90-forensicap.1`)

Output: three `.deb` packages per variant, attached to the release:

```
linux-image-<kver>_<pkgver>_arm64.deb
linux-headers-<kver>_<pkgver>_arm64.deb
linux-libc-dev_<pkgver>_arm64.deb
```

Consumed by [`ForensicAP/forensicap-image`](https://github.com/ForensicShark/ForensicAP)
via the `FORENSICAP_KERNEL_RELEASE_TAG` config key in `forensicap-image/config`.

## Local build

To reproduce a release build locally (e.g. for testing an overlay change
before tagging):

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2711_defconfig
scripts/kconfig/merge_config.sh -m .config forensicap/v8.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    KDEB_PKGVERSION="$(cat forensicap/version).0" \
    -j"$(nproc)" bindeb-pkg
```

Resulting `linux-*.deb` files appear in the parent directory.

## Why a config overlay (and not a kernel-source patch)

The two ForensicAP tweaks (`CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=n`,
`CONFIG_LOCALVERSION`) are pure Kconfig changes — no code modifications.
Keeping them as a config snippet means:

- Trivial upstream rebases — no merge conflicts in `net/wireless/Kconfig`
- Reviewable in one place (this directory)
- Easy to test alternative settings without rebuilding patches
