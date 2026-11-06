// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Integer shader conversions against independent host rational and floating-point references.
static uint32_t pixel_float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}
static void pixel_conversions() {
    Conv c = {};
    for (uint32_t bits = 1; bits <= 16; bits++) {
        uint32_t maximum = (1u << bits) - 1;
        for (uint32_t value = 0; value <= maximum; value++)
            CHECK(c.tc_normal_float(value, bits) == pixel_float_bits(float(value) / float(maximum)),
                  "normalized field %u/%u", value, maximum);
    }
    const uint32_t formats[] = {
        GPU_PIXEL_DESC(0u, 4u, GPU_PIXEL_UNORM),
        GPU_PIXEL_DESC(1u, 4u, GPU_PIXEL_UNORM),
        GPU_PIXEL_DESC(1u, 4u, GPU_PIXEL_5551),
        GPU_PIXEL_DESC(1u, 3u, GPU_PIXEL_565),
        GPU_PIXEL_DESC(2u, 4u, GPU_PIXEL_1010102),
    };
    std::vector<glsl::leuint> source(4);
    c.src = source.data();
    for (uint32_t value = 0; value < 65536; value++) {
        source[0] = value;
        uint32_t src_format = GPU_PIXEL_DESC(1u, 4u, GPU_PIXEL_5551);
        for (uint32_t destination : formats) {
            uint8_t expected[8] = {};
            uint32_t word = 0, kind = GPU_PIXEL_TYPE(destination);
            for (uint32_t channel = 0; channel < 4; channel++) {
                uint32_t in_max = channel == 3 ? 1 : 31;
                uint32_t field = (value >> (channel * 5)) & in_max;
                uint32_t out_bits = kind == GPU_PIXEL_565 ? (channel == 1 ? 6 : 5)
                                    : kind == GPU_PIXEL_5551 ? (channel == 3 ? 1 : 5)
                                    : kind == GPU_PIXEL_1010102 ? (channel == 3 ? 2 : 10)
                                    : 8u << (destination & 3);
                if (kind == GPU_PIXEL_565 && channel == 3) continue;
                uint32_t out_max = (1u << out_bits) - 1;
                uint32_t result = (field * out_max + in_max / 2) / in_max;
                if (kind >= GPU_PIXEL_565) {
                    uint32_t shift = kind == GPU_PIXEL_565 ? (channel == 0 ? 0 : channel == 1 ? 5 : 11)
                                     : channel * (kind == GPU_PIXEL_5551 ? 5 : 10);
                    word |= result << shift;
                } else {
                    uint32_t bytes = out_bits / 8;
                    for (uint32_t byte = 0; byte < bytes; byte++) expected[channel * bytes + byte] = result >> (byte * 8);
                }
            }
            if (kind >= GPU_PIXEL_565)
                for (uint32_t byte = 0; byte < GPU_PIXEL_BYTES(destination); byte++) expected[byte] = word >> (byte * 8);
            for (uint32_t byte = 0; byte < GPU_PIXEL_BYTES(destination); byte++)
                CHECK(c.tc_pixel_byte(0, src_format, destination, byte) == expected[byte],
                      "packed conversion value=%u format=%x byte=%u", value, destination, byte);
        }
    }
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t v = random_word();
        float f;
        std::memcpy(&f, &v, sizeof f);
        for (uint32_t maximum : {1u, 31u, 63u, 255u, 1023u, 65535u}) {
            double scaled = double(f) * maximum;
            uint32_t expected;
            if (!(scaled > 0)) expected = 0;
            else if (scaled >= maximum) expected = maximum;
            else {
                uint32_t low = uint32_t(scaled);
                double fraction = scaled - low;
                expected = low + uint32_t(fraction > 0.5 || (fraction == 0.5 && (low & 1)));
            }
            CHECK(c.tc_float_normal(v, maximum) == expected, "float-normal %08x max=%u", v, maximum);
        }
    }
    for (uint32_t value = 0; value < 256; value++) {
        source[0] = value;
        uint32_t source_format = GPU_PIXEL_DESC(0u, 4u, GPU_PIXEL_UNORM) | GPU_PIXEL_SRGB;
        uint32_t destination_format = GPU_PIXEL_DESC(2u, 4u, GPU_PIXEL_FLOAT);
        uint32_t linear_bits = 0;
        for (uint32_t byte = 0; byte < 4; byte++)
            linear_bits |= c.tc_pixel_byte(0, source_format, destination_format, byte) << (byte * 8);
        float linear;
        std::memcpy(&linear, &linear_bits, sizeof linear);
        double encoded = double(value) / 255.0;
        double expected = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        CHECK(std::fabs(double(linear) - expected) < 1e-6, "sRGB decode %u", value);
        source[0] = linear_bits;
        uint32_t roundtrip = c.tc_pixel_byte(0, destination_format, source_format, 0);
        CHECK(roundtrip == value, "sRGB encode roundtrip %u -> %u", value, roundtrip);
    }
}
