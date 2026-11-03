/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Image decoding and encoding for BitmapFactory and Bitmap.compress (image.h), over FFmpeg's image codecs. A whole
   file is one packet for each of them, as FFmpeg's image2 demuxer gives it. libswscale converts the decoder's pixel
   format to AV_PIX_FMT_RGB32, which is a 32-bit 0xAARRGGBB in the host's byte order: the layout of a jint, on a
   big-endian host too, so no byte swap follows. FFmpeg writes and reads the pixels as bytes */
#include "android/hwui/image.h"
#include "android/runtime/jvm.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* the largest image in pixels: what a jint array of them holds, and what its bytes in a size_t hold */
enum { MAX_PIXELS = INT_MAX / 4 };

AndroidImageFormat android_image_format(const unsigned char* data, size_t size) {
    static const unsigned char png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (size >= 8 && memcmp(data, png, 8) == 0) return ANDROID_IMAGE_PNG;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ANDROID_IMAGE_JPEG;
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return ANDROID_IMAGE_WEBP;
    if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) return ANDROID_IMAGE_GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return ANDROID_IMAGE_BMP;
    return ANDROID_IMAGE_NONE;
}

static enum AVCodecID codec_id(AndroidImageFormat format) {
    switch (format) {
    case ANDROID_IMAGE_PNG: return AV_CODEC_ID_PNG;
    case ANDROID_IMAGE_JPEG: return AV_CODEC_ID_MJPEG;
    case ANDROID_IMAGE_WEBP: return AV_CODEC_ID_WEBP;
    case ANDROID_IMAGE_GIF: return AV_CODEC_ID_GIF;
    case ANDROID_IMAGE_BMP: return AV_CODEC_ID_BMP;
    case ANDROID_IMAGE_NONE: break;
    }
    return AV_CODEC_ID_NONE;
}

bool android_image_decode(AndroidImage* image, const unsigned char* data, size_t size) {
    *image = (AndroidImage){0};
    AndroidImageFormat format = android_image_format(data, size);
    const AVCodec* codec = avcodec_find_decoder(codec_id(format));
    if (!codec || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) return false;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = false;
    if (context && packet && frame) {
        context->thread_count = 1;
        /* av_new_packet zeroes the padding the decoders read past the end */
        if (avcodec_open2(context, codec, NULL) == 0 && av_new_packet(packet, (int)size) == 0) {
            memcpy(packet->data, data, size);
            /* the flush after the packet: a decoder that holds its frame gives it then */
            ok = avcodec_send_packet(context, packet) == 0 && avcodec_send_packet(context, NULL) == 0 &&
                 avcodec_receive_frame(context, frame) == 0 && frame->width > 0 && frame->height > 0 &&
                 (int64_t)frame->width * frame->height <= MAX_PIXELS;
        }
    }
    av_packet_free(&packet);
    avcodec_free_context(&context);
    if (!ok) {
        av_frame_free(&frame);
        return false;
    }
    /* the JPEG decoder's full-range formats are FFmpeg's old names of the YUV ones with a range, which libswscale
       warns about */
    static const enum AVPixelFormat full[][2] = {{AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P},
                                                 {AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P},
                                                 {AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P},
                                                 {AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUV440P},
                                                 {AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUV411P}};
    for (size_t i = 0; i < sizeof full / sizeof *full; ++i)
        if (frame->format == full[i][0]) {
            frame->format = full[i][1];
            frame->color_range = AVCOL_RANGE_JPEG;
        }
    image->frame = frame;
    image->format = format;
    image->width = frame->width;
    image->height = frame->height;
    return true;
}

/* src converted to dst's format and size, with area averaging, which a smaller dst (inSampleSize) needs */
static bool convert(AVFrame* dst, const AVFrame* src) {
    SwsContext* sws = sws_alloc_context();
    if (!sws) return false;
    sws->flags = SWS_AREA | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    sws->threads = 1;
    bool ok = sws_scale_frame(sws, dst, src) >= 0;
    sws_free_context(&sws);
    return ok;
}

bool android_image_pixels(const AndroidImage* image, int width, int height, jint* pixels, bool* alpha) {
    if (!image->frame || width <= 0 || height <= 0 || (int64_t)width * height > MAX_PIXELS) return false;
    AVFrame* dst = av_frame_alloc();
    if (!dst) return false;
    dst->format = AV_PIX_FMT_RGB32;
    dst->width = width;
    dst->height = height;
    dst->data[0] = (uint8_t*)pixels;
    dst->linesize[0] = width * 4;
    bool ok = convert(dst, image->frame);
    av_frame_free(&dst);
    if (!ok) return false;
    /* a jint's alpha, the top byte, is 0xff for each opaque pixel */
    uint32_t any = 0xffffffffu;
    for (size_t i = 0, n = (size_t)width * height; i < n; ++i) any &= (uint32_t)pixels[i];
    *alpha = any >> 24 != 0xff;
    return true;
}

void android_image_free(AndroidImage* image) {
    av_frame_free(&image->frame);
    *image = (AndroidImage){0};
}

size_t android_image_encode(const jint* pixels, int width, int height, AndroidImageFormat format, int quality,
                            bool alpha, unsigned char** out) {
    *out = NULL;
    bool jpeg = format == ANDROID_IMAGE_JPEG;
    if ((!jpeg && format != ANDROID_IMAGE_PNG) || width <= 0 || height <= 0 ||
        (int64_t)width * height > MAX_PIXELS) return 0;
    const AVCodec* codec = avcodec_find_encoder(jpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame* src = av_frame_alloc();
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    size_t size = 0;
    if (!context || !src || !frame || !packet) goto done;
    enum AVPixelFormat pix = jpeg ? AV_PIX_FMT_YUV420P : alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    context->width = width;
    context->height = height;
    context->pix_fmt = pix;
    context->time_base = (AVRational){1, 1};
    context->thread_count = 1;
    if (jpeg) {
        /* JFIF is full range. Quality 100 is the finest quantizer, 1, and 0 the coarsest, 31 */
        context->color_range = AVCOL_RANGE_JPEG;
        int q = quality < 0 ? 0 : quality > 100 ? 100 : quality;
        context->flags |= AV_CODEC_FLAG_QSCALE;
        context->global_quality = FF_QP2LAMBDA * (1 + (100 - q) * 30 / 100);
    }
    if (avcodec_open2(context, codec, NULL) != 0) goto done;
    src->format = AV_PIX_FMT_RGB32;
    src->width = width;
    src->height = height;
    src->data[0] = (uint8_t*)(uintptr_t)pixels;
    src->linesize[0] = width * 4;
    frame->format = pix;
    frame->width = width;
    frame->height = height;
    frame->color_range = context->color_range;
    if (av_frame_get_buffer(frame, 0) != 0 || !convert(frame, src)) goto done;
    frame->pts = 0;
    if (jpeg) frame->quality = context->global_quality;
    if (avcodec_send_frame(context, frame) != 0 || avcodec_send_frame(context, NULL) != 0 ||
        avcodec_receive_packet(context, packet) != 0 || packet->size <= 0) goto done;
    *out = malloc((size_t)packet->size);
    if (*out) {
        memcpy(*out, packet->data, (size_t)packet->size);
        size = (size_t)packet->size;
    }
done:
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&src);
    avcodec_free_context(&context);
    return size;
}

/* a side of the image decoded with a sample size, as Skia's codecs give it: libjpeg's scale rounds up, and
   SkSampledCodec, which the other formats go through, rounds down to at least 1 */
static int64_t sampled(int64_t side, int64_t sample, AndroidImageFormat format) {
    if (format == ANDROID_IMAGE_JPEG) return (side + sample - 1) / sample;
    return sample > side ? 1 : side / sample;
}

/* Image.decode(data, offset, length, sample, scale, info): the pixels of the image decoded with a sample size, then
   scaled, with info = {width, height, format, has alpha}. sample 0 gives info alone (inJustDecodeBounds), and null */
static jintArray JNICALL decode(JNIEnv* env, jclass unused, jbyteArray data_in, jint offset, jint length, jint sample,
                                jfloat scale, jintArray info_out) {
    (void)unused;
    jsize total = (*env)->GetArrayLength(env, data_in);
    if (offset < 0 || length < 0 || offset > total - length || !(scale > 0 && scale < 64)) return NULL;
    jbyte* data = (*env)->GetByteArrayElements(env, data_in, NULL);
    if (!data) return NULL;
    AndroidImage image;
    bool decoded = android_image_decode(&image, (const unsigned char*)data + offset, (size_t)length);
    (*env)->ReleaseByteArrayElements(env, data_in, data, JNI_ABORT);
    if (!decoded) return NULL;
    int64_t step = sample > 1 ? sample : 1;
    int64_t w = sampled(image.width, step, image.format), h = sampled(image.height, step, image.format);
    if (scale != 1.0f) {
        /* BitmapFactory's rounding of its density scale */
        w = (int64_t)((float)w * scale + 0.5f);
        h = (int64_t)((float)h * scale + 0.5f);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    jintArray pixels_out = NULL;
    jint info[4] = {0, 0, (jint)image.format, 0};
    if (w * h <= MAX_PIXELS) {
        info[0] = (jint)w;
        info[1] = (jint)h;
        if (sample > 0) pixels_out = (*env)->NewIntArray(env, (jsize)(w * h));
    }
    if (pixels_out) {
        bool alpha = false;
        jint* pixels = (*env)->GetPrimitiveArrayCritical(env, pixels_out, NULL);
        bool ok = pixels && android_image_pixels(&image, (int)w, (int)h, pixels, &alpha);
        if (pixels) (*env)->ReleasePrimitiveArrayCritical(env, pixels_out, pixels, 0);
        info[3] = alpha;
        if (!ok) pixels_out = NULL;
    }
    android_image_free(&image);
    (*env)->SetIntArrayRegion(env, info_out, 0, 4, info);
    return pixels_out;
}

/* Image.encode(pixels, width, height, format, quality, alpha): the encoded file, null when it does not encode */
static jbyteArray JNICALL encode(JNIEnv* env, jclass unused, jintArray pixels_in, jint width, jint height,
                                 jint format, jint quality, jboolean alpha) {
    (void)unused;
    if (width <= 0 || height <= 0 || (int64_t)width * height > (*env)->GetArrayLength(env, pixels_in)) return NULL;
    jint* pixels = (*env)->GetIntArrayElements(env, pixels_in, NULL);
    if (!pixels) return NULL;
    unsigned char* bytes;
    size_t size = android_image_encode(pixels, width, height, (AndroidImageFormat)format, quality,
                                       alpha != JNI_FALSE, &bytes);
    (*env)->ReleaseIntArrayElements(env, pixels_in, pixels, JNI_ABORT);
    jbyteArray out = size && size <= INT_MAX ? (*env)->NewByteArray(env, (jsize)size) : NULL;
    if (out) (*env)->SetByteArrayRegion(env, out, 0, (jsize)size, (const jbyte*)bytes);
    free(bytes);
    return out;
}

bool android_image_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"decode", (char*)"([BIIIF[I)[I", (void*)decode},
        {(char*)"encode", (char*)"([IIIIIZ)[B", (void*)encode},
    };
    return android_jvm_register(env, "snx/android/hwui/Image", methods, (int)(sizeof methods / sizeof *methods));
}
