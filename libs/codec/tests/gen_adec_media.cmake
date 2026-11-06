# Generates the AC3 streams test_adec_ac3 feeds through cellAdec, and FFmpeg's
# own decode of the 5.1 one as the reference. Nothing here is committed:
# everything is written to the build tree at test time.
#
#   cmake -DFFMPEG=<ffmpeg> -DOUT=<dir> -P gen_adec_media.cmake
#
# 448 kb/s at 48 kHz makes every frame 1792 bytes. A different tone on each
# channel, so a channel swap shows.
if(NOT FFMPEG OR NOT OUT)
    message(FATAL_ERROR "pass -DFFMPEG=<ffmpeg> -DOUT=<dir>")
endif()
file(MAKE_DIRECTORY "${OUT}")

function(run)
    execute_process(COMMAND "${FFMPEG}" -nostdin -hide_banner -loglevel error -y ${ARGN}
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "ffmpeg failed (${rc}): ${ARGN}")
    endif()
endfunction()

run(-f lavfi -i "aevalsrc=0.5*sin(2*PI*330*t)|0.4*sin(2*PI*440*t)|0.3*sin(2*PI*550*t)|0.2*sin(2*PI*60*t)|0.3*sin(2*PI*660*t)|0.25*sin(2*PI*770*t):s=48000:c=5.1:d=0.4"
    -c:a ac3 -b:a 448k -f ac3 "${OUT}/t51.ac3")
run(-f lavfi -i "aevalsrc=0.5*sin(2*PI*330*t)|0.4*sin(2*PI*440*t):s=48000:c=stereo:d=0.2"
    -c:a ac3 -b:a 448k -f ac3 "${OUT}/t20.ac3")
run(-i "${OUT}/t51.ac3" -f f32le "${OUT}/t51.f32")
