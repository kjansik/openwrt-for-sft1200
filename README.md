# OpenWrt 25.12 for GL.iNet GL-SFT1200 (Opal) — GitHub mirror

**Community build. NOT official OpenWrt.**

> **Read-only mirror.** The canonical repository, issue tracker and CI live on
> Codeberg: <https://codeberg.org/kjansik/openwrt-for-sft1200>. This GitHub
> repository exists so the build can be found and downloaded from here; see
> [MIRROR.md](MIRROR.md) for how it is kept in step and where to report bugs.
>
> Only the `github-mirror` branch is maintained here. Upstream `main` and the
> integration branch `sync` stay on Codeberg. The tags are the same tags, so a
> tag resolves to the same commit on both hosts.

## Download — release `v4-sft1200-25.12`

Built from source commit `0fdfe4f` (the release commit only adds the images and
the documentation). Provenance — `*.manifest`, `config.buildinfo`,
`feeds.buildinfo`, `version.buildinfo`, `SHA256SUMS` — sits next to the images.

- sysupgrade.tar (from a running OpenWrt):
  https://raw.githubusercontent.com/kjansik/openwrt-for-sft1200/v4-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-sysupgrade.tar
- factory.img (U-Boot recovery only):
  https://raw.githubusercontent.com/kjansik/openwrt-for-sft1200/v4-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-factory.img
- checksums:
  https://raw.githubusercontent.com/kjansik/openwrt-for-sft1200/v4-sft1200-25.12/images/SHA256SUMS

Verify before flashing: download the two images, the four provenance files and
`SHA256SUMS` from the same ref, put them in one directory and run
`sha256sum -c SHA256SUMS`.

URL shapes differ per host: GitHub serves `raw.githubusercontent.com/<owner>/<repo>/<tag>/<path>`,
while `raw/tag/...` is the Forgejo shape and does not exist here. For a link that
never moves, replace the tag with a commit SHA.

## What this is

OpenWrt 25.12 for the SFT1200 (target `siflower/sf19a2890`, device
`glinet_gl-sft1200`, kernel 6.12.112) with the driver and script fixes needed to
use it as a travel router with a working 5 GHz repeater: 5 GHz AP + STA on one
radio, LuCI, travelmate, WireGuard, OpenVPN, tinc. Release config:
`configs/sft1200-25.12.config`.

Read [README.sft1200.md](README.sft1200.md) before flashing — it carries the
known limitations (VHT must stay disabled on both bands, radio1 needs a fixed
non-DFS channel and `htmode=HT40`, travelmate needs the shipped STA vif removed),
the flashing procedure, the provenance check and the patch list.

## Flashing

- `sysupgrade.tar` — from a running OpenWrt (LuCI → Flash Firmware)
- `factory.img` — U-Boot recovery only

NEVER flash mtd0–mtd3 (spl / u-boot / u-boot-env / factory): factory holds the
WiFi calibration and MAC addresses. Back it up first:

    dd if=/dev/mtd3ro of=/tmp/factory.bin
    scp root@<ip>:/tmp/factory.bin .

Keep GL.iNet U-Boot recovery (192.168.1.1 with reset held) available.

## Licence

GPL-2.0 — see [COPYING](COPYING). The corresponding source for every released
image is this repository at the released tag, including
`configs/sft1200-25.12.config`.
