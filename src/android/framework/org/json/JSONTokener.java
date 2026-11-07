// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package org.json;

/** Reads JSON text as Android's does: lenient about a few things (single quotes, unquoted names, # comments). */
public class JSONTokener {
    private final String in;
    private int pos;

    public JSONTokener(String in) {
        if (in != null && in.startsWith("\ufeff")) in = in.substring(1);
        this.in = in;
    }

    public Object nextValue() throws JSONException {
        int c = nextCleanInternal();
        switch (c) {
            case -1: throw syntaxError("End of input");
            case '{': return readObject();
            case '[': return readArray();
            case '\'': case '"': return nextString((char) c);
            default: --pos; return readLiteral();
        }
    }

    private int nextCleanInternal() throws JSONException {
        while (pos < in.length()) {
            char c = in.charAt(pos++);
            switch (c) {
                case '\t': case ' ': case '\n': case '\r': continue;
                case '/':
                    if (pos == in.length()) return c;
                    char peek = in.charAt(pos);
                    if (peek == '*') {
                        int end = in.indexOf("*/", pos + 1);
                        if (end == -1) throw syntaxError("Unterminated comment");
                        pos = end + 2;
                        continue;
                    }
                    if (peek == '/') { ++pos; skipToEndOfLine(); continue; }
                    return c;
                case '#': skipToEndOfLine(); continue;
                default: return c;
            }
        }
        return -1;
    }

    private void skipToEndOfLine() {
        for (; pos < in.length(); ++pos) {
            char c = in.charAt(pos);
            if (c == '\r' || c == '\n') { ++pos; break; }
        }
    }

    public String nextString(char quote) throws JSONException {
        StringBuilder b = null;
        int start = pos;
        while (pos < in.length()) {
            int c = in.charAt(pos++);
            if (c == quote) {
                if (b == null) return in.substring(start, pos - 1);
                b.append(in, start, pos - 1);
                return b.toString();
            }
            if (c == '\\') {
                if (pos == in.length()) throw syntaxError("Unterminated escape sequence");
                if (b == null) b = new StringBuilder();
                b.append(in, start, pos - 1);
                b.append(readEscape());
                start = pos;
            }
        }
        throw syntaxError("Unterminated string");
    }

    private char readEscape() throws JSONException {
        char e = in.charAt(pos++);
        switch (e) {
            case 'u':
                if (pos + 4 > in.length()) throw syntaxError("Unterminated escape sequence");
                String hex = in.substring(pos, pos + 4);
                pos += 4;
                try { return (char) Integer.parseInt(hex, 16); }
                catch (NumberFormatException x) { throw syntaxError("Invalid escape sequence: " + hex); }
            case 't': return '\t';
            case 'b': return '\b';
            case 'n': return '\n';
            case 'r': return '\r';
            case 'f': return '\f';
            default: return e;
        }
    }

    private Object readLiteral() throws JSONException {
        String literal = nextToInternal("{}[]/\\:,=;# \t\f");
        if (literal.isEmpty()) throw syntaxError("Expected literal value");
        if ("null".equalsIgnoreCase(literal)) return JSONObject.NULL;
        if ("true".equalsIgnoreCase(literal)) return Boolean.TRUE;
        if ("false".equalsIgnoreCase(literal)) return Boolean.FALSE;
        if (literal.indexOf('.') == -1) {
            int base = 10;
            String number = literal;
            if (number.startsWith("0x") || number.startsWith("0X")) { number = number.substring(2); base = 16; }
            else if (number.startsWith("0") && number.length() > 1) { number = number.substring(1); base = 8; }
            try {
                long v = Long.parseLong(number, base);
                return v <= Integer.MAX_VALUE && v >= Integer.MIN_VALUE ? (Object) (int) v : (Object) v;
            } catch (NumberFormatException ignored) {}
        }
        try { return Double.valueOf(literal); } catch (NumberFormatException ignored) {}
        return literal; // an unquoted string
    }

    private String nextToInternal(String excluded) {
        int start = pos;
        for (; pos < in.length(); ++pos) {
            char c = in.charAt(pos);
            if (c == '\r' || c == '\n' || excluded.indexOf(c) != -1) return in.substring(start, pos);
        }
        return in.substring(start);
    }

    private JSONObject readObject() throws JSONException {
        JSONObject result = new JSONObject();
        int first = nextCleanInternal();
        if (first == '}') return result;
        if (first != -1) --pos;
        while (true) {
            Object name = nextValue();
            if (!(name instanceof String)) {
                if (name == null) throw syntaxError("Names cannot be null");
                throw syntaxError("Names must be strings, but " + name + " is of type " + name.getClass().getName());
            }
            int separator = nextCleanInternal();
            if (separator != ':' && separator != '=') throw syntaxError("Expected ':' after " + name);
            if (pos < in.length() && in.charAt(pos) == '>') ++pos;
            result.put((String) name, nextValue());
            switch (nextCleanInternal()) {
                case '}': return result;
                case ';': case ',': continue;
                default: throw syntaxError("Unterminated object");
            }
        }
    }

    private JSONArray readArray() throws JSONException {
        JSONArray result = new JSONArray();
        boolean hasTrailingSeparator = false;
        while (true) {
            switch (nextCleanInternal()) {
                case -1: throw syntaxError("Unterminated array");
                case ']': if (hasTrailingSeparator) result.put(null); return result;
                case ',': case ';': result.put(null); hasTrailingSeparator = true; continue;
                default: --pos;
            }
            result.put(nextValue());
            switch (nextCleanInternal()) {
                case ']': return result;
                case ',': case ';': hasTrailingSeparator = true; continue;
                default: throw syntaxError("Unterminated array");
            }
        }
    }

    public JSONException syntaxError(String message) { return new JSONException(message + this); }
    @Override public String toString() { return " at character " + pos + " of " + in; }
    public boolean more() { return pos < in.length(); }
    public char next() { return pos < in.length() ? in.charAt(pos++) : '\0'; }
    public char next(char c) throws JSONException {
        char result = next();
        if (result != c) throw syntaxError("Expected " + c + " but was " + result);
        return result;
    }
    public char nextClean() throws JSONException { int c = nextCleanInternal(); return c == -1 ? '\0' : (char) c; }
    public String next(int length) throws JSONException {
        if (pos + length > in.length()) throw syntaxError(length + " is out of bounds");
        String result = in.substring(pos, pos + length);
        pos += length;
        return result;
    }
    public String nextTo(String excluded) { return nextToInternal(excluded).trim(); }
    public String nextTo(char excluded) { return nextToInternal(String.valueOf(excluded)).trim(); }
    public void skipPast(String thru) { int i = in.indexOf(thru, pos); pos = i == -1 ? in.length() : i + thru.length(); }
    public char skipTo(char to) {
        int i = in.indexOf(to, pos);
        if (i == -1) return '\0';
        pos = i;
        return to;
    }
    public void back() { if (--pos == -1) pos = 0; }
    public static int dehexchar(char hex) {
        if (hex >= '0' && hex <= '9') return hex - '0';
        if (hex >= 'A' && hex <= 'F') return hex - 'A' + 10;
        if (hex >= 'a' && hex <= 'f') return hex - 'a' + 10;
        return -1;
    }
}
