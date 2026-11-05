/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Format conversion for nearest memory blits. Each invocation owns an output word. Numeric conversion is integer:
   normalized-to-normalized conversion scales the original rational value directly, and half/packed float output
   rounds once to nearest-even. The sRGB transfer uses floating point only for color components. There are no CPU
   texel reads or extra per-pixel dispatches. */

uint tc_pixel_srgb_decode(uint v) {
    float encoded = uintBitsToFloat(v);
    float linear = encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4);
    return floatBitsToUint(linear);
}
uint tc_pixel_srgb_encode(uint v) {
    float linear = uintBitsToFloat(v);
    if (!(linear > 0.0)) return 0u;
    if (linear >= 1.0) return 0x3F800000u;
    float encoded = linear <= 0.0031308 ? linear * 12.92 : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
    return floatBitsToUint(encoded);
}

/* A normalized field as {unsigned magnitude, bit width}. Signed fields remain in two's complement. */
uvec2 tc_pixel_field(uint p, uint f, uint k) {
    if (k == 3u && (f & GPU_PIXEL_X) != 0u) return uvec2(1u, 1u);
    if ((f & GPU_PIXEL_BGR) != 0u && k < 3u) k = 2u - k;
    uint kind = GPU_PIXEL_TYPE(f), bits = 8u << (f & 3u), at = p + k * (bits >> 3);
    if (kind >= GPU_PIXEL_565) {
        uint v = tc_src_byte(p) | (tc_src_byte(p + 1u) << 8);
        if (kind >= GPU_PIXEL_1010102) v |= (tc_src_byte(p + 2u) << 16) | (tc_src_byte(p + 3u) << 24);
        if (kind == GPU_PIXEL_565) {
            if (k == 3u) return uvec2(1u, 1u);
            bits = k == 1u ? 6u : 5u;
            return uvec2((v >> (k == 0u ? 0u : k == 1u ? 5u : 11u)) & ((1u << bits) - 1u), bits);
        }
        bits = kind == GPU_PIXEL_5551 ? (k == 3u ? 1u : 5u) : (k == 3u ? 2u : 10u);
        return uvec2((v >> (k * (kind == GPU_PIXEL_5551 ? 5u : 10u))) & ((1u << bits) - 1u), bits);
    }
    if (k >= (((f >> 2) & 3u) + 1u)) return uvec2(k == 3u ? 1u : 0u, 1u);
    uint v = tc_src_byte(at);
    if (bits == 16u) v |= tc_src_byte(at + 1u) << 8;
    return uvec2(v, bits);
}
/* v/(2^bits-1): the significand repeats v's bits. The infinite tail makes a guard bit of one round upwards.
   This includes the all-ones field, whose carry produces exactly 1.0. */
uint tc_normal_float(uint v, uint bits) {
    if (v == 0u) return 0u;
    uint p = uint(findMSB(v)), m = 0u;
    for (int shift = 23 - int(p); shift > -int(bits); shift -= int(bits))
        m |= shift >= 0 ? v << uint(shift) : v >> uint(-shift);
    uint guard = (p + bits - (24u % bits)) % bits;
    if (((v >> guard) & 1u) != 0u) m++;
    uint e = 127u + p - bits;
    if (m == 0x1000000u) { m >>= 1; e++; }
    return (e << 23) | (m & 0x7FFFFFu);
}
uint tc_pixel_float(uint p, uint f, uint k) {
    if (k == 3u && (f & GPU_PIXEL_X) != 0u) return 0x3F800000u;
    uint kind = GPU_PIXEL_TYPE(f);
    if (kind < GPU_PIXEL_565 && k >= (((f >> 2) & 3u) + 1u)) return k == 3u ? 0x3F800000u : 0u;
    if (kind == GPU_PIXEL_FLOAT) {
        if (k >= (((f >> 2) & 3u) + 1u)) return k == 3u ? 0x3F800000u : 0u;
        uint bytes = 1u << (f & 3u), at = p + k * bytes;
        uint v = tc_src_byte(at) | (tc_src_byte(at + 1u) << 8);
        return bytes == 2u ? tc_half_float(v) : v | (tc_src_byte(at + 2u) << 16) | (tc_src_byte(at + 3u) << 24);
    }
    if (kind == GPU_PIXEL_111110) {
        if (k == 3u) return 0x3F800000u;
        uint v = tc_src_byte(p) | (tc_src_byte(p + 1u) << 8) | (tc_src_byte(p + 2u) << 16) | (tc_src_byte(p + 3u) << 24);
        return tc_half_float(((v >> (k == 0u ? 0u : k == 1u ? 11u : 22u)) & (k == 2u ? 1023u : 2047u))
                             << (k == 2u ? 5u : 4u));
    }
    uvec2 field = tc_pixel_field(p, f, k);
    if (kind != GPU_PIXEL_SNORM) {
        uint value = tc_normal_float(field.x, field.y);
        return (f & GPU_PIXEL_SRGB) != 0u && k < 3u ? tc_pixel_srgb_decode(value) : value;
    }
    uint sign = field.x >> (field.y - 1u), mask = (1u << field.y) - 1u;
    uint magnitude = sign != 0u ? (0u - field.x) & mask : field.x;
    uint maximum = mask >> 1;
    return (sign << 31) | tc_normal_float(min(magnitude, maximum), field.y - 1u);
}
/* Round a nonnegative f32 to a normalized field. A 24x16-bit product uses two words, with no shaderInt64 feature. */
uint tc_float_normal(uint v, uint maximum) {
    if ((v & 0x80000000u) != 0u || v > 0x7F800000u) return 0u;
    if (v >= 0x3F800000u) return maximum;
    uint e = v >> 23;
    if (e < 94u) return 0u;
    uint m = (v & 0x7FFFFFu) | 0x800000u, lo = (m & 65535u) * maximum;
    uint hi = (m >> 16) * maximum + (lo >> 16);
    lo = (lo & 65535u) | (hi << 16);
    hi >>= 16;
    uint shift = 150u - e, q, rem, tie;
    bool sticky = false;
    if (shift < 32u) {
        q = (lo >> shift) | (hi << (32u - shift));
        rem = lo & ((1u << shift) - 1u);
        tie = 1u << (shift - 1u);
    } else if (shift == 32u) {
        q = hi; rem = lo; tie = 0x80000000u;
    } else {
        uint s = shift - 32u;
        q = hi >> s; rem = hi & ((1u << s) - 1u); tie = 1u << (s - 1u); sticky = lo != 0u;
    }
    return q + uint(rem > tie || (rem == tie && (sticky || (q & 1u) != 0u)));
}
uint tc_pixel_normal(uint p, uint source, uint k, uint bits, bool signed_out) {
    uint kind = GPU_PIXEL_TYPE(source), maximum = (1u << (signed_out ? bits - 1u : bits)) - 1u;
    uint magnitude, sign = 0u;
    if (kind != GPU_PIXEL_FLOAT && kind != GPU_PIXEL_111110 && !((source & GPU_PIXEL_SRGB) != 0u && k < 3u)) {
        uvec2 field = tc_pixel_field(p, source, k);
        uint in_max = (1u << field.y) - 1u;
        magnitude = field.x;
        if (kind == GPU_PIXEL_SNORM && field.y > 1u) {
            sign = magnitude >> (field.y - 1u);
            if (sign != 0u) magnitude = (0u - magnitude) & in_max;
            in_max >>= 1;
            magnitude = min(magnitude, in_max);
        }
        magnitude = (magnitude * maximum + in_max / 2u) / in_max;
    } else {
        uint v = tc_pixel_float(p, source, k);
        sign = v >> 31;
        magnitude = tc_float_normal(v & 0x7FFFFFFFu, maximum);
    }
    if (sign != 0u) return signed_out ? (0u - magnitude) & ((1u << bits) - 1u) : 0u;
    return magnitude;
}
/* Unsigned 5-exponent-bit floats, with five or six mantissa bits. Round from f32 once, including subnormals. */
uint tc_float_unsigned(uint v, uint mantissa) {
    uint e = (v >> 23) & 255u, m = v & 0x7FFFFFu, h, rem, tie;
    if (e == 255u && m != 0u) return (31u << mantissa) | (1u << (mantissa - 1u));
    if ((v & 0x80000000u) != 0u) return 0u;
    if (e >= 143u) return 31u << mantissa;
    uint shift = 23u - mantissa;
    if (e <= 112u) {
        if (e < 112u - mantissa) return 0u;
        shift = 136u - e - mantissa;
        m |= 0x800000u;
        h = m >> shift;
    } else h = ((e - 112u) << mantissa) | (m >> shift);
    rem = m & ((1u << shift) - 1u); tie = 1u << (shift - 1u);
    return h + uint(rem > tie || (rem == tie && (h & 1u) != 0u));
}
uint tc_pixel_byte(uint p, uint source, uint destination, uint byte_index) {
    uint kind = GPU_PIXEL_TYPE(destination);
    if (kind >= GPU_PIXEL_565) {
        uint v = 0u;
        for (uint channel = 0u; channel < 4u; channel++) {
            uint k = (destination & GPU_PIXEL_BGR) != 0u && channel < 3u ? 2u - channel : channel;
            if (kind == GPU_PIXEL_111110) {
                if (channel < 3u) v |= tc_float_unsigned(tc_pixel_float(p, source, k), channel == 2u ? 5u : 6u)
                                      << (channel == 0u ? 0u : channel == 1u ? 11u : 22u);
            } else {
                if (kind == GPU_PIXEL_565 && channel == 3u) continue;
                uint bits = kind == GPU_PIXEL_565 ? (channel == 1u ? 6u : 5u)
                            : kind == GPU_PIXEL_5551 ? (channel == 3u ? 1u : 5u) : (channel == 3u ? 2u : 10u);
                uint value = channel == 3u && (destination & GPU_PIXEL_X) != 0u ? (1u << bits) - 1u
                             : tc_pixel_normal(p, source, k, bits, false);
                uint shift = kind == GPU_PIXEL_565 ? (channel == 0u ? 0u : channel == 1u ? 5u : 11u)
                             : channel * (kind == GPU_PIXEL_5551 ? 5u : 10u);
                v |= value << shift;
            }
        }
        return (v >> (byte_index * 8u)) & 255u;
    }
    uint bytes = 1u << (destination & 3u), k = byte_index / bytes;
    if ((destination & GPU_PIXEL_BGR) != 0u && k < 3u) k = 2u - k;
    uint v;
    if (kind == GPU_PIXEL_FLOAT) {
        v = (destination & GPU_PIXEL_X) != 0u && k == 3u ? 0x3F800000u : tc_pixel_float(p, source, k);
        if (bytes == 2u) v = tc_float_half(v);
    } else if ((destination & GPU_PIXEL_SRGB) != 0u && k < 3u) {
        v = tc_float_normal(tc_pixel_srgb_encode(tc_pixel_float(p, source, k)), 255u);
    } else v = (destination & GPU_PIXEL_X) != 0u && k == 3u ? (1u << (bytes * 8u - uint(kind == GPU_PIXEL_SNORM))) - 1u
               : tc_pixel_normal(p, source, k, bytes * 8u, kind == GPU_PIXEL_SNORM);
    return (v >> ((byte_index % bytes) * 8u)) & 255u;
}
/* The table's coordinates already use texel centers and clamp to logical image bounds. Do not fetch a neighbor
   whose weight is zero: besides saving fetches this keeps an out-of-range/NaN padding texel out of the result. */
uint tc_pixel_linear_component(uint pixel, uint line, uint source, uint channel) {
    float x = uintBitsToFloat(src[const_a + pixel]), y = uintBitsToFloat(src[const_b + line]);
    uint x0 = uint(x), y0 = uint(y), bytes = GPU_PIXEL_BYTES(source);
    float fx = x - float(x0), fy = y - float(y0);
    uint at = in_bias + y0 * in_pitch + x0 * bytes;
    float top = uintBitsToFloat(tc_pixel_float(at, source, channel));
    if (fx != 0.0) top += (uintBitsToFloat(tc_pixel_float(at + bytes, source, channel)) - top) * fx;
    if (fy != 0.0) {
        float bottom = uintBitsToFloat(tc_pixel_float(at + in_pitch, source, channel));
        if (fx != 0.0)
            bottom += (uintBitsToFloat(tc_pixel_float(at + in_pitch + bytes, source, channel)) - bottom) * fx;
        top += (bottom - top) * fy;
    }
    return floatBitsToUint(top);
}
uint tc_pixel_linear_byte(uint pixel, uint line, uint source, uint destination, uint byte_index) {
    uint kind = GPU_PIXEL_TYPE(destination);
    if (kind >= GPU_PIXEL_565) {
        uint packed = 0u;
        for (uint channel = 0u; channel < 4u; channel++) {
            if ((kind == GPU_PIXEL_565 || kind == GPU_PIXEL_111110) && channel == 3u) continue;
            uint k = (destination & GPU_PIXEL_BGR) != 0u && channel < 3u ? 2u - channel : channel;
            uint value = channel == 3u && (destination & GPU_PIXEL_X) != 0u ? 0x3F800000u
                         : tc_pixel_linear_component(pixel, line, source, k);
            if (kind == GPU_PIXEL_111110) {
                packed |= tc_float_unsigned(value, channel == 2u ? 5u : 6u)
                          << (channel == 0u ? 0u : channel == 1u ? 11u : 22u);
            } else {
                uint bits = kind == GPU_PIXEL_565 ? (channel == 1u ? 6u : 5u)
                            : kind == GPU_PIXEL_5551 ? (channel == 3u ? 1u : 5u) : (channel == 3u ? 2u : 10u);
                uint shift = kind == GPU_PIXEL_565 ? (channel == 0u ? 0u : channel == 1u ? 5u : 11u)
                             : channel * (kind == GPU_PIXEL_5551 ? 5u : 10u);
                packed |= tc_float_normal(value, (1u << bits) - 1u) << shift;
            }
        }
        return (packed >> (byte_index * 8u)) & 255u;
    }
    uint bytes = 1u << (destination & 3u), k = byte_index / bytes;
    if ((destination & GPU_PIXEL_BGR) != 0u && k < 3u) k = 2u - k;
    uint value = k == 3u && (destination & GPU_PIXEL_X) != 0u ? 0x3F800000u
                 : tc_pixel_linear_component(pixel, line, source, k);
    if (kind == GPU_PIXEL_FLOAT) {
        if (bytes == 2u) value = tc_float_half(value);
    } else {
        if ((destination & GPU_PIXEL_SRGB) != 0u && k < 3u) value = tc_pixel_srgb_encode(value);
        uint bits = bytes * 8u, magnitude = tc_float_normal(value & 0x7FFFFFFFu,
                                                           (1u << (bits - uint(kind == GPU_PIXEL_SNORM))) - 1u);
        value = (value & 0x80000000u) != 0u ? (kind == GPU_PIXEL_SNORM ? (0u - magnitude) & ((1u << bits) - 1u) : 0u)
                                           : magnitude;
    }
    return (value >> ((byte_index % bytes) * 8u)) & 255u;
}
