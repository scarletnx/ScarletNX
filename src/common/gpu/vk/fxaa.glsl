// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Fast approximate anti-aliasing of the picture a window shows (the anti_aliasing setting), the same for the Switch's
// presenter (nx/gpu/vk/present_quad.frag) and the 3DS's (3ds/gpu/vk/shaders/fxaa.comp). Where the luma at a point and
// at its four diagonal neighbours a pixel away spans enough to be an edge, the colour there is blurred along the edge:
// two pairs of taps along the edge's direction, the wider pair kept while its luma stays inside the neighbourhood's
// range. Every tap is one bilinear fetch of image, which takes a linear sampler that clamps to the edge. A point is in
// the image's pixels, a pixel's centre half a pixel in from its corner, so a point between centres (a picture scaled
// as it is shown) is filtered as the picture is there. bounds is the part of the image the window shows, from its first
// pixel's centre to its last's (x0, y0, x1, y1): every tap is held inside it, so a crop of the image (the Switch's
// dynamic resolution) blends in no pixel the game does not show.
const float FXAA_EDGE_MIN = 1.0 / 24.0;      // an edge is a luma range of at least this
const float FXAA_EDGE_RELATIVE = 0.125;      // and of this part of the brightest tap
const float FXAA_REDUCE_MIN = 1.0 / 128.0;   // the direction's smallest divisor
const float FXAA_REDUCE_RELATIVE = 1.0 / 8.0;
const float FXAA_SPAN_MAX = 8.0;             // the search's reach, in pixels

// luma of a colour as the display shows it: linear holds light (an sRGB image sampled through its sRGB view), which
// the square root brings close to the display's encoding, so that an edge's contrast is judged as it is seen
float fxaa_luma(vec3 c, bool linear) {
    return dot(linear ? sqrt(max(c, vec3(0.0))) : c, vec3(0.299, 0.587, 0.114));
}
vec4 fxaa_tap(sampler2D image, vec2 point, vec4 bounds, vec2 texel) {
    return textureLod(image, clamp(point, bounds.xy, bounds.zw) * texel, 0.0);
}
// the picture at point, its jagged edges smoothed; alpha is the picture's own there
vec4 fxaa(sampler2D image, vec2 point, vec4 bounds, bool linear) {
    vec2 texel = 1.0 / vec2(textureSize(image, 0));
    vec4 middle = fxaa_tap(image, point, bounds, texel);
    float lm = fxaa_luma(middle.rgb, linear);
    float nw = fxaa_luma(fxaa_tap(image, point + vec2(-1.0, -1.0), bounds, texel).rgb, linear);
    float ne = fxaa_luma(fxaa_tap(image, point + vec2(1.0, -1.0), bounds, texel).rgb, linear);
    float sw = fxaa_luma(fxaa_tap(image, point + vec2(-1.0, 1.0), bounds, texel).rgb, linear);
    float se = fxaa_luma(fxaa_tap(image, point + vec2(1.0, 1.0), bounds, texel).rgb, linear);
    float low = min(lm, min(min(nw, ne), min(sw, se))), high = max(lm, max(max(nw, ne), max(sw, se)));
    if (high - low < max(FXAA_EDGE_MIN, high * FXAA_EDGE_RELATIVE)) return middle;
    // across the luma's gradient, scaled so that its shorter part is about a pixel
    vec2 direction = vec2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.25 * FXAA_REDUCE_RELATIVE, FXAA_REDUCE_MIN);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce), -FXAA_SPAN_MAX, FXAA_SPAN_MAX);
    vec3 narrow = 0.5 * (fxaa_tap(image, point + direction * (1.0 / 3.0 - 0.5), bounds, texel).rgb +
                         fxaa_tap(image, point + direction * (2.0 / 3.0 - 0.5), bounds, texel).rgb);
    vec3 wide = 0.5 * narrow + 0.25 * (fxaa_tap(image, point - direction * 0.5, bounds, texel).rgb +
                                       fxaa_tap(image, point + direction * 0.5, bounds, texel).rgb);
    float l = fxaa_luma(wide, linear);
    return vec4(l < low || l > high ? narrow : wide, middle.a);
}
