# Auto-apply the local patches to managed (registry) components, the same way
# kvs_webrtc applies the webrtc-c submodule patches, and under the same
# CONFIG_KVS_AUTO_APPLY_PATCHES switch.
#
# This lives in media_stream because media_stream is what consumes the patched
# components (esp_video, usb_host_uvc). It has to run from a component CMakeLists
# rather than earlier: managed_components/ does not exist until the component manager
# has run, which is part of the same configure.
#
# The catch is that two of those patches add Kconfig options, and Kconfig has already
# been read by the time any component CMakeLists runs. So a run that actually applies
# something has produced a source tree this configure cannot describe - it exits 10 and
# we stop, rather than let the build proceed against a sdkconfig that predates the
# options the patches just added. The next configure sees everything already applied
# and says nothing.
#
# That is not a rare path. The component manager restores a managed component from its
# checksum whenever it thinks the on-disk copy has drifted, which quietly reverts these
# patches mid-configure; without this hook the next build is against unpatched sources
# and the only symptom is a missing CONFIG_ symbol or, worse, silently different
# behaviour.
if(CONFIG_KVS_AUTO_APPLY_PATCHES)
    set(_mc_apply_script
        "${CMAKE_CURRENT_SOURCE_DIR}/../../patches/managed_components/apply_managed_component_patches.sh")
    if(EXISTS "${_mc_apply_script}")
        execute_process(
            COMMAND bash "${_mc_apply_script}" "${CMAKE_SOURCE_DIR}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
            RESULT_VARIABLE _mc_patch_result
            OUTPUT_VARIABLE _mc_patch_output
            ERROR_VARIABLE _mc_patch_error)
        if(_mc_patch_result EQUAL 10)
            message(FATAL_ERROR
                "Managed-component patches were just applied:\n${_mc_patch_output}\n"
                "Two of them add Kconfig options, and Kconfig was read before this point, "
                "so this configure cannot see them. Re-run the build - it will pick them "
                "up and this message will not come back.")
        elseif(NOT _mc_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply managed-component patches.\n"
                "stdout:\n${_mc_patch_output}\n"
                "stderr:\n${_mc_patch_error}\n"
                "Rebase the offending patch (they are applied at --fuzz=0 on purpose), or "
                "set CONFIG_KVS_AUTO_APPLY_PATCHES=n if you patch by hand.")
        endif()
    endif()
endif()

if (ENABLE_SIGNALLING_ONLY)
    set(srcs )
else()
    # Every file is listed unconditionally. Sources that depend on
    # P4/S3-only headers (esp_video / esp_h264 / esp_image_effects)
    # self-gate at the top of their own translation unit, so on
    # unsupported targets they compile to an empty object with no
    # link-visible symbols.
    set(srcs
        "src/esp_video_if.c"
        "src/esp_h264_hw_enc.c"
        "src/app_camera_esp.c"
        "src/esp32p4_frame_grabber.c"
        "src/video_rate_ctrl.c"
        "src/H264FrameGrabber.c"
        "src/MJPEGFrameGrabber.c"
        "src/OpusFrameGrabber.c"
        "src/OpusAudioPlayer.c"
        "src/resampling.c"
        "src/ringbuf.c"
        "src/video_sink.c"
        "src/video_capture_adapter.c"
        "src/audio_capture_adapter.c"
        "src/audio_player_adapter.c"
        "src/video_player_adapter.c"
        "src/h264_nalu_utils.c"
        "src/video_render_display.c"
        "src/media_stream.c"
    )
endif()

set(priv_reqs "")
if(CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER)
    list(APPEND priv_reqs espressif__esp_h264)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_jpeg
    PRIV_REQUIRES ${priv_reqs}
    PRIV_INCLUDE_DIRS "src"
)
