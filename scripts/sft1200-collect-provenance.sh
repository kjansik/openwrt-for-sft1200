#!/bin/sh
# Collect OpenWrt provenance files for the published SFT1200 images.
#
# OpenWrt writes *.manifest / *.buildinfo next to the images in
# bin/targets/siflower/sf19a2890/. They are what makes a release
# reproducible and auditable, but they cannot be invented: they must come
# from the real build that produced the images in images/.
#
# Run this on the machine where you built, after `make` finished:
#
#   scripts/sft1200-collect-provenance.sh
#
# Then commit images/ and (optionally) re-tag the release.
set -eu

TARGET_DIR=${TARGET_DIR:-bin/targets/siflower/sf19a2890}
IMAGE_DIR=${IMAGE_DIR:-images}
FW=openwrt-siflower-sf19a2890-glinet_gl-sft1200

[ -d "$TARGET_DIR" ] || {
	echo "error: $TARGET_DIR not found - run this where you built, or set TARGET_DIR" >&2
	exit 1
}
[ -d "$IMAGE_DIR" ] || {
	echo "error: $IMAGE_DIR not found - run from the top of the repository" >&2
	exit 1
}

# Refuse to mix provenance from a different build into published images.
echo "== verifying published images against $IMAGE_DIR/SHA256SUMS"
(cd "$IMAGE_DIR" && sha256sum -c SHA256SUMS) || {
	echo "error: published images do not match SHA256SUMS; refusing to add provenance from an unrelated build" >&2
	exit 1
}

# Same build, or a newer one? Compare the built images with the published ones.
echo "== comparing build output with published images"
changed=0
for f in "$FW-squashfs-factory.img" "$FW-squashfs-sysupgrade.tar"; do
	if [ ! -f "$TARGET_DIR/$f" ]; then
		echo "error: $TARGET_DIR/$f missing - that build did not finish" >&2
		exit 1
	fi
	a=$(sha256sum "$TARGET_DIR/$f" | cut -d' ' -f1)
	b=$(sha256sum "$IMAGE_DIR/$f" | cut -d' ' -f1)
	if [ "$a" = "$b" ]; then
		echo "   same: $f"
	else
		echo "   DIFFERENT: $f"
		echo "             build:     $a"
		echo "             published: $b"
		changed=1
	fi
done

if [ "$changed" -eq 1 ]; then
	cat >&2 <<'EOF'

error: the build output does not match the published images.

The provenance files describe the build, not the published images, so copying
them now would attach wrong metadata to the release. Either publish the new
build (replace images/ and SHA256SUMS) or rebuild at the tagged commit.
EOF
	exit 1
fi

echo "== collecting provenance"
for f in "$FW.manifest" config.buildinfo feeds.buildinfo version.buildinfo; do
	if [ ! -f "$TARGET_DIR/$f" ]; then
		echo "error: $TARGET_DIR/$f missing; cannot record provenance" >&2
		exit 1
	fi
	cp "$TARGET_DIR/$f" "$IMAGE_DIR/$f"
	echo "   $f"
done

# Regenerate the sums so the new files are covered too. Image hashes are
# unchanged, so existing documentation links stay valid.
echo "== updating $IMAGE_DIR/SHA256SUMS"
(cd "$IMAGE_DIR" && sha256sum "$FW-squashfs-factory.img" "$FW-squashfs-sysupgrade.tar" \
	"$FW.manifest" config.buildinfo feeds.buildinfo version.buildinfo > SHA256SUMS)
cat "$IMAGE_DIR/SHA256SUMS"

echo
echo "done. Review, then commit:"
echo "  git add $IMAGE_DIR && git commit -s -m 'images: add build provenance for v1-sft1200-25.12'"
