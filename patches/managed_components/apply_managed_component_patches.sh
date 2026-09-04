#!/bin/bash
# Apply local patches to managed (registry) components.
#
#   ./patches/managed_components/apply_managed_component_patches.sh examples/streaming_only [target]
#
# The first argument may be relative to the repo root or absolute. The second is the IDF
# target; media_stream's CMakeLists passes ${IDF_TARGET}, and when it is omitted we read
# CONFIG_IDF_TARGET out of <example>/sdkconfig. It decides whether the UVC patch set is
# expected at all - see "Which patches are expected on which target" below.
#
# Idempotent: a patch that is already applied is skipped. Exit codes:
#   0   nothing to do - every patch was already applied or was not expected on this
#       target
#   10  at least one patch was applied just now. The caller must treat this as
#       "re-configure required"
#   1   a patch failed to apply, or a component this target is supposed to patch is on
#       a version the patch was not made against
#
# Each patch here is a stop-gap for an upstream fix that has not been released yet;
# drop it once the component version that carries the fix is published and pinned.

set -e

REPO_ROOT="$(git rev-parse --show-toplevel)"
EXAMPLE_DIR="${1:-examples/streaming_only}"
case "$EXAMPLE_DIR" in
    /*) EXAMPLE_ABS="$EXAMPLE_DIR" ;;
    *)  EXAMPLE_ABS="$REPO_ROOT/$EXAMPLE_DIR" ;;
esac
MC="$EXAMPLE_ABS/managed_components"
PATCH_DIR="$REPO_ROOT/patches/managed_components"

# Which patches are expected on which target.
#
# The UVC stack (0002/0003/0004) is esp32s31-only, because that is the only target where
# components/media_stream/idf_component.yml asks for the versions those patches are cut
# against - esp_video 2.4.1 and usb_host_uvc 2.5.2.

TARGET="${2:-}"
if [ -z "$TARGET" ] && [ -f "$EXAMPLE_ABS/sdkconfig" ]; then
    TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' "$EXAMPLE_ABS/sdkconfig" | head -1)"
fi
UVC_PATCH_TARGET="esp32s31"

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

    # --fuzz=0 throughout: a hunk that only matches with fuzz is a hunk whose context has
    # drifted, and patch will happily land it somewhere plausible but wrong. That is how
    # 0003's Kconfig hunk once got applied against the wrong anchor, silently dropping the
    # config it was supposed to add - the build then failed on an undeclared CONFIG_ symbol
    # a long way from the cause. Fail loudly and rebase the patch instead.
    if patch -p1 -d "$target_dir" --dry-run --reverse --force --fuzz=0 < "$patch" >/dev/null 2>&1; then
        echo "already applied: $(basename "$patch")"
        return 0
    fi

    echo "applying: $(basename "$patch") -> $component ($have)"
    patch -p1 -d "$target_dir" --fuzz=0 < "$patch"
    APPLIED_ANY=1
}

# apply_one for a patch that is only expected on one target. Same patching, different
# answer to a version mismatch:
#   - off that target the patch is not wanted at all, so say so and move on;
#   - on it, the component being on another version means the pin in
#     components/media_stream/idf_component.yml and this directory disagree, which is a
#     bug to fix rather than a line to scroll past. Fail the build.
apply_expected() {
    local want_target="$1" patch="$2" target_dir="$3" expect_version="$4" component="$5"

    if [ "$TARGET" != "$want_target" ]; then
        echo "skip $(basename "$patch"): target is ${TARGET:-unknown}, this patch is ${want_target}-only"
        return 0
    fi

    if [ -d "$target_dir" ]; then
        local have
        have="$(grep -E '^version:' "$target_dir/idf_component.yml" | awk '{print $2}' | tr -d '"'"'"'')"
        if [ "$have" != "$expect_version" ]; then
            echo "ERROR $(basename "$patch"): $component on disk is $have, patch was made against $expect_version." >&2
            echo "      $target_dir" >&2
            echo "      On $want_target this is fatal. Either the pin in" >&2
            echo "      components/media_stream/idf_component.yml moved, or this example's" >&2
            echo "      dependencies.lock is stale - the component manager does not re-solve when a" >&2
            echo "      *local* component's manifest changes, so 'rm $EXAMPLE_DIR/dependencies.lock'" >&2
            echo "      and configure again. If the fix has been released upstream, drop the patch;" >&2
            echo "      otherwise rebase it onto $have and update the expected version here." >&2
            exit 1
        fi
    fi

    apply_one "$patch" "$target_dir" "$expect_version" "$component"
}

# usb_host_uvc: reach the camera's own H.264 encoder (bitrate, key-frame period) through the
# H.264 extension unit, and ask for a key frame on demand. Without this the camera streams at
# whatever rate it likes with a multi-second GOP.
apply_expected "$UVC_PATCH_TARGET" \
          "$PATCH_DIR/0002-usb_host_uvc-h264-extension-unit-rate-control.patch" \
          "$MC/espressif__usb_host_uvc" "2.5.2" "usb_host_uvc"

# usb_host_uvc: pick the alternate setting that matches the payload size the camera
# negotiated. uvc_claim_interface() has to run before the probe, so it caps on MAX_MPS_IN
# and takes the largest endpoint the target supports (alt 11, 3 x 1020 = 3060 on the test
# camera) while the probe then negotiates dwMaxPayloadTransferSize = 512 for H.264. Every
# isoc packet comes back COMPLETED with zero bytes. Re-selecting after the probe picks
# alt 3 (512) for H.264, alt 10 (2688) for YUYV, alt 11 at 1080p.
apply_expected "$UVC_PATCH_TARGET" \
          "$PATCH_DIR/0004-usb_host_uvc-match-alt-setting-to-negotiated-payload.patch" \
          "$MC/espressif__usb_host_uvc" "2.5.2" "usb_host_uvc"

# esp_video: apply that configuration between format_select() and stream_start(), which is the
# only point the camera accepts it.
apply_expected "$UVC_PATCH_TARGET" \
          "$PATCH_DIR/0003-esp_video-apply-uvc-h264-config-before-stream-start.patch" \
          "$MC/espressif__esp_video" "2.4.1" "esp_video"

if [ "$APPLIED_ANY" -eq 1 ]; then
    echo "done (patches applied - a re-configure is required, see exit codes above)"
    exit 10
fi

echo "done (all patches were already applied)"
