/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The image codec of the Android driver (src/android/hwui/image.c): a PNG that it encodes decodes to the same
   pixels, alpha too; a JPEG comes back near its pixels; a sample size scales down; signatures name their formats */
#include "android/hwui/image.h"
#include "android/runtime/jvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the natives' registration is the program's: the test calls the codec directly */
bool android_jvm_register(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count) {
    (void)env, (void)class_name, (void)methods, (void)count;
    return false;
}

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

enum { W = 37, H = 21 };

static jint pixel(int x, int y, bool alpha) {
    uint32_t a = alpha ? (uint32_t)(x * 7 + y * 3) & 0xff : 0xff;
    uint32_t c = a << 24 | (uint32_t)(x * 6) << 16 | (uint32_t)(y * 12) << 8 | (uint32_t)((x + y) * 4);
    return (jint)c;
}

static int channel(jint c, int shift) {
    return (int)(((uint32_t)c >> shift) & 0xff);
}

static void round_trip(AndroidImageFormat format, bool alpha, int tolerance) {
    static jint pixels[W * H];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) pixels[y * W + x] = pixel(x, y, alpha);
    unsigned char* file;
    size_t size = android_image_encode(pixels, W, H, format, 95, alpha, &file);
    CHECK(size > 0);
    if (!size) return;
    CHECK(android_image_format(file, size) == format);
    AndroidImage image;
    CHECK(android_image_decode(&image, file, size));
    free(file);
    CHECK(image.width == W && image.height == H && image.format == format);
    static jint out[W * H];
    bool has_alpha = !alpha;
    CHECK(android_image_pixels(&image, W, H, out, &has_alpha));
    CHECK(has_alpha == alpha);
    int worst = 0;
    for (int i = 0; i < W * H; ++i)
        for (int shift = 0; shift < 32; shift += 8) {
            int d = abs(channel(out[i], shift) - channel(pixels[i], shift));
            if (d > worst) worst = d;
        }
    if (worst > tolerance) fprintf(stderr, "format %d: a channel is %d from its value\n", (int)format, worst);
    CHECK(worst <= tolerance);
    /* a sample of 2, rounded up, as inSampleSize: each pixel the average of a 2x2 square of the image */
    static jint half[((W + 1) / 2) * ((H + 1) / 2)];
    CHECK(android_image_pixels(&image, (W + 1) / 2, (H + 1) / 2, half, &has_alpha));
    if (format == ANDROID_IMAGE_PNG && !alpha) {
        int green = channel(half[2 * ((W + 1) / 2) + 3], 8); /* y 2 averages rows 4 and 5: about 12 * 4.5 */
        CHECK(green >= 48 && green <= 60);
    }
    android_image_free(&image);
}

int main(void) {
    round_trip(ANDROID_IMAGE_PNG, true, 0);
    round_trip(ANDROID_IMAGE_PNG, false, 0);
    round_trip(ANDROID_IMAGE_JPEG, false, 24);

    static const unsigned char gif[] = "GIF89a", webp[] = "RIFF\0\0\0\0WEBPVP8 ", bmp[] = "BMxx", jpeg[] = {0xff, 0xd8, 0xff, 0xe0};
    CHECK(android_image_format(gif, 6) == ANDROID_IMAGE_GIF);
    CHECK(android_image_format(webp, 16) == ANDROID_IMAGE_WEBP);
    CHECK(android_image_format(bmp, 4) == ANDROID_IMAGE_BMP);
    CHECK(android_image_format(jpeg, 4) == ANDROID_IMAGE_JPEG);
    CHECK(android_image_format(bmp, 1) == ANDROID_IMAGE_NONE);
    AndroidImage image;
    CHECK(!android_image_decode(&image, webp, 16) && !image.frame);

    /* a BMP of 2x1 pixels, 24-bit, rows bottom up and padded to 4 bytes: blue then red */
    static const unsigned char bmp_file[62] = {
        'B', 'M', 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, /* the file header: size, the pixels' offset */
        40, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, /* BITMAPINFOHEADER: 2 by 1, a plane, 24 bits */
        0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* no compression, 8 bytes */
        0xff, 0, 0, 0, 0, 0xff, 0, 0}; /* B, G, R of each pixel, then the padding */
    CHECK(android_image_decode(&image, bmp_file, sizeof bmp_file));
    jint two[2] = {0};
    bool alpha = true;
    CHECK(image.width == 2 && image.height == 1 && android_image_pixels(&image, 2, 1, two, &alpha));
    CHECK((uint32_t)two[0] == 0xff0000ffu && (uint32_t)two[1] == 0xffff0000u && !alpha);
    android_image_free(&image);

    if (failures) fprintf(stderr, "%d checks failed\n", failures);
    return failures != 0;
}
