// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.net.Uri;
import java.util.ArrayList;
import java.util.List;

/**
 * Matches content URIs to codes: a tree of the authority and the path segments. In a pattern, "#" takes a segment
 * of digits and "*" any segment. An exact segment wins over "#", and "#" wins over "*".
 */
public class UriMatcher {
    public static final int NO_MATCH = -1;

    private int code;
    private final String text;
    private final ArrayList<UriMatcher> children = new ArrayList<>();

    public UriMatcher(int code) { this(code, null); }

    private UriMatcher(int code, String text) {
        this.code = code;
        this.text = text;
    }

    public void addURI(String authority, String path, int code) {
        if (path != null && path.startsWith("/")) path = path.substring(1);
        ArrayList<String> tokens = new ArrayList<>();
        tokens.add(authority);
        if (path != null && !path.isEmpty()) for (String t : path.split("/")) tokens.add(t);
        UriMatcher node = this;
        for (int i = 0; i < tokens.size(); ++i) {
            String token = tokens.get(i);
            UriMatcher next = null;
            for (UriMatcher c : node.children) if (c.text.equals(token)) { next = c; break; }
            if (next == null) {
                next = new UriMatcher(NO_MATCH, token);
                node.children.add(next);
            }
            node = next;
        }
        node.code = code;
    }

    public int match(Uri uri) {
        List<String> segments = uri.getPathSegments();
        int n = segments.size();
        if (n == 0 && uri.getAuthority() == null) return code;
        UriMatcher node = this;
        for (int i = -1; i < n; ++i) {
            String s = i < 0 ? uri.getAuthority() : segments.get(i);
            if (s == null) s = "";
            UriMatcher exact = null, number = null, any = null;
            for (UriMatcher c : node.children) {
                if (c.text.equals(s)) exact = c;
                else if (c.text.equals("#") && isNumber(s)) number = c;
                else if (c.text.equals("*")) any = c;
            }
            node = exact != null ? exact : number != null ? number : any;
            if (node == null) return NO_MATCH;
        }
        return node.code;
    }

    private static boolean isNumber(String s) {
        if (s.isEmpty()) return false;
        for (int i = 0; i < s.length(); ++i) if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
        return true;
    }
}
