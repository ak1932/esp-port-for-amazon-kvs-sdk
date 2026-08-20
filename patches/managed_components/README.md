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
