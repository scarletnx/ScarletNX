// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.view.Surface;
import java.io.IOException;
import java.nio.ByteBuffer;

/** Encoders and decoders. No codec is here, so none can be created. */
public final class MediaCodec {
    public static final int BUFFER_FLAG_KEY_FRAME = 1, BUFFER_FLAG_CODEC_CONFIG = 2, BUFFER_FLAG_END_OF_STREAM = 4,
        BUFFER_FLAG_PARTIAL_FRAME = 8;
    public static final int CONFIGURE_FLAG_ENCODE = 1;
    public static final int INFO_TRY_AGAIN_LATER = -1, INFO_OUTPUT_FORMAT_CHANGED = -2, INFO_OUTPUT_BUFFERS_CHANGED = -3;
    public static final int CRYPTO_MODE_UNENCRYPTED = 0, CRYPTO_MODE_AES_CTR = 1, CRYPTO_MODE_AES_CBC = 2;

    private MediaCodec() {}

    public static MediaCodec createDecoderByType(String type) throws IOException {
        if (type == null) throw new NullPointerException("type");
        throw new IOException("snx: no decoder for " + type);
    }
    public static MediaCodec createEncoderByType(String type) throws IOException {
        if (type == null) throw new NullPointerException("type");
        throw new IOException("snx: no encoder for " + type);
    }
    public static MediaCodec createByCodecName(String name) throws IOException {
        if (name == null) throw new NullPointerException("name");
        throw new IOException("snx: no codec " + name);
    }

    public void configure(MediaFormat format, Surface surface, MediaCrypto crypto, int flags) {}
    public void start() {}
    public void stop() {}
    public void flush() {}
    public void release() {}
    public void reset() {}
    public int dequeueInputBuffer(long timeoutUs) { return INFO_TRY_AGAIN_LATER; }
    public int dequeueOutputBuffer(BufferInfo info, long timeoutUs) { return INFO_TRY_AGAIN_LATER; }
    public void queueInputBuffer(int index, int offset, int size, long presentationTimeUs, int flags) {}
    public void releaseOutputBuffer(int index, boolean render) {}
    public ByteBuffer getInputBuffer(int index) { return null; }
    public ByteBuffer getOutputBuffer(int index) { return null; }
    @Deprecated public ByteBuffer[] getInputBuffers() { return new ByteBuffer[0]; }
    @Deprecated public ByteBuffer[] getOutputBuffers() { return new ByteBuffer[0]; }
    public MediaFormat getOutputFormat() { return new MediaFormat(); }

    public static final class BufferInfo {
        public int offset, size, flags;
        public long presentationTimeUs;

        public BufferInfo() {}

        public void set(int newOffset, int newSize, long newTimeUs, int newFlags) {
            offset = newOffset; size = newSize; presentationTimeUs = newTimeUs; flags = newFlags;
        }
    }

    public static final class CryptoInfo {
        public int numSubSamples, mode;
        public int[] numBytesOfClearData, numBytesOfEncryptedData;
        public byte[] key, iv;
        private Pattern pattern = new Pattern(0, 0);

        public CryptoInfo() {}

        public void set(int newNumSubSamples, int[] newNumBytesOfClearData, int[] newNumBytesOfEncryptedData,
                byte[] newKey, byte[] newIV, int newMode) {
            numSubSamples = newNumSubSamples; numBytesOfClearData = newNumBytesOfClearData;
            numBytesOfEncryptedData = newNumBytesOfEncryptedData; key = newKey; iv = newIV; mode = newMode;
            pattern = new Pattern(0, 0);
        }

        public void setPattern(Pattern newPattern) { pattern = newPattern != null ? newPattern : new Pattern(0, 0); }
        public Pattern getPattern() { return new Pattern(pattern.getEncryptBlocks(), pattern.getSkipBlocks()); }

        public static final class Pattern {
            private int encrypt, skip;

            public Pattern(int blocksToEncrypt, int blocksToSkip) { set(blocksToEncrypt, blocksToSkip); }
            public void set(int blocksToEncrypt, int blocksToSkip) { encrypt = blocksToEncrypt; skip = blocksToSkip; }
            public int getSkipBlocks() { return skip; }
            public int getEncryptBlocks() { return encrypt; }
        }
    }
}
