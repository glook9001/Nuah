/* SIMD bulk-copy primitive for Nuah's offline derived-texture pack builder.
 *
 * This is intentionally not a render-thread shader or a replacement for
 * Roblox's ETC2 decoder. It is called only while materialising the derived
 * cache, where KTX2 retained mip payloads are copied into their compact
 * stream. ISPC emits AVX2 on the i3-8145U and SSE4 as a portable fallback.
 */
export void nuah_ispc_copy(uniform uint8 *uniform destination,
                           uniform const uint8 *uniform source,
                           uniform int length) {
    foreach (index = 0 ... length) {
        destination[index] = source[index];
    }
}

/* Classify fixed-width RBXH/KTX2 prefixes in parallel before the host parses
 * KTX level metadata. Each program instance owns one cache record, making
 * this a genuine SPMD batch operation rather than a scalar helper wrapped in
 * ISPC. `records` is a densely packed array of 49-byte prefixes. */
export void nuah_ispc_mark_rbxh_ktx2(uniform const uint8 *uniform records,
                                     uniform int count,
                                     uniform int stride,
                                     uniform uint8 *uniform matches) {
    foreach (record = 0 ... count) {
        varying bool valid = records[record * stride + 0] == 82 &&
                             records[record * stride + 1] == 66 &&
                             records[record * stride + 2] == 88 &&
                             records[record * stride + 3] == 72;
        uniform int magic[12] = {171, 75, 84, 88, 32, 50, 48, 187, 13, 10, 26, 10};
        for (uniform int byte = 0; byte < 12; ++byte) {
            valid &= records[record * stride + 37 + byte] == magic[byte];
        }
        matches[record] = valid ? 1 : 0;
    }
}

/* Two independent position-sensitive 64-bit fingerprints for mapped Vulkan
 * upload memory.  Each gang hashes a vector of bytes at once and reduces it
 * locally; this is used only by Nuah's opt-in duplicate-upload experiment.
 * It never changes payload bytes or replaces Vulkan validation. */
export void nuah_ispc_upload_fingerprint(uniform const uint8 *uniform bytes,
                                         uniform int length,
                                         uniform uint64 *uniform result) {
    /* AVX2 has native eight-lane 32-bit multiply but not 64-bit multiply.
     * Keep two independent 32-bit lanes and return their 64-bit pair: this
     * preserves a 64-bit collision space without scalarizing the hot loop. */
    uniform uint32 first = 0x9e3779b1;
    uniform uint32 second = 0x85ebca77;
    foreach (index = 0 ... length) {
        varying uint32 position = (uint32)(index + 1);
        varying uint32 value = (uint32)bytes[index];
        varying uint32 left = value + 0x9e3779b1 * position;
        varying uint32 right = value + 0x85ebca77 * position;
        left ^= left >> 15;
        right ^= right >> 13;
        first += reduce_add(left * 0x27d4eb2d);
        second += reduce_add(right * 0x165667b1);
    }
    result[0] = (uint64)first;
    result[1] = (uint64)second;
}
