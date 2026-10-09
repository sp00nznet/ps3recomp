# Generates the streams test_vdec_decode feeds through cellVdec, and FFmpeg's
# own decode of each as the reference. Nothing here is committed: everything
# is written to the build tree at test time.
#
#   cmake -DFFMPEG=<ffmpeg> -DOUT=<dir> -P gen_vdec_media.cmake
#
# 30 frames of testsrc2 at 192x108: not a multiple of 16 high, so the
# decoders crop, and two B-frames between references, so pictures leave the
# decoder in a different order from the AUs that went in.
if(NOT FFMPEG OR NOT OUT)
    message(FATAL_ERROR "pass -DFFMPEG=<ffmpeg> -DOUT=<dir>")
endif()
file(MAKE_DIRECTORY "${OUT}")

set(SRC -f lavfi -i testsrc2=size=192x108:rate=25 -frames:v 30 -pix_fmt yuv420p)

function(run)
    execute_process(COMMAND "${FFMPEG}" -nostdin -hide_banner -loglevel error -y ${ARGN}
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "ffmpeg failed (${rc}): ${ARGN}")
    endif()
endfunction()

# H.264 Annex B with an access unit delimiter in front of every AU, which is
# where the test splits it.
run(${SRC} -c:v libx264 -preset fast -bf 2 -g 12
    -bsf:v h264_metadata=aud=insert -f h264 "${OUT}/v.h264")
run(${SRC} -c:v mpeg2video -bf 2 -g 12 -f mpeg2video "${OUT}/v.m2v")

foreach(s h264 m2v)
    run(-i "${OUT}/v.${s}" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p
        "${OUT}/ref_${s}.yuv")
    run(-i "${OUT}/v.${s}" -fps_mode passthrough -f rawvideo -pix_fmt rgba
        "${OUT}/ref_${s}.rgba")
endforeach()
