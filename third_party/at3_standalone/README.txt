This is the atrac3/atrac3+ decoders from ffmpeg, extracted to be standalone from ffmpeg.

ps3recomp: vendored from PPSSPP ext/at3_standalone at commit
ebc151f8f28beca7c07c9ec5d183fc6171c4a1fc, unmodified except that aac_defines.h
(unused) and PPSSPP's CMakeLists.txt were dropped. ppsspp_config.h and
Common/*.h are small shims standing in for PPSSPP headers; at3_bridge.* is a
C interface for libs/codec/cellAtrac.c.

The decoder sources are LGPL-2.1-or-later (see COPYING.LGPLv2.1 and the
headers of each file). They are kept in this directory as a separable
component; the rest of ps3recomp is MIT.
