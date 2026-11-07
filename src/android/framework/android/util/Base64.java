// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** Base64 with Android's flags, on java.util.Base64. */
public class Base64 {
    public static final int DEFAULT = 0;
    public static final int NO_PADDING = 1;
    public static final int NO_WRAP = 2;
    public static final int CRLF = 4;
    public static final int URL_SAFE = 8;
    public static final int NO_CLOSE = 16;

    private Base64() {}

    public static byte[] decode(String str, int flags) { return decode(str.getBytes(java.nio.charset.StandardCharsets.US_ASCII), flags); }
    public static byte[] decode(byte[] input, int flags) { return decode(input, 0, input.length, flags); }

    public static byte[] decode(byte[] input, int offset, int len, int flags) {
        StringBuilder b = new StringBuilder(len);
        for (int i = offset; i < offset + len; ++i) {
            char c = (char) (input[i] & 0xff);
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            if ((flags & URL_SAFE) != 0) c = c == '-' ? '+' : c == '_' ? '/' : c;
            b.append(c);
        }
        while (b.length() % 4 != 0) b.append('=');
        try {
            return java.util.Base64.getDecoder().decode(b.toString());
        } catch (IllegalArgumentException e) {
            throw new IllegalArgumentException("bad base-64", e);
        }
    }

    public static String encodeToString(byte[] input, int flags) { return encodeToString(input, 0, input.length, flags); }

    public static String encodeToString(byte[] input, int offset, int len, int flags) {
        return new String(encode(input, offset, len, flags), java.nio.charset.StandardCharsets.US_ASCII);
    }

    public static byte[] encode(byte[] input, int flags) { return encode(input, 0, input.length, flags); }

    public static byte[] encode(byte[] input, int offset, int len, int flags) {
        byte[] part = java.util.Arrays.copyOfRange(input, offset, offset + len);
        java.util.Base64.Encoder e = (flags & URL_SAFE) != 0 ? java.util.Base64.getUrlEncoder() : java.util.Base64.getEncoder();
        if ((flags & NO_PADDING) != 0) e = e.withoutPadding();
        String s = e.encodeToString(part);
        if ((flags & NO_WRAP) == 0) {
            String nl = (flags & CRLF) != 0 ? "\r\n" : "\n";
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < s.length(); i += 76) b.append(s, i, Math.min(s.length(), i + 76)).append(nl);
            s = b.toString();
        }
        return s.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    }
}
