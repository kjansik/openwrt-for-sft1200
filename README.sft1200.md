# OpenWrt 25.12 for GL.iNet GL-SFT1200 (Opal) — SiFlower SF19A28

**Community build. NOT official OpenWrt.**

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
