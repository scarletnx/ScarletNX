/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_IMAGE_H
#define SNX_ANDROID_IMAGE_H
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The image codec of BitmapFactory and Bitmap.compress, the HLE of Skia's codecs, over FFmpeg's decoders and
   encoders. Pixels are Android's int colors (0xAARRGGBB, not premultiplied) in the host's byte order, as a jint
   holds them */

/* the formats, by the numbers snx.android.hwui.Image gives them */
typedef enum {
    ANDROID_IMAGE_NONE = 0,
    ANDROID_IMAGE_PNG = 1,
    ANDROID_IMAGE_JPEG = 2,
    ANDROID_IMAGE_WEBP = 3,
    ANDROID_IMAGE_GIF = 4,
    ANDROID_IMAGE_BMP = 5,
} AndroidImageFormat;

/* the format of an encoded image, from its signature */
AndroidImageFormat android_image_format(const unsigned char* data, size_t size);

/* a decoded image: the decoder's frame (its first, for an animated GIF) */
typedef struct {
    struct AVFrame* frame;
    AndroidImageFormat format;
    int width, height;
} AndroidImage;

/* decodes data. False, with *image empty, for a format it does not know or data that does not decode */
bool android_image_decode(AndroidImage* image, const unsigned char* data, size_t size);
/* the image scaled to width by height (area averaging when smaller), into pixels (width * height of them). Whether
   any pixel is not opaque goes into *alpha */
bool android_image_pixels(const AndroidImage* image, int width, int height, jint* pixels, bool* alpha);
void android_image_free(AndroidImage* image);

/* pixels (width * height) encoded as a PNG (with an alpha channel when alpha is true) or a JPEG of quality 0 to 100.
   The encoded size, with *out from malloc, or 0 */
size_t android_image_encode(const jint* pixels, int width, int height, AndroidImageFormat format, int quality,
                            bool alpha, unsigned char** out);

/* snx.android.hwui.Image's natives */
bool android_image_register(JNIEnv* env);
#endif
