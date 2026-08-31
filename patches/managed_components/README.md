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
not made against.

| Patch | Component | Why | Drop when |
|-------|-----------|-----|-----------|
| `0001-usb_host_uvc-2.4.2-complete-frame-on-frame-id-toggle.patch` | `espressif/usb_host_uvc` 2.4.2 | The isochronous path completes a frame only when the payload header carries the EoF flag. Cameras that never set it (observed on a Logitech H.264 stream: ~21 MB of valid payloads with zero EoF flags, on every alternate setting) hit the frame-ID toggle instead, which logged `missed EoF` and **reset the buffer**, discarding a complete frame every time. No frame ever reached the application. The patch completes the frame at the toggle, which is the only frame boundary such a camera provides. | `usb_host_uvc` releases the equivalent isoc fix. Upstream fixed the analogous **bulk** case in 2.5.1 ("bulk frame reconstruction for non-conforming cameras that can send a same-frame payload header without EOF"); the isoc path is unchanged as of 2.5.1. Related: [esp-usb#538](https://github.com/espressif/esp-usb/issues/538) reports the same `missed EoF` symptom from a different cause (a high-bandwidth alt setting the RX FIFO cannot sustain) - both can produce it, so check which applies before dropping this. |
| `0002-usb_host_uvc-h264-extension-unit-rate-control.patch` | `espressif/usb_host_uvc` 2.5.1 | No way to reach the camera's own H.264 encoder. Adds `uvc_host_stream_h264_set_rate()` / `uvc_host_stream_request_key_frame()` over the H.264 extension unit (GUID-located `UVCX_VIDEO_CONFIG_PROBE/COMMIT`), and fills `wWidth`/`wHeight` from the negotiated format so the commit is not rejected. | `usb_host_uvc` exposes encoding-unit controls of its own. |
| `0004-usb_host_uvc-match-alt-setting-to-negotiated-payload.patch` | `espressif/usb_host_uvc` 2.5.1 | Alternate setting is chosen before the probe, so it does not match `dwMaxPayloadTransferSize`; every isoc packet then returns `COMPLETED` with zero bytes. Also makes `uvc_host_usb_ctrl()` survive a transaction error: USBH clears EP0 from the host-library task, not inline, so a resubmit has to yield first - without that a single `Dev 1 EP 0 Error` after a soft reset turned every later control transfer into `ESP_ERR_INVALID_STATE` and the stream never opened. A STALL is deliberately *not* retried (it is the device's definitive answer, and USBH re-arms EP0 itself), and the `usb_host_endpoint_clear(dev_hdl, 0)` this patch used to do is gone - EP0 is not claimable, so it only ever printed two error lines. | Upstream re-selects the alt setting after the probe. |
| `0003-esp_video-apply-uvc-h264-config-before-stream-start.patch` | `espressif/esp_video` 2.4.1 | Opens the UVC stream with the V4L2-selected format instead of `frame_info[0]`, applies the H.264 extension-unit config between `format_select()` and `stream_start()`, and defines `esp_video_uvc_request_key_frame()`. It no longer asks for a key frame at stream start: a camera without that control STALLs it and USBH logs `Dev N EP 0 STALL` at ERROR on every boot for nothing. The root-port-power workaround it once carried is gone — `PRTPWR` does not gate the camera's VBUS, so it never did anything; see `components/media_stream/README.md`. | `esp_video` opens with the selected format and exposes UVC encoder controls. |

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
  in between with no message. It bit 0001/0002/0004 on `usb_host_uvc` while 0003 on
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
  deliberate: patches 0001 and 0003 add Kconfig options, and continuing would build against
  a sdkconfig that predates them.

  Still worth knowing when driving the board by hand: never trust a verification run whose
  build did not print "Project build complete", because `idf.py flash` will happily flash
  the previous binary.
- **CMake 4 and the usrsctp submodule.** `components/esp_usrsctp/usrsctp/CMakeLists.txt` asks
  for `cmake_minimum_required(VERSION 3.0)`, and CMake 4 removed compatibility below 3.5. It
  is a submodule, so `examples/kvs_combined/CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM
  3.5` instead of every build needing `-D` on the command line.

## UVC H.264 capture on ESP32-S31 (investigation log, 2026-08-21)

Goal: a USB-UVC camera that encodes H.264 itself, forwarded untouched, at 640x480 then
1280x720 then 1920x1080. **Not yet achieved** - see "Where it stands".

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
4. **usb_host_uvc: upstream isoc fixes missing from 2.5.1** (patch 0001, taken verbatim
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

Fix 3 (alt setting) is the strongest remaining candidate and is **unvalidated**: the camera
stopped enumerating before it could be tested. `esp_video_if.c` now reports the USB device
list on open failure, which proves the point rather than inferring it from a UVC-level
timeout:

    esp_video_if: Failed to open camera device /dev/video40, errno: 19
    esp_video_if: USB bus: 0 device(s) enumerated

The host stack is installed and answering, and there is nothing on the bus - the camera is
not presenting itself at all. It
had streamed YUYV earlier in the same session, so this is a device state that a USB bus
reset does not clear. There is no software VBUS control on this board - the camera must be
physically unplugged and replugged.

Next step after a replug: run at 640x480 and check for `Alternate setting 11 -> 3 to match
negotiated payload 512 B`, then walk 640x480 -> 1280x720 -> 1920x1080. A frame larger than
the capture buffer now warns (`frame buffer overflow ... raise sizeimage`) instead of
failing silently; `compressed_buffer_size()` caps at 1 MB, which is ample for H.264 but is
the first thing to raise if that warning appears.
