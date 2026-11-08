// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** A URI: scheme, authority, path, query and fragment, kept encoded as Android keeps them. */
public abstract class Uri implements android.os.Parcelable, Comparable<Uri> {
    public static final Uri EMPTY = new StringUri("");
    private static final String HEX = "0123456789ABCDEF";

    Uri() {}

    public static Uri parse(String uriString) { return new StringUri(uriString); }
    public static Uri fromFile(java.io.File file) { return new Builder().scheme("file").authority("").path(file.getAbsolutePath()).build(); }
    public static Uri fromParts(String scheme, String ssp, String fragment) {
        return new StringUri(scheme + ":" + encode(ssp, "/@:?") + (fragment != null ? "#" + encode(fragment) : ""));
    }
    public static Uri withAppendedPath(Uri baseUri, String pathSegment) { return baseUri.buildUpon().appendEncodedPath(pathSegment).build(); }

    public abstract boolean isHierarchical();
    public boolean isOpaque() { return !isHierarchical(); }
    public boolean isRelative() { return getScheme() == null; }
    public boolean isAbsolute() { return !isRelative(); }
    public abstract String getScheme();
    public abstract String getSchemeSpecificPart();
    public abstract String getEncodedSchemeSpecificPart();
    public abstract String getAuthority();
    public abstract String getEncodedAuthority();
    public abstract String getUserInfo();
    public abstract String getEncodedUserInfo();
    public abstract String getHost();
    public abstract int getPort();
    public abstract String getPath();
    public abstract String getEncodedPath();
    public abstract String getQuery();
    public abstract String getEncodedQuery();
    public abstract String getFragment();
    public abstract String getEncodedFragment();
    public abstract List<String> getPathSegments();
    public abstract String getLastPathSegment();
    public abstract Builder buildUpon();

    public Set<String> getQueryParameterNames() {
        String q = getEncodedQuery();
        if (q == null) return Collections.emptySet();
        LinkedHashSet<String> names = new LinkedHashSet<>();
        for (String part : q.split("&")) {
            int eq = part.indexOf('=');
            names.add(decode(eq >= 0 ? part.substring(0, eq) : part));
        }
        return Collections.unmodifiableSet(names);
    }
    public List<String> getQueryParameters(String key) {
        String q = getEncodedQuery();
        if (q == null) return Collections.emptyList();
        ArrayList<String> values = new ArrayList<>();
        for (String part : q.split("&")) {
            int eq = part.indexOf('=');
            if (decode(eq >= 0 ? part.substring(0, eq) : part).equals(key)) values.add(eq >= 0 ? decode(part.substring(eq + 1).replace('+', ' ')) : "");
        }
        return values;
    }
    public String getQueryParameter(String key) {
        List<String> v = getQueryParameters(key);
        return v.isEmpty() ? null : v.get(0);
    }
    public boolean getBooleanQueryParameter(String key, boolean defaultValue) {
        String v = getQueryParameter(key);
        if (v == null) return defaultValue;
        v = v.toLowerCase(java.util.Locale.ROOT);
        return !"false".equals(v) && !"0".equals(v);
    }
    public Uri normalizeScheme() {
        String s = getScheme();
        return s == null || s.equals(s.toLowerCase(java.util.Locale.ROOT)) ? this : buildUpon().scheme(s.toLowerCase(java.util.Locale.ROOT)).build();
    }

    @Override public boolean equals(Object o) { return o instanceof Uri u && toString().equals(u.toString()); }
    @Override public int hashCode() { return toString().hashCode(); }
    @Override public int compareTo(Uri other) { return toString().compareTo(other.toString()); }
    public String toSafeString() { return toString(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeString(toString()); }
    public static void writeToParcel(android.os.Parcel out, Uri uri) { out.writeString(uri != null ? uri.toString() : null); }
    public static final android.os.Parcelable.Creator<Uri> CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public Uri createFromParcel(android.os.Parcel in) { String s = in.readString(); return s != null ? parse(s) : null; }
        @Override public Uri[] newArray(int size) { return new Uri[size]; }
    };

    public static String encode(String s) { return encode(s, null); }
    public static String encode(String s, String allow) {
        if (s == null) return null;
        StringBuilder b = null;
        byte[] bytes = s.getBytes(StandardCharsets.UTF_8);
        for (int i = 0; i < bytes.length; ++i) {
            int c = bytes[i] & 0xff;
            boolean ok = c < 0x80 && (Character.isLetterOrDigit(c) || "_-!.~'()*".indexOf(c) >= 0 || (allow != null && allow.indexOf(c) >= 0));
            if (ok) { if (b != null) b.append((char) c); }
            else {
                if (b == null) b = new StringBuilder(new String(bytes, 0, i, StandardCharsets.UTF_8));
                b.append('%').append(HEX.charAt(c >> 4)).append(HEX.charAt(c & 15));
            }
        }
        return b != null ? b.toString() : s;
    }
    public static String decode(String s) {
        if (s == null) return null;
        if (s.indexOf('%') < 0) return s;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (int i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            if (c == '%' && i + 2 < s.length()) {
                int h = Character.digit(s.charAt(i + 1), 16), l = Character.digit(s.charAt(i + 2), 16);
                if (h >= 0 && l >= 0) { out.write(h << 4 | l); i += 2; continue; }
            }
            byte[] b = String.valueOf(c).getBytes(StandardCharsets.UTF_8);
            out.write(b, 0, b.length);
        }
        return out.toString(StandardCharsets.UTF_8);
    }

    /** A URI from its string, split into its parts as they are needed. */
    private static final class StringUri extends Uri {
        private final String s;

        StringUri(String s) { this.s = s; }
        private int schemeEnd() {
            int ssi = s.indexOf(':');
            if (ssi < 0) return -1;
            for (int i = 0; i < ssi; ++i) { char c = s.charAt(i); if (c == '/' || c == '?' || c == '#') return -1; }
            return ssi;
        }
        private int fragmentStart() { return s.indexOf('#', Math.max(0, schemeEnd())); }
        @Override public String getScheme() { int e = schemeEnd(); return e < 0 ? null : s.substring(0, e); }
        @Override public String getEncodedSchemeSpecificPart() {
            int start = schemeEnd() + 1, f = fragmentStart();
            return f < 0 ? s.substring(start) : s.substring(start, f);
        }
        @Override public String getSchemeSpecificPart() { return decode(getEncodedSchemeSpecificPart()); }
        @Override public boolean isHierarchical() {
            int e = schemeEnd();
            if (e < 0) return true;
            return s.length() > e + 1 && s.charAt(e + 1) == '/';
        }
        @Override public String getEncodedAuthority() {
            String ssp = getEncodedSchemeSpecificPart();
            if (!ssp.startsWith("//")) return null;
            int end = 2;
            while (end < ssp.length() && "/?#".indexOf(ssp.charAt(end)) < 0) ++end;
            return ssp.substring(2, end);
        }
        @Override public String getAuthority() { return decode(getEncodedAuthority()); }
        @Override public String getEncodedUserInfo() {
            String a = getEncodedAuthority();
            if (a == null) return null;
            int at = a.lastIndexOf('@');
            return at < 0 ? null : a.substring(0, at);
        }
        @Override public String getUserInfo() { return decode(getEncodedUserInfo()); }
        @Override public String getHost() {
            String a = getEncodedAuthority();
            if (a == null) return null;
            int at = a.lastIndexOf('@'), colon = a.indexOf(':', at + 1);
            return decode(colon >= 0 ? a.substring(at + 1, colon) : a.substring(at + 1));
        }
        @Override public int getPort() {
            String a = getEncodedAuthority();
            if (a == null) return -1;
            int at = a.lastIndexOf('@'), colon = a.indexOf(':', at + 1);
            try { return colon >= 0 ? Integer.parseInt(a.substring(colon + 1)) : -1; } catch (NumberFormatException e) { return -1; }
        }
        @Override public String getEncodedPath() {
            if (!isHierarchical()) return null;
            String ssp = getEncodedSchemeSpecificPart();
            int start = 0;
            if (ssp.startsWith("//")) { start = 2; while (start < ssp.length() && "/?#".indexOf(ssp.charAt(start)) < 0) ++start; }
            int end = ssp.indexOf('?', start);
            return end < 0 ? ssp.substring(start) : ssp.substring(start, end);
        }
        @Override public String getPath() { return decode(getEncodedPath()); }
        @Override public String getEncodedQuery() {
            if (!isHierarchical()) return null;
            String ssp = getEncodedSchemeSpecificPart();
            int q = ssp.indexOf('?');
            return q < 0 ? null : ssp.substring(q + 1);
        }
        @Override public String getQuery() { return decode(getEncodedQuery()); }
        @Override public String getEncodedFragment() { int f = fragmentStart(); return f < 0 ? null : s.substring(f + 1); }
        @Override public String getFragment() { return decode(getEncodedFragment()); }
        @Override public List<String> getPathSegments() {
            String p = getEncodedPath();
            if (p == null) return Collections.emptyList();
            ArrayList<String> out = new ArrayList<>();
            for (String seg : p.split("/")) if (!seg.isEmpty()) out.add(decode(seg));
            return Collections.unmodifiableList(out);
        }
        @Override public String getLastPathSegment() {
            List<String> seg = getPathSegments();
            return seg.isEmpty() ? null : seg.get(seg.size() - 1);
        }
        @Override public Builder buildUpon() {
            Builder b = new Builder().scheme(getScheme());
            if (isHierarchical()) {
                b.encodedAuthority(getEncodedAuthority()).encodedPath(getEncodedPath()).encodedQuery(getEncodedQuery());
            } else {
                b.encodedOpaquePart(getEncodedSchemeSpecificPart());
            }
            return b.encodedFragment(getEncodedFragment());
        }
        @Override public String toString() { return s; }
    }

    public static final class Builder {
        private String scheme, authority, path, query, fragment, opaque;

        public Builder() {}
        public Builder scheme(String scheme) { this.scheme = scheme; return this; }
        public Builder opaquePart(String opaquePart) { opaque = encode(opaquePart, "/@:?=&"); return this; }
        public Builder encodedOpaquePart(String opaquePart) { opaque = opaquePart; return this; }
        public Builder authority(String authority) { this.authority = encode(authority, "@:"); opaque = null; return this; }
        public Builder encodedAuthority(String authority) { this.authority = authority; opaque = null; return this; }
        public Builder path(String path) { this.path = encode(path, "/"); opaque = null; return this; }
        public Builder encodedPath(String path) { this.path = path; opaque = null; return this; }
        public Builder appendPath(String newSegment) { return appendEncodedPath(encode(newSegment)); }
        public Builder appendEncodedPath(String newSegment) {
            opaque = null;
            if (path == null || path.isEmpty()) path = newSegment.startsWith("/") ? newSegment : "/" + newSegment;
            else path = path.endsWith("/") ? path + (newSegment.startsWith("/") ? newSegment.substring(1) : newSegment)
                : path + (newSegment.startsWith("/") ? newSegment : "/" + newSegment);
            return this;
        }
        public Builder query(String query) { this.query = encode(query, "=&"); return this; }
        public Builder encodedQuery(String query) { this.query = query; return this; }
        public Builder fragment(String fragment) { this.fragment = encode(fragment); return this; }
        public Builder encodedFragment(String fragment) { this.fragment = fragment; return this; }
        public Builder appendQueryParameter(String key, String value) {
            String p = encode(key) + "=" + encode(value);
            query = query == null || query.isEmpty() ? p : query + "&" + p;
            return this;
        }
        public Builder clearQuery() { query = null; return this; }
        public Uri build() {
            StringBuilder b = new StringBuilder();
            if (scheme != null) b.append(scheme).append(':');
            if (opaque != null) {
                b.append(opaque);
            } else {
                if (authority != null) b.append("//").append(authority);
                if (path != null) {
                    if (authority != null && !path.isEmpty() && !path.startsWith("/")) b.append('/');
                    b.append(path);
                }
                if (query != null) b.append('?').append(query);
            }
            if (fragment != null) b.append('#').append(fragment);
            return new StringUri(b.toString());
        }
        @Override public String toString() { return build().toString(); }
    }
}
