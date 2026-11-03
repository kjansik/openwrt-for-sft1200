# OpenWrt 25.12 for GL.iNet GL-SFT1200 (Opal) — SiFlower SF19A28

**Community build. NOT official OpenWrt.**

## Download

Release `v1-sft1200-25.12` (commit `6eacd0037fcec8b09443baf340551d47be013767`).
Both images are also attached to the release page:

https://codeberg.org/kjansik/openwrt-for-sft1200/releases/tag/v1-sft1200-25.12

- sysupgrade.tar (from a running OpenWrt):
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v1-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-sysupgrade.tar
- factory.img (U-Boot recovery only):
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v1-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-factory.img
- checksums:
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v1-sft1200-25.12/images/SHA256SUMS

For a frozen link that never moves even if the tag is ever re-pointed, swap
`tag/v1-sft1200-25.12` for `commit/6eacd0037fcec8b09443baf340551d47be013767`:
`raw/commit/<sha>/...` returns the same bytes.

Verify before flashing: download the images and `images/SHA256SUMS` from the
same ref, put them in one directory and run `sha256sum -c SHA256SUMS` there.
That in-repository file (two lines) is the authoritative one.

Do **not** use the `sha256sums` asset attached to the release page yet. It
lists seven files, and five of them - `*.manifest`, `config.buildinfo`,
`feeds.buildinfo`, `version.buildinfo`, `profiles.json` - are not published
anywhere, so `sha256sum -c` reports `WARNING: 5 listed files could not be
read`. It is a leftover from the build that produced the images; see
"Provenance for the release" below.

Note on URL shapes (Forgejo): `raw/branch/<name>` works only for branches;
for tags use `raw/tag/<tag>`; for a frozen link use `raw/commit/<sha>`.

Note on hashes: the `.tar`/`.img` hashes in SHA256SUMS identify the exact build
tested on hardware. A rebuild from the same source produces different `.tar`/`.img`
hashes (squashfs timestamps), so compare `*.manifest`, `config.buildinfo`,
`feeds.buildinfo` and `version.buildinfo` from your own
`bin/targets/siflower/sf19a2890/` against the build they describe. Those
provenance files are not published under `images/` yet - only the two images and
SHA256SUMS are, so today that comparison only works against a build you make
yourself.

### What the released image says about itself

`/etc/openwrt_release` inside the published rootfs reports
`DISTRIB_REVISION='r0-8c6241b'` and `DISTRIB_DESCRIPTION='OpenWrt 25.12-SNAPSHOT
r0-8c6241b'`, while the release tag points at `6eacd00`. The image is newer than
that string suggests: `/usr/share/hostap/common.uc` inside it is byte-identical
to this branch (`sha256 1407a8dd…`), so it already carries the fix from
`953b554`, which is three commits after `8c6241b`. The images were built from a
work tree that still carried uncommitted fixes, so the generated revision
lags. This is a good example of why the provenance files matter: without them
the only trace of the source tree is a string that turns out to be wrong.

OpenWrt 25.12 image for the GL-SFT1200, including the driver and script fixes
needed to use it as a travel router with a working 5 GHz repeater.

## Build

- Target: siflower / sf19a2890, device glinet_gl-sft1200, kernel 6.12.112
- Config used for releases: configs/sft1200-25.12.config

    cp configs/sft1200-25.12.config .config
    make defconfig
    make -j$(nproc) download
    make -j$(nproc) V=s

Images land in bin/targets/siflower/sf19a2890/.

## Continuous integration

The repository carries the full upstream OpenWrt CI in .github/workflows/.
Those workflows target upstream buildbot and do not build this port. Forgejo
(and therefore Codeberg) only reads the *first* workflow directory it finds,
checking .forgejo/workflows, then .gitea/workflows, then .github/workflows.
Because .forgejo/workflows now exists, the upstream workflows are not run
here at all.

- .forgejo/workflows/sft1200-consistency.yml (hosted runner, ~1 min)
  Runs on push to sync and on pull requests. It does not compile anything.
  It checks that images/ still matches SHA256SUMS, that
  configs/sft1200-25.12.config still enables the packages the sections above
  document, that the in-tree SiFlower fixes are still present **with their
  content** (the patched files are upstream code and survive an upstream sync
  even when the fix inside them is gone, so the job greps for the fixed line
  instead of only testing that the file exists), that no two paths collide
  when case is folded, and that .gitattributes keeps the * -text rule.

  A real buildroot build takes hours. Codeberg's hosted runners are capped at
  2/5/10 minutes and their policy rules out heavy pipelines, so no hosted job
  attempts to compile.

- .forgejo/workflows/sft1200-build.yml (self-hosted runner, manual only)
  Builds the images and collects the provenance files. It only runs through
  workflow_dispatch, never on push. It requires a runner registered on a
  machine you own, labelled sft1200; see the header of that file. It stops
  with an error when `make defconfig` rewrites the shipped config, and it
  compares the built `*.manifest` and `config.buildinfo` against the published
  ones instead of comparing image hashes - a rebuild rewrites the squashfs
  timestamps, so equal image hashes were never a reproducibility test.

## Provenance for the release

The release ships the two images but not the build metadata. OpenWrt writes
*.manifest, config.buildinfo, feeds.buildinfo and version.buildinfo next to
the images in bin/targets/siflower/sf19a2890/, and those are what make a
release auditable. They cannot be reconstructed afterwards, so they have to
come from the build that produced the images:

    scripts/sft1200-collect-provenance.sh

Run it on the build machine, from the top of the repository. It refuses to
copy anything unless the images it finds in the build output are byte
identical to the published ones, so provenance from an unrelated build can
never end up attached to this release. It then updates images/SHA256SUMS.

Either that, or run .forgejo/workflows/sft1200-build.yml on your own runner,
which produces the images and the provenance files together as an artifact.

## What works

- Switch (mxl-gsw1xx, GSW150 variant detection), LAN1/LAN2/WAN, 1 Gbps
- 2.4 GHz AP, 5 GHz AP
- 5 GHz repeater: STA uplink + AP on the same radio
- TUN, WireGuard, OpenVPN, tinc

## Known limitations

- 5 GHz is HT40 only (~300 Mbit/s PHY). VHT must stay disabled: with a VHT80
  STA the LMAC firmware force-merges the AP vif into the STA context
  (lmac: The AP channel should follow the sta channel), the AP stops
  beaconing, clients discard the cached PSK and re-ask for the password, and
  CCA/ACS loops at step 6.
- radio1 must use a fixed NON-DFS channel. With channel=auto the ACS picked a
  DFS channel (104/116), the AP never came up. Channels 36-48 and 149-165 are
  non-DFS. Not yet verified whether every non-DFS channel behaves the same.
- travelmate works on both bands. Before enabling it, remove the STA vif and the
  wwan interface that ship in this build:

      uci delete wireless.wifinet4      # or wifinet3 on radio0
      uci delete network.wwan
      uci commit

  travelmate then creates its own trm_uplink/trm_wwan. If the pre-configured
  STA is left in place, netifd and travelmate fight over the same radio and the
  link keeps being torn down.
- AP and STA must never share one radio with different channel widths.
- 2.4 GHz is HT20 only.
- iw dev phyX-staN link reports unreliable RX counters even while traffic
  flows; use /sys/class/net/*/statistics/.

## Flashing

- sysupgrade.tar  - from a running OpenWrt (LuCI -> Flash Firmware)
- factory.img     - U-Boot recovery only

NEVER flash mtd0-mtd3 (spl / u-boot / u-boot-env / factory).
factory holds the WiFi calibration and MAC addresses. Back it up first:

    dd if=/dev/mtd3ro of=/tmp/factory.bin
    scp root@<ip>:/tmp/factory.bin .

Keep GL.iNet U-Boot recovery (192.168.1.1 with reset held) available.

## Patches in this tree

- package/network/config/wifi-scripts/.../common.uc
  template literal -> string concatenation; ucode could not compile it,
  so no STA vif was created.

- package/kernel/sf_smac/.../siwifi_main.c
  STA chanctx linked before connect; chanctx reuse without CSA;
  width check + bounds guard in check_vif_channel_same().

- package/kernel/sf_smac/.../siwifi_utils.c
  recover a missing chanctx; ch_idx bounds guard; skip recovery while the
  STA has carrier; in-place chan_def resync.

- package/kernel/sf_smac/.../siwifi_mod_params.c
  disable VHT on 5 GHz (same class of bug that 8c6241b fixed on 2.4 GHz).

## Licence

GPL-2.0. The corresponding source for every released image is this
repository, including configs/sft1200-25.12.config.
