/* Console lanes of cflts/cfltu at I8 0..16, one row per V(op, I8, input word, console result, kind).
 * Source: console probe, CECHC04 on FW 4.81, a sweep of both ops over the same input block at
 * every I8 in 0..16 (129024 lanes dumped; this is every fourth row of a hand-picked 320-row
 * selection plus all eight of its exponent-0 rows at sh 7 and 8; the selection covers each
 * input class: zero, denormal, normal, NaN exponent, and the exponents
 * around each threshold: sh = be + 23 - I8 at 7/8/9 (the saturation thresholds), 263/264/265 (the
 * end of the modelled region) and the I8 15/16 controls). Every lane is the instruction's own
 * result.
 *
 * kind M: the model covers the lane, so spu_cflts/spu_cfltu must return `console` and
 *         spu_cfl_unmodelled_lane() must not stop on it.
 * kind U: the model does NOT cover the lane (sh >= 264 where neither the cfltu negative rule nor
 *         the cfltu sh == 264 rule applies): `console` is kept as the evidence, and the model
 *         must stop on the lane instead of producing a value. The expectation is the stop, never
 *         `console`.
 * Inputs are raw words chosen by the probe; none comes from a title or from firmware. */
    V(cflts,  15, 0x00000000u, 0x7FFFFFFFu, M) /* zero be=0 sh=8 */
    V(cflts,  16, 0x00000000u, 0x00000000u, M) /* zero be=0 sh=7 */
    V(cflts,  15, 0x80652112u, 0x80000000u, M) /* denormal be=0 sh=8 */
    V(cflts,  15, 0x803EA729u, 0x80000000u, M) /* denormal be=0 sh=8 */
    V(cflts,  14, 0x00000000u, 0x7FFFFFFFu, M) /* zero be=0 sh=9 */
    V(cflts,  14, 0x80652112u, 0x80000000u, M) /* denormal be=0 sh=9 */
    V(cflts,  14, 0x42DB9056u, 0x7FFFFFFFu, M) /* normal be=133 sh=142 */
    V(cflts,  14, 0xFFABDE3Du, 0x3CC8732Au, U) /* NaN be=255 sh=264 */
    V(cflts,   0, 0xF84DEADEu, 0x80000000u, M) /* normal be=240 sh=263 */
    V(cflts,   0, 0x797A16E5u, 0xE5120000u, U) /* normal be=242 sh=265 */
    V(cflts,   0, 0xFFABDE3Du, 0x3CCA8FFFu, U) /* NaN be=255 sh=278 */
    V(cflts,   1, 0x797A16E5u, 0xE5490000u, U) /* normal be=242 sh=264 */
    V(cflts,   1, 0xFF66344Du, 0x4CBA4000u, U) /* normal be=254 sh=276 */
    V(cflts,   2, 0x797A16E5u, 0x7FFFFFFFu, M) /* normal be=242 sh=263 */
    V(cflts,   2, 0x7A39D119u, 0x19000000u, U) /* normal be=244 sh=265 */
    V(cflts,   2, 0xFFABDE3Du, 0x3CB2A3FFu, U) /* NaN be=255 sh=276 */
    V(cflts,   3, 0x7A39D119u, 0x19000000u, U) /* normal be=244 sh=264 */
    V(cflts,   3, 0xFF66344Du, 0x4C8E9000u, U) /* normal be=254 sh=274 */
    V(cflts,   4, 0x7A39D119u, 0x7FFFFFFFu, M) /* normal be=244 sh=263 */
    V(cflts,   4, 0x7B688EF3u, 0xF3230000u, U) /* normal be=246 sh=265 */
    V(cflts,   4, 0xFFABDE3Du, 0x3CCCA8FFu, U) /* NaN be=255 sh=274 */
    V(cflts,   5, 0x7B688EF3u, 0xF3118000u, U) /* normal be=246 sh=264 */
    V(cflts,   5, 0xFF66344Du, 0x4CE3A400u, U) /* normal be=254 sh=272 */
    V(cflts,   6, 0x7B688EF3u, 0x7FFFFFFFu, M) /* normal be=246 sh=263 */
    V(cflts,   6, 0xFC272E10u, 0x0FB5727Fu, U) /* normal be=248 sh=265 */
    V(cflts,   6, 0xFFABDE3Du, 0x3CF32A3Fu, U) /* NaN be=255 sh=272 */
    V(cflts,   7, 0xFC272E10u, 0x0FDAB93Fu, U) /* normal be=248 sh=264 */
    V(cflts,   7, 0xFF66344Du, 0x4CF8E900u, U) /* normal be=254 sh=270 */
    V(cflts,   8, 0xFC272E10u, 0x80000000u, M) /* normal be=248 sh=263 */
    V(cflts,   8, 0xFD7EF9FDu, 0xFCFFDFFFu, U) /* normal be=250 sh=265 */
    V(cflts,   8, 0xFFABDE3Du, 0x3C9CCA8Fu, U) /* NaN be=255 sh=270 */
    V(cflts,   9, 0xFD7EF9FDu, 0xFCFFEFFFu, U) /* normal be=250 sh=264 */
    V(cflts,   9, 0xFF66344Du, 0x4CFE3A40u, U) /* normal be=254 sh=268 */
    V(cflts,  10, 0xFD7EF9FDu, 0x80000000u, M) /* normal be=250 sh=263 */
    V(cflts,  10, 0xFE01D5A6u, 0xA5CE07BFu, U) /* normal be=252 sh=265 */
    V(cflts,  10, 0xFFABDE3Du, 0x3C8732A3u, U) /* NaN be=255 sh=268 */
    V(cflts,  11, 0xFE01D5A6u, 0xA5E703DFu, U) /* normal be=252 sh=264 */
    V(cflts,  11, 0xFF66344Du, 0x4CDF8E90u, U) /* normal be=254 sh=266 */
    V(cflts,  12, 0xFE01D5A6u, 0x80000000u, M) /* normal be=252 sh=263 */
    V(cflts,  12, 0xFF66344Du, 0x4CAFC748u, U) /* normal be=254 sh=265 */
    V(cflts,  13, 0xFECC0F3Cu, 0x80000000u, M) /* normal be=253 sh=263 */
    V(cflts,  13, 0xFFABDE3Du, 0x3C90E654u, U) /* NaN be=255 sh=265 */
    V(cflts,  15, 0xFFABDE3Du, 0x80000000u, M) /* NaN be=255 sh=263 */
    V(cflts,  16, 0xFF66344Du, 0x80000000u, M) /* normal be=254 sh=261 */
    V(cfltu,  15, 0x00000000u, 0x00000000u, M) /* zero be=0 sh=8 */
    V(cfltu,  16, 0x00000000u, 0x00000000u, M) /* zero be=0 sh=7 */
    V(cfltu,  15, 0x80652112u, 0x00000000u, M) /* denormal be=0 sh=8 */
    V(cfltu,  15, 0x803EA729u, 0x00000000u, M) /* denormal be=0 sh=8 */
    V(cfltu,  14, 0x00000000u, 0xFFFFFFFFu, M) /* zero be=0 sh=9 */
    V(cfltu,  14, 0x80652112u, 0x00000000u, M) /* denormal be=0 sh=9 */
    V(cfltu,  14, 0x42DB9056u, 0xFFFFFFFFu, M) /* normal be=133 sh=142 */
    V(cfltu,  14, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=264 */
    V(cfltu,   0, 0xF84DEADEu, 0x00000000u, M) /* normal be=240 sh=263 */
    V(cfltu,   0, 0x797A16E5u, 0xE5120000u, U) /* normal be=242 sh=265 */
    V(cfltu,   0, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=278 */
    V(cfltu,   1, 0x797A16E5u, 0xFFFFFFFFu, M) /* normal be=242 sh=264 */
    V(cfltu,   1, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=276 */
    V(cfltu,   2, 0x797A16E5u, 0xFFFFFFFFu, M) /* normal be=242 sh=263 */
    V(cfltu,   2, 0x7A39D119u, 0x19000000u, U) /* normal be=244 sh=265 */
    V(cfltu,   2, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=276 */
    V(cfltu,   3, 0x7A39D119u, 0xFFFFFFFFu, M) /* normal be=244 sh=264 */
    V(cfltu,   3, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=274 */
    V(cfltu,   4, 0x7A39D119u, 0xFFFFFFFFu, M) /* normal be=244 sh=263 */
    V(cfltu,   4, 0x7B688EF3u, 0xF3230000u, U) /* normal be=246 sh=265 */
    V(cfltu,   4, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=274 */
    V(cfltu,   5, 0x7B688EF3u, 0xFFFFFFFFu, M) /* normal be=246 sh=264 */
    V(cfltu,   5, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=272 */
    V(cfltu,   6, 0x7B688EF3u, 0xFFFFFFFFu, M) /* normal be=246 sh=263 */
    V(cfltu,   6, 0xFC272E10u, 0x00000000u, M) /* normal be=248 sh=265 */
    V(cfltu,   6, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=272 */
    V(cfltu,   7, 0xFC272E10u, 0x00000000u, M) /* normal be=248 sh=264 */
    V(cfltu,   7, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=270 */
    V(cfltu,   8, 0xFC272E10u, 0x00000000u, M) /* normal be=248 sh=263 */
    V(cfltu,   8, 0xFD7EF9FDu, 0x00000000u, M) /* normal be=250 sh=265 */
    V(cfltu,   8, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=270 */
    V(cfltu,   9, 0xFD7EF9FDu, 0x00000000u, M) /* normal be=250 sh=264 */
    V(cfltu,   9, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=268 */
    V(cfltu,  10, 0xFD7EF9FDu, 0x00000000u, M) /* normal be=250 sh=263 */
    V(cfltu,  10, 0xFE01D5A6u, 0x00000000u, M) /* normal be=252 sh=265 */
    V(cfltu,  10, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=268 */
    V(cfltu,  11, 0xFE01D5A6u, 0x00000000u, M) /* normal be=252 sh=264 */
    V(cfltu,  11, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=266 */
    V(cfltu,  12, 0xFE01D5A6u, 0x00000000u, M) /* normal be=252 sh=263 */
    V(cfltu,  12, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=265 */
    V(cfltu,  13, 0xFECC0F3Cu, 0x00000000u, M) /* normal be=253 sh=263 */
    V(cfltu,  13, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=265 */
    V(cfltu,  15, 0xFFABDE3Du, 0x00000000u, M) /* NaN be=255 sh=263 */
    V(cfltu,  16, 0xFF66344Du, 0x00000000u, M) /* normal be=254 sh=261 */
