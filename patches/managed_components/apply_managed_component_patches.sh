#!/bin/bash
# Apply local patches to managed (registry) components.
#
# Managed components live in <example>/managed_components/, which is generated and
# git-ignored, so a fix that lives only there is lost on a clean checkout - and the
# component manager restores a component from its checksum whenever it decides the
# on-disk copy has drifted, which silently throws these patches away mid-configure.
# media_stream's CMakeLists therefore runs this at configure time under
# CONFIG_KVS_AUTO_APPLY_PATCHES; see the exit codes below for how it reports back.
#
#   ./patches/managed_components/apply_managed_component_patches.sh examples/streaming_only
#
# The argument may be relative to the repo root or absolute.
#
# Idempotent: a patch that is already applied is skipped. Exit codes:
#   0   nothing to do - every patch was already applied, or there is no
#       managed_components/ yet (a fresh configure, before the component manager has
#       materialised anything)
#   10  at least one patch was applied just now. The caller must treat this as
#       "re-configure required": two of these patches add Kconfig options, and Kconfig
#       has already been read by the time any component CMakeLists runs, so this
#       configure is working from a sdkconfig that predates them.
#   1   a patch failed to apply
#
# Each patch here is a stop-gap for an upstream fix that has not been released yet;
# drop it once the component version that carries the fix is published and pinned.

set -e

REPO_ROOT="$(git rev-parse --show-toplevel)"
EXAMPLE_DIR="${1:-examples/streaming_only}"
case "$EXAMPLE_DIR" in
    /*) MC="$EXAMPLE_DIR/managed_components" ;;
    *)  MC="$REPO_ROOT/$EXAMPLE_DIR/managed_components" ;;
esac
PATCH_DIR="$REPO_ROOT/patches/managed_components"

# Tracks whether this run changed anything, which is what turns into exit 10.
APPLIED_ANY=0

if [ ! -d "$MC" ]; then
    echo "note: $MC does not exist yet - nothing to patch. The component manager"
    echo "      materialises it during the build; this script runs again then."
    exit 0
fi

apply_one() {
    local patch="$1" target_dir="$2" expect_version="$3" component="$4"

    if [ ! -d "$target_dir" ]; then
        echo "skip $(basename "$patch"): $component not present in $EXAMPLE_DIR"
        return 0
    fi

    local have
    have="$(grep -E '^version:' "$target_dir/idf_component.yml" | awk '{print $2}' | tr -d '"'"'"'')"
    if [ "$have" != "$expect_version" ]; then
        echo "WARNING $(basename "$patch"): $component is $have, patch was made against $expect_version."
        echo "        Verify the fix is still needed (it may have been released upstream) before forcing it."
        return 0
    fi

    if patch -p1 -d "$target_dir" --dry-run --reverse --force < "$patch" >/dev/null 2>&1; then
        echo "already applied: $(basename "$patch")"
        return 0
    fi

    echo "applying: $(basename "$patch") -> $component ($have)"
    patch -p1 -d "$target_dir" --fuzz=0 < "$patch"
    APPLIED_ANY=1
}

# usb_host_uvc: cameras that never set the EoF payload flag (observed on a Logitech H.264
# stream) lose every frame, because the isoc path only completes a frame on EoF and resets
# the buffer at the frame-ID toggle. Upstream fixed the equivalent bulk case in 2.5.1; the
# isoc path is still affected as of 2.5.1.
apply_one "$PATCH_DIR/0001-usb_host_uvc-complete-frame-on-frame-id-toggle.patch" \
          "$MC/espressif__usb_host_uvc" "2.4.2" "usb_host_uvc"

# usb_host_uvc: reach the camera's own H.264 encoder (bitrate, key-frame period) through the
# H.264 extension unit, and ask for a key frame on demand. Without this the camera streams at
# whatever rate it likes with a multi-second GOP.
apply_one "$PATCH_DIR/0002-usb_host_uvc-h264-extension-unit-rate-control.patch" \
          "$MC/espressif__usb_host_uvc" "2.4.2" "usb_host_uvc"

# esp_video: apply that configuration between format_select() and stream_start(), which is the
# only point the camera accepts it.
apply_one "$PATCH_DIR/0003-esp_video-apply-uvc-h264-config-before-stream-start.patch" \
          "$MC/espressif__esp_video" "2.0.1" "esp_video"

if [ "$APPLIED_ANY" -eq 1 ]; then
    echo "done (patches applied - a re-configure is required, see exit codes above)"
    exit 10
fi

echo "done (all patches were already applied)"
