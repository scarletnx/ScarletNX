// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** A package's signing certificate. */
public class Signature implements android.os.Parcelable {
    private final byte[] bytes;
    public Signature(byte[] signature) { bytes = signature.clone(); }
    public Signature(String text) { bytes = text.getBytes(java.nio.charset.StandardCharsets.US_ASCII); }
    public byte[] toByteArray() { return bytes.clone(); }
    public char[] toChars() { char[] c = new char[bytes.length * 2]; String h = "0123456789abcdef"; for (int i = 0; i < bytes.length; ++i) { c[2 * i] = h.charAt((bytes[i] >> 4) & 15); c[2 * i + 1] = h.charAt(bytes[i] & 15); } return c; }
    public String toCharsString() { return new String(toChars()); }
    @Override public boolean equals(Object o) { return o instanceof Signature s && java.util.Arrays.equals(s.bytes, bytes); }
    @Override public int hashCode() { return java.util.Arrays.hashCode(bytes); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeByteArray(bytes); }
}
