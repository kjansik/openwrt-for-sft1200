# OpenWrt 25.12 for GL.iNet GL-SFT1200 (Opal) — SiFlower SF19A28

**Community build. NOT official OpenWrt.**

## Download

Release `v3-sft1200-25.12`, built from source commit `51f508c` (the release
commit only adds the images and this documentation). Both images and the build
provenance are in `images/`; the release page points at the same tag:

https://codeberg.org/kjansik/openwrt-for-sft1200/releases/tag/v3-sft1200-25.12

- sysupgrade.tar (from a running OpenWrt):
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v3-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-sysupgrade.tar
- factory.img (U-Boot recovery only):
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v3-sft1200-25.12/images/openwrt-siflower-sf19a2890-glinet_gl-sft1200-squashfs-factory.img
- checksums and provenance:
  https://codeberg.org/kjansik/openwrt-for-sft1200/raw/tag/v3-sft1200-25.12/images/SHA256SUMS
  (`*.manifest`, `config.buildinfo`, `feeds.buildinfo`, `version.buildinfo`
  are published next to the images)

For a frozen link that never moves even if the tag is ever re-pointed, swap
`tag/v3-sft1200-25.12` for `commit/<sha>`, using the full SHA that
`git rev-parse v3-sft1200-25.12` prints: `raw/commit/<sha>/...` returns the
same bytes.

v2 shipped neither `luci-base` nor `uhttpd` because the LuCI feed was missing
from the build tree, although the config asked for it; `travelmate` and `tinc`
were missing for the same reason. v3 restores all of them.

Verify before flashing: download the two images, the four provenance files and
`images/SHA256SUMS` from the same ref, put them in one directory and run
`sha256sum -c SHA256SUMS` there. That in-repository file (six lines) is the
authoritative one.

Note on URL shapes (Forgejo): `raw/branch/<name>` works only for branches;
for tags use `raw/tag/<tag>`; for a frozen link use `raw/commit/<sha>`.

Note on hashes: the `.tar`/`.img` hashes in SHA256SUMS identify the exact build
tested on hardware. A rebuild from the same source produces different `.tar`/`.img`
hashes (squashfs timestamps), so compare `*.manifest`, `config.buildinfo`,
`feeds.buildinfo` and `version.buildinfo` from your own
`bin/targets/siflower/sf19a2890/` against the published ones under `images/`.

### What the released image says about itself

`/etc/openwrt_release` inside the published rootfs reports
`DISTRIB_REVISION='r0-51f508c'` and `DISTRIB_DESCRIPTION='OpenWrt 25.12-SNAPSHOT
r0-51f508c'`, and `images/version.buildinfo` records the same revision. The
images were built from a clean tree at `51f508c`; the release tag points at a
later commit that only adds the images and documentation, so the generated
revision lags the tag by design. With the provenance files published next to
the images, that string is no longer the only trace of the source tree.

OpenWrt 25.12 image for the GL-SFT1200, including the driver and script fixes
needed to use it as a travel router with a working 5 GHz repeater.

## Branches

- `main` mirrors upstream (`scada/openwrt-for-sft1200`) and carries no build
  work. It is the upstream line as it stood at fork time, plus commits that
  were later rewritten, so it can both lack the patches below and contain an
  older variant of them. Do not read `main` as "the current tree".
- `sync` is the integration and build branch: upstream `main` plus the patches
  listed further down. The consistency workflow runs on pushes to it and this
  is the branch the images are built from.
- What you download is a tag (`v3-sft1200-25.12`, ...) rendered from `sync`.
  The links above use `raw/tag/<tag>`, so they keep serving the same bytes no
  matter what happens to any branch.

The fork's default branch is still `main`, so the repository front page shows
an upstream snapshot rather than this work; use the release page or the `sync`
branch to see the state the images were built from.

## Build

- Target: siflower / sf19a2890, device glinet_gl-sft1200, kernel 6.12.112
- Config used for releases: configs/sft1200-25.12.config

    cp configs/sft1200-25.12.config .config
    make defconfig
    make -j$(nproc) download
    make -j$(nproc) V=s

The host needs **gawk**, not mawk: include/scan.awk uses the gawk-only
`asort`, and with mawk the build stops at "function asort never defined"
before any rule is defined. On this machine the root-free host tools live in
~/work/.buildhost/root (usable via `source ~/work/.buildhost/env.sh`);
staging_dir/host/bin/awk symlinks to that gawk, and the binary carries the
RUNPATH `$ORIGIN/../lib/x86_64-linux-gnu` so it resolves libsigsegv.so.2
without the environment being set.

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

The release ships the images together with the build metadata. OpenWrt writes
*.manifest, config.buildinfo, feeds.buildinfo and version.buildinfo next to
the images in bin/targets/siflower/sf19a2890/, and those are what make a
release auditable. They cannot be reconstructed afterwards, so they have to
come from the build that produced the images. Since v3 they are published in
images/ alongside the images:

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
- LuCI web interface (uhttpd + luci-base, bootstrap theme)

## Known limitations

- 5 GHz is HT40 only (~300 Mbit/s PHY). VHT must stay disabled: with a VHT80
  STA the LMAC firmware force-merges the AP vif into the STA context
  (lmac: The AP channel should follow the sta channel), the AP stops
  beaconing, clients discard the cached PSK and re-ask for the password, and
  CCA/ACS loops at step 6.
  A different symptom of the same mismatch: with VHT disabled but htmode left
  at HT80, hostapd never brings the AP up and logs

      nl80211: kernel reports: (extension) channel is disabled
      Could not set channel for kernel driver
      Interface initialization failed

  VHT off is a driver capability, it does not change the configured width, so
  radio1 has to be set explicitly:

      uci set wireless.radio1.htmode='HT40'
      uci commit wireless

  The two errors above are not the same failure: the first is the firmware
  merging a mismatched vif, the second is hostapd refusing the channel.
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
- 2.4 GHz VHT is disabled too (8c6241b). This is a robustness fix, not a
  gate: without it the STA still associates, completes WPA and eventually
  gets a DHCP lease. What the enablement breaks is the low-band rate setup.
  The driver advertises VHT for the 2.4 GHz band, siwifi_send_me_config_req()
  forwards that to the LMAC as vht_supp, and the low-band firmware rejects
  the resulting rate configuration:

      lmac[0] rc_init: station_id=0 format_mod=4 ... max_bw=0
      lmac[0] lb:the value of mode (4) is not right

  (format_mod=4 is FORMATMOD_VHT, hal_desc.h; the string is inside the
  firmware binaries, tagged lb: in the log.) With the capability off the
  same line reports format_mod=2 (FORMATMOD_HT_MF) and the error is gone.
  Measured on an HT20 AP: 14-170 s to IPv4-up (median 24 s) with VHT
  advertised, < 1-7 s (median 2 s) without, 3/5 vs 6/6 HTTPS checks passing
  afterwards. Those
  await times are consistent with DHCP client retransmission backoff after
  lost exchanges, not with a hard link failure.
- iw dev phyX-staN link reports unreliable RX counters even while traffic
  flows; use /sys/class/net/*/statistics/.
- iw dev phyX-apN station dump is partly wrong on the AP side too:
  `tx bitrate` always reads the legacy lowest rate (1.0 MBit/s) because
  siwifi_dump_station_info() fills txrate from stats.last_tx_rate_config,
  which the TX completion path never updates, and `tx retries` / `tx failed`
  are copied from the per-device counters (siwifi_hw->stats), not per station.
  A client pulling 70 MBit/s shows "tx bitrate: 1.0 MBit/s" there. Judge the
  link by measured throughput and by the rx side, not by these fields.

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
  disable VHT on 2.4 GHz (8c6241b) and on 5 GHz (62dee89); same class of bug,
  fixed one band at a time. Without 62dee89 the 5 GHz band still advertises
  VHT (0x038001b1) while 2.4 GHz does not (0x03800191), which is exactly the
  state the AP+STA blocker needs.

## Checking a rebuilt image on a device

    dmesg | grep 'lb:the value of mode'    # nothing after a 2.4 GHz association
    iw phy phy0 info | grep VHT            # nothing: VHT is off on both bands
    iw dev phy1-sta0 link                  # 5 GHz uplink, 40 MHz, no VHT
    uci get wireless.radio1.htmode         # HT40

For the DHCP path, follow the uplink coming up:

    logread -f | grep -i -e udhcpc -e dhcp # lease within a few seconds

If the low band regresses after flashing, the only commit that changes what
the driver sends to the firmware is the ME config band selection
(siwifi_msg_tx.c, siwifi_send_me_config_req()); `git revert` of that commit
returns the tree to the image that was released.

## Licence

GPL-2.0. The corresponding source for every released image is this
repository, including configs/sft1200-25.12.config.
