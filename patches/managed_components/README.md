# Managed-component patches

Fixes we need in components that come from the ESP component registry.

`managed_components/` is generated and git-ignored, so anything changed in place is
lost on a clean checkout or a dependency re-resolve. Each fix therefore lives here as
a patch plus a note on what replaces it upstream.

Apply after the first build of an example (that is what materialises the components):

```bash
idf.py -B build.uvc16 build            # once, so managed_components/ exists
./patches/managed_components/apply_managed_component_patches.sh examples/streaming_only
```

The script is idempotent and refuses to force a patch onto a component version it was
not made against. It takes the IDF target as an optional second argument (`media_stream`'s
CMakeLists passes `${IDF_TARGET}`; otherwise it reads `CONFIG_IDF_TARGET` from the
example's `sdkconfig`), because what a mismatch *means* depends on the target - see below.

## The UVC patches are esp32s31-only, and on esp32s31 a mismatch is fatal

`components/media_stream/idf_component.yml` holds `esp_video` at `~2.0.1` on every target
except esp32s31, and requests `usb_host_uvc` on esp32s31 only, so that the board BSPs can
resolve to a version that compiles on ESP-IDF v6.

**0002 and 0004 survived the same audit untouched.** Pristine 2.5.2 has no `uvcx`, no
`UVCX_VIDEO_CONFIG_PROBE`/`COMMIT`, no XU GUID and no `ae_priority`; its H.264 vocabulary is
inert (`UVC_VS_GENERATE_KEY_FRAME_CONTROL` and `UVC_VC_DESC_SUBTYPE_EXTENSION_UNIT` are
enum-only at `private_include/usb_types_uvc.h:51,68` and never issued), and `include/` is
byte-identical to 2.5.1. The alternate setting is still chosen once before the probe from the
`MAX_MPS_IN` constant (`uvc_host.c:595`, called at `:818`, probe at `:843-845`) and never
revisited, and `uvc_host_usb_ctrl()` (`uvc_host.c:1113-1166`) is byte-identical to 2.5.1 with
no EP0 retry. Two known gaps in 0004, neither introduced by the retarget: it passes a
`uint32_t dwMaxPayloadTransferSize` into a `uint16_t` parameter
(`uvc_descriptor_parsing.c:113` - harmless below 64 KiB), and it only covers the
`uvc_host_stream_open()` path, so `uvc_host_stream_format_select()` (`uvc_host.c:983-1022`)
still leaves the alt setting stale after a runtime resolution change.

One free improvement rode along in 2.5.2 and is worth knowing: `uvc_host_stream_close()`
now uses `ESP_ERROR_CHECK_WITHOUT_ABORT` (`uvc_host.c:922`), so closing a stalled or yanked
camera no longer aborts the system. It does **not** replace the STREAMOFF-on-shutdown handler
in `esp_video_if.c` - see `components/media_stream/KNOWN_ISSUES.md` §1.

There is no 0001 any more, and the numbering keeps its gap on purpose - 0002/0003/0004 are
referenced by number from here and from `components/media_stream/KNOWN_ISSUES.md`.

| Patch | Component | Why | Drop when |
|-------|-----------|-----|-----------|
| `0002-usb_host_uvc-h264-extension-unit-rate-control.patch` | `espressif/usb_host_uvc` 2.5.2 | No way to reach the camera's own H.264 encoder. Adds `uvc_host_stream_h264_set_rate()` / `uvc_host_stream_request_key_frame()` over the H.264 extension unit (GUID-located `UVCX_VIDEO_CONFIG_PROBE/COMMIT`), and fills `wWidth`/`wHeight` from the negotiated format so the commit is not rejected. Also pins the frame rate on request (`bmHints` frame-interval bit plus the `FIXED_FRAME_RATE` flag in `bRateControlMode`), which is what chooses whether a too-small bitrate costs frames or sharpness. Adds `uvc_host_stream_set_ae_priority()` over the Camera Terminal's Auto-Exposure Priority control - the SENSOR half of that same choice, and the half that actually dominates indoors. | `usb_host_uvc` exposes encoding-unit controls of its own. |
| `0004-usb_host_uvc-match-alt-setting-to-negotiated-payload.patch` | `espressif/usb_host_uvc` 2.5.2 | Alternate setting is chosen before the probe, so it does not match `dwMaxPayloadTransferSize`; every isoc packet then returns `COMPLETED` with zero bytes. Also makes `uvc_host_usb_ctrl()` survive a transaction error: USBH clears EP0 from the host-library task, not inline, so a resubmit has to yield first - without that a single `Dev 1 EP 0 Error` after a soft reset turned every later control transfer into `ESP_ERR_INVALID_STATE` and the stream never opened. A STALL is deliberately *not* retried (it is the device's definitive answer, and USBH re-arms EP0 itself), and the `usb_host_endpoint_clear(dev_hdl, 0)` this patch used to do is gone - EP0 is not claimable, so it only ever printed two error lines. | Upstream re-selects the alt setting after the probe. |
| `0003-esp_video-apply-uvc-h264-config-before-stream-start.patch` | `espressif/esp_video` 2.4.1 | Opens the UVC stream with the V4L2-selected format instead of `frame_info[0]`, applies the H.264 extension-unit config between `format_select()` and `stream_start()`, and defines `esp_video_uvc_request_key_frame()`. It no longer asks for a key frame at stream start: a camera without that control STALLs it and USBH logs `Dev N EP 0 STALL` at ERROR on every boot for nothing. The root-port-power workaround it once carried is gone — `PRTPWR` does not gate the camera's VBUS, so it never did anything; see `components/media_stream/README.md`. | `esp_video` opens with the selected format and exposes UVC encoder controls. |
| (same patch, added later) | | It also carries `esp_video_uvc_set_h264_start_bitrate()`: the rate asked for at stream start was a compile-time constant, so restarting the stream re-applied the same value. Measured on ESP32-S31, the camera honours the extension-unit PROBE and COMMIT at runtime and then **ignores** the bitrate, keeping what it committed at stream start - and frame rate is no substitute, since it is CBR (30 -> 20 fps left the rate unchanged and doubled per-frame size). A stream restart is therefore the only actuation left, and this is the setter it needs. Folded into 0003 rather than added as its own patch: a separate patch editing lines 0003 *adds* breaks 0003's reverse-dry-run "already applied" check, so the second configure re-applies it and every hunk fails. | |

## Regression: 0003 lost its `.vs_format` change once

The single most load-bearing line in 0003 is the one that opens the UVC stream with the
format V4L2 selected instead of `frame_info[0]`. It went missing during a patch
regeneration and nothing noticed at 640x480, because the mismatch happened to be benign
there. At 1280x720 it showed up as a steady drip of `uvc-isoc: frame error`:

```
Default format from camera: 640x480@30.0FPS, fmt=2 (YUY2)   <- what open probed
Alternate setting 11 -> 10 to match negotiated payload 2688 B
Default format from camera: 1280x720@30.0FPS, fmt=3, payload=1600   <- what we stream
```

`uvc_host_stream_open()` is what probes the camera, picks the alternate setting from
`dwMaxPayloadTransferSize` and allocates the ISOC URBs. `uvc_host_stream_format_select()`
re-commits the format and revisits neither. So the endpoint was sized for 2688 B/microframe
while the camera sent 1600, the camera set the payload-header error bit every few frames,
and each one cost a full GOP while the grabber waited for the next IDR - long enough for
KVS to reject the fragment and force a session restart.

If `frame_info[0]` ever appears in the `stream_config` again, this is the bug.

### The frame-rate/quality trade lives in two places, and the encoder is the lesser one

The H.264 extension unit decides how the encoder spends a bitrate. It cannot decide how
many frames the sensor produces - and in dim light that is the binding constraint, because
auto-exposure lengthens exposure past the frame interval and the sensor rate falls to suit.

Measured on ESP32-S31 at 720p in indoor evening light, with the extension unit alone:
14-15 fps at every bitrate from 800 kbps to 2.4 Mbps. Average frame grew 6.5 -> 20.2 KB and
the frame count never moved. Tripling the bitrate bought only quality, because no encoder
setting can create a frame that was never captured.

The control that does decide it is the Camera Terminal's **Auto-Exposure Priority**
(`CT_AE_PRIORITY_CONTROL`, selector 0x03): 0 holds the frame rate constant and lets the
picture go dark, 1 lets exposure win and the rate fall. Setting it to 0 took the same
camera at the same 800 kbps from 14-15 fps to 29-30.

`bmControls` D2 is checked before writing: a camera that does not implement a control
STALLs EP0 and USBH logs that at ERROR, which would put an error in the log of every boot
on cameras that lack it - the same trap 0003 documents for the key-frame control.

Worth knowing: this camera **clears** the encoder's `FIXED_FRAME_RATE` flag
(`rc_mode 0x01`, never 0x11), so the encoder half of the trade is inert on it and the
sensor half carries the whole result. A camera could plausibly be the other way round,
which is why both are set.

### `bmHints` values are assignments, not bit positions

The H.264 payload spec's `bmHints` bits are not in field order, and 0002 originally guessed
them: `FRAME_INTERVAL` as `0x0001`, `BITRATE` as `0x0002`, `IFRAME_PERIOD` as `0x2000`. The
real values are `0x0800`, `0x2000` and `0x8000`. Nothing failed, because a wrong bit is
still a valid hint for some *other* field - so the guesses quietly asserted `RESOLUTION`,
`PROFILE` and `BITRATE` instead.

The bitrate lever therefore worked only by accident: the value written for the key-frame
period's hint happened to be the bitrate's real bit, and
`CONFIG_ESP_VIDEO_UVC_H264_IFRAME_PERIOD_MS` is non-zero by default, so it was always OR'd
in. Set that option to 0 and the camera would have stopped honouring the bitrate.

`bmHints` says which fields the camera must hold fixed, so every field left unhinted is one
it may trade away. Pinning the frame rate hinted resolution, profile and bitrate and *not*
the frame interval - naming the frame interval as the one thing the camera was free to
sacrifice. It obliged: 30 fps became 15 at an unchanged bitrate, the exact inverse of the
request, and the option read as "the camera ignores the hint". It was doing as it was told.

Two consequences worth keeping:

- `bmHints` is rebuilt from zero on every transaction rather than OR'd into the read-back.
  It is a host-to-device directive, not device state, and accumulating made a hint permanent
  once set - the frame-rate pin could be switched on but never off, which both the
  mid-stream and restart actuation paths need to do.
- Pinning asserts *two* things: the frame-interval hint ("do not change this number") and
  `UVCX_RC_FIXED_FRAME_RATE` in `bRateControlMode` ("do not drop frames to meet the
  bitrate"). A camera may honour either alone, and the second is not implied by the first.

## Clean-rebuild notes

Traps that only show up on a fresh `managed_components/`:

- **`--fuzz=0` is not optional.** `patch` will land a hunk whose context has drifted at a
  plausible-looking nearby anchor. 0003's Kconfig hunk did exactly that: it applied "cleanly",
  silently dropped the `ESP_VIDEO_UVC_PORT_POWER_OFF_MS` option it was meant to add, and the
  build then failed on an undeclared `CONFIG_` symbol far from the cause. The apply script now
  passes `--fuzz=0` on both the already-applied probe and the real apply, so a drifted patch
  fails loudly and gets rebased instead.
- **The component manager silently reverts these patches - now handled automatically.**
  Delete a managed component and let the component manager re-fetch it, and a later
  `idf.py build` may restore it again from its checksum, throwing away every patch applied
  in between with no message. It bit `usb_host_uvc` (0002, 0004 and the since-dropped 0001)
  while 0003 on
  `esp_video` survived the same build, so it is per-component and invisible in the output.
  The symptom was a build failing on `implicit declaration of function
  'uvc_host_stream_h264_set_rate'` - a declaration patch 0002 adds to
  `include/usb/uvc_host.h`.

  `components/media_stream/CMakeLists.txt` now runs this script at configure time under
  `CONFIG_KVS_AUTO_APPLY_PATCHES`, the same switch `kvs_webrtc` uses for the submodule
  patches, so a revert is repaired on the next configure instead of reaching a flash. It
  cannot run any earlier than a component CMakeLists - `managed_components/` does not exist
  until the component manager has run - and by then Kconfig has been read, so a run that
  actually applies something exits **10** and CMake stops with "re-run the build". That is
  deliberate: patch 0003 adds Kconfig options, and continuing would build against a
  sdkconfig that predates them. (0001 used to add one too; 2.5.2 ships it upstream.)

  Still worth knowing when driving the board by hand: never trust a verification run whose
  build did not print "Project build complete", because `idf.py flash` will happily flash
  the previous binary.
- **A version-guard failure on esp32s31 usually means a stale `dependencies.lock`.** The
  guard is fatal there now, and the one thing that trips it in an existing checkout is a
  lock that predates the pins. `dependencies.lock` is git-ignored, so CI always solves
  fresh and gets the pinned versions - but locally the component manager does **not**
  re-solve when a *local* component's manifest changes, only when the project's own
  manifests do. That is why several examples sat on `esp_video` 2.0.1 long after
  `media_stream` pinned 2.4.x, and why a plain reconfigure will not fix them. The remedy is
  one line, per example:

  ```bash
  rm examples/<example>/dependencies.lock     # then configure again
  ```

  Refreshing a long-stale lock does move other components too, so expect to re-verify that
  example rather than assuming it still builds.
- **CMake 4 and the usrsctp submodule.** `components/esp_usrsctp/usrsctp/CMakeLists.txt` asks
  for `cmake_minimum_required(VERSION 3.0)`, and CMake 4 removed compatibility below 3.5. It
  is a submodule, so `examples/kvs_combined/CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM
  3.5` instead of every build needing `-D` on the command line.

## UVC H.264 capture on ESP32-S31 (investigation log, 2026-08-21)

Goal: a USB-UVC camera that encodes H.264 itself, forwarded untouched, at 640x480 then
1280x720 then 1920x1080. **Achieved** with the patches below - see "Where it stands".

The camera advertises JPEG, YUYV and H264, each at 640x480 up to 1920x1080, and exposes an
H.264 extension unit (bUnitID 12 on VideoControl interface 0).

### Bugs found and fixed

1. **`esp_video_if.c`: capture resolution was ignored.** `fallback_resolutions[]` had a
   hardcoded `{640, 480}` ahead of the configured entry, so `S_FMT` always succeeded at
   640x480 on the first attempt and `CONFIG_MEDIA_STREAM_*_CAPTURE_WIDTH/HEIGHT` was never
   requested. Affected **P4 as well**, which was likewise always capturing 640x480.
2. **`esp_video_if.c`: compressed formats need `sizeimage`.** `esp_video_config_buffer()`
   honours `sizeimage` for JPEG/H264 and otherwise uses `width * height * bpp / 8` with
   `bpp = 8` - 307200 at 640x480, against a camera declaring `dwMaxVideoFrameSize` 614400.
   The undersized buffer became `uvc_host_stream_open(.frame_size)` and every frame was
   discarded by `uvc_isoc.c` with only an `ESP_LOGD`.
3. **usb_host_uvc: alternate setting did not match the negotiated payload** (patch 0004).
   `uvc_claim_interface()` runs before the probe, so it caps on `MAX_MPS_IN` (4096) and
   takes the largest endpoint - alt 11 (3 x 1020 = 3060) - while the probe then negotiates
   `dwMaxPayloadTransferSize = 512` for H.264. Re-selecting after the probe picks alt 3
   (512) for H.264, alt 10 (2688) for YUYV, alt 11 at 1080p.
   The camera's alternate settings: 1:192 2:384 3:512 4:640 5:800 6:944 7:1280 8:1600
   9:1984 10:2688 11:3060.
4. **usb_host_uvc: upstream isoc fixes missing from 2.5.1** (was patch 0001, **released in
   2.5.2 and dropped**; taken verbatim
   from esp-usb `c04d1c7` + `23ad9ed`). An *empty* isoc packet failed payload-header
   validation and set `skip_current_frame`, poisoning the frame in progress; a low-bitrate
   stream is mostly empty packets, so every frame died. Also the frame-ID-toggle boundary
   for cameras that never set EoF.
5. **`CONFIG_UVC_CHECK_PAYLOAD_HEADER_EOH=n`** - this camera leaves EOH clear, and the
   check rejected every payload.
6. **esp_video: the stream was opened with `frame_info[0]`** (patch 0003), i.e. the
   camera's first descriptor (YUYV), while `format_select()` then asked for H.264. Open is
   what fixes the alternate setting and ISOC URB geometry; `format_select()` does not
   revisit either. Now opens with the V4L2-selected format.

### Ruled out on hardware, each with its own flash-and-measure cycle

High-bandwidth alternate setting (forced 944: no change); the H.264 extension unit
(disabled entirely: no change); the post-`STREAMON` Generate-Key-Frame request that STALLs
EP0 (removed: no change); resolution (1080p / 640x480 / 160x120); bandwidth and power
(160x120 YUYV is about 1 MB/s); and format - H.264, YUYV **and** MJPEG all produced zero
bytes.

### The measurement that matters

A temporary counter in `isoc_transfer_callback` gave the fact every theory had been
missing:

    isoc: xfer=200 pkts=800 done=800 empty=800 skip=0 err=0 bytes=0

Every isoc packet completes with `USB_TRANSFER_STATUS_COMPLETED` and zero bytes. The
endpoint is live and the host polls correctly; the camera sends nothing. Host-side frame
assembly was never involved. Re-add that counter first if this recurs.

### Where it stands

**H.264 capture streams.** Fix 3 (alt setting) - the strongest remaining
candidate when this log was first written, and untestable at the time because the camera had
stopped enumerating - is validated. With these patches applied the camera streams H.264
at the configured resolution, and `examples/kvs_combined` runs PutMedia and WebRTC off it
concurrently. The most recent measurement: five chained soft reboots, streaming every time, zero
`Failed to get frame`, against 4 failures in 6 before the shutdown handler was added. Not yet
done: soak, a second camera model, a second board revision.

Two things from the failed sessions are still worth keeping.

**A camera that stopped enumerating needs a replug, and the log says so plainly.**
`esp_video_if.c` reports the USB device list on open failure, which proves the point rather
than inferring it from a UVC-level timeout:

    esp_video_if: Failed to open camera device /dev/video40, errno: 19
    esp_video_if: USB bus: 0 device(s) enumerated

The host stack is installed and answering, and there is nothing on the bus - the camera is
not presenting itself at all. It had streamed YUYV earlier in the same session, so this is a
device state that a USB bus reset does not clear. There is no software VBUS control on this
board - the camera must be physically unplugged and replugged. `media_stream` prevents it
where it can, by stopping the stream from a shutdown handler; nothing on the host recovers a
camera that was cut off without one.

**What to check when bringing up a different camera.** Run at 640x480 first and look for
`Alternate setting 11 -> 3 to match negotiated payload 512 B`, then walk
640x480 -> 1280x720 -> 1920x1080. A frame larger than the capture buffer warns
(`frame buffer overflow ... raise sizeimage`) instead of failing silently;
`compressed_buffer_size()` caps at `CONFIG_MEDIA_STREAM_UVC_COMPRESSED_FRAME_MAX_KB`
(1 MB by default), which is ample for H.264 but is the first thing to raise if that warning
appears.
