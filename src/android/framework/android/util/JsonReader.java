// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;

/** Reads one JSON value as a stream of tokens. Lenient mode takes what Android's does: comments, unquoted text. */
public final class JsonReader implements java.io.Closeable {
    private enum Scope { EMPTY_ARRAY, NONEMPTY_ARRAY, EMPTY_OBJECT, DANGLING_NAME, NONEMPTY_OBJECT, EMPTY_DOCUMENT, NONEMPTY_DOCUMENT, CLOSED }

    private final Reader in;
    private final ArrayList<Scope> stack = new ArrayList<>();
    private boolean lenient;
    private int lookahead = -2; /* a character read back, or -2 */
    private JsonToken token;    /* the token peeked, null until peek */
    private String value;       /* its text: a name, a string, a number or a literal */

    public JsonReader(Reader in) {
        if (in == null) throw new NullPointerException("in == null");
        this.in = in;
        stack.add(Scope.EMPTY_DOCUMENT);
    }
    public void setLenient(boolean lenient) { this.lenient = lenient; }
    public boolean isLenient() { return lenient; }

    private int read() throws IOException {
        if (lookahead != -2) {
            int c = lookahead;
            lookahead = -2;
            return c;
        }
        return in.read();
    }
    private void unread(int c) { lookahead = c; }
    private int nextNonWhitespace() throws IOException {
        for (int c; (c = read()) != -1;) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            if (c == '/' && lenient) {
                int n = read();
                if (n == '*') { skipComment(); continue; }
                if (n == '/') { skipLine(); continue; }
                unread(n);
            }
            if (c == '#' && lenient) { skipLine(); continue; }
            return c;
        }
        return -1;
    }
    private void skipLine() throws IOException { for (int c; (c = read()) != -1 && c != '\n';) {} }
    private void skipComment() throws IOException {
        for (int c, prev = 0; (c = read()) != -1; prev = c) if (prev == '*' && c == '/') return;
        throw syntaxError("Unterminated comment");
    }
    private Scope top() { return stack.get(stack.size() - 1); }
    private void replaceTop(Scope s) { stack.set(stack.size() - 1, s); }
    private IOException syntaxError(String message) { return new MalformedJsonException(message); }

    public JsonToken peek() throws IOException {
        if (token != null) return token;
        switch (top()) {
            case EMPTY_DOCUMENT -> {
                replaceTop(Scope.NONEMPTY_DOCUMENT);
                JsonToken first = nextValue();
                if (!lenient && first != JsonToken.BEGIN_ARRAY && first != JsonToken.BEGIN_OBJECT)
                    throw new IOException("Expected JSON document to start with '[' or '{'");
                return first;
            }
            case EMPTY_ARRAY -> {
                replaceTop(Scope.NONEMPTY_ARRAY);
                int c = nextNonWhitespace();
                if (c == ']') { stack.remove(stack.size() - 1); return token = JsonToken.END_ARRAY; }
                unread(c);
                return nextValue();
            }
            case NONEMPTY_ARRAY -> {
                int c = nextNonWhitespace();
                if (c == ']') { stack.remove(stack.size() - 1); return token = JsonToken.END_ARRAY; }
                if (c != ',' && !(lenient && c == ';')) throw syntaxError("Unterminated array");
                return nextValue();
            }
            case EMPTY_OBJECT, NONEMPTY_OBJECT -> {
                int c = nextNonWhitespace();
                if (c == '}') { stack.remove(stack.size() - 1); return token = JsonToken.END_OBJECT; }
                if (top() == Scope.NONEMPTY_OBJECT) {
                    if (c != ',' && !(lenient && c == ';')) throw syntaxError("Unterminated object");
                    c = nextNonWhitespace();
                }
                replaceTop(Scope.DANGLING_NAME);
                if (c == '"' || (lenient && c == '\'')) value = readString((char) c);
                else if (lenient) { unread(c); value = readLiteral(); }
                else throw syntaxError("Expected name");
                return token = JsonToken.NAME;
            }
            case DANGLING_NAME -> {
                int c = nextNonWhitespace();
                if (c == '=' && lenient) {
                    int n = read();
                    if (n != '>') unread(n);
                } else if (c != ':') {
                    throw syntaxError("Expected ':'");
                }
                replaceTop(Scope.NONEMPTY_OBJECT);
                return nextValue();
            }
            case NONEMPTY_DOCUMENT -> {
                int c = nextNonWhitespace();
                if (c == -1) return token = JsonToken.END_DOCUMENT;
                if (!lenient) throw syntaxError("Expected EOF");
                unread(c);
                return nextValue();
            }
            default -> throw new IllegalStateException("JsonReader is closed");
        }
    }

    private JsonToken nextValue() throws IOException {
        int c = nextNonWhitespace();
        switch (c) {
            case -1: throw new java.io.EOFException("End of input");
            case '{': stack.add(Scope.EMPTY_OBJECT); return token = JsonToken.BEGIN_OBJECT;
            case '[': stack.add(Scope.EMPTY_ARRAY); return token = JsonToken.BEGIN_ARRAY;
            case '\'': if (!lenient) throw syntaxError("Unexpected '");
            // fall through
            case '"': value = readString((char) c); return token = JsonToken.STRING;
            default:
                unread(c);
                value = readLiteral();
                if (value.isEmpty()) throw syntaxError("Expected literal value");
                if (value.equals("null")) return token = JsonToken.NULL;
                if (value.equals("true") || value.equals("false")) return token = JsonToken.BOOLEAN;
                try {
                    Double.parseDouble(value);
                    return token = JsonToken.NUMBER;
                } catch (NumberFormatException e) {
                    if (!lenient) throw syntaxError("Unexpected value " + value);
                    return token = JsonToken.STRING;
                }
        }
    }
    private String readString(char quote) throws IOException {
        StringBuilder b = new StringBuilder();
        for (int c; (c = read()) != -1;) {
            if (c == quote) return b.toString();
            if (c == '\\') {
                int e = read();
                switch (e) {
                    case 'u' -> {
                        char[] hex = new char[4];
                        for (int i = 0; i < 4; ++i) hex[i] = (char) read();
                        b.append((char) Integer.parseInt(new String(hex), 16));
                    }
                    case 't' -> b.append('\t');
                    case 'b' -> b.append('\b');
                    case 'n' -> b.append('\n');
                    case 'r' -> b.append('\r');
                    case 'f' -> b.append('\f');
                    case -1 -> throw syntaxError("Unterminated escape sequence");
                    default -> b.append((char) e);
                }
            } else {
                b.append((char) c);
            }
        }
        throw syntaxError("Unterminated string");
    }
    private String readLiteral() throws IOException {
        StringBuilder b = new StringBuilder();
        for (int c; (c = read()) != -1;) {
            if ("{}[]:, \t\f\r\n;#=\\/'\"".indexOf(c) >= 0) { unread(c); break; }
            b.append((char) c);
        }
        return b.toString();
    }

    private void expect(JsonToken t) throws IOException {
        if (peek() != t) throw new IllegalStateException("Expected " + t + " but was " + peek());
        token = null;
    }
    public void beginArray() throws IOException { expect(JsonToken.BEGIN_ARRAY); }
    public void endArray() throws IOException { expect(JsonToken.END_ARRAY); }
    public void beginObject() throws IOException { expect(JsonToken.BEGIN_OBJECT); }
    public void endObject() throws IOException { expect(JsonToken.END_OBJECT); }
    public boolean hasNext() throws IOException { JsonToken t = peek(); return t != JsonToken.END_OBJECT && t != JsonToken.END_ARRAY; }
    public String nextName() throws IOException { expect(JsonToken.NAME); return value; }
    public String nextString() throws IOException {
        JsonToken t = peek();
        if (t != JsonToken.STRING && t != JsonToken.NUMBER) throw new IllegalStateException("Expected a string but was " + t);
        token = null;
        return value;
    }
    public boolean nextBoolean() throws IOException { expect(JsonToken.BOOLEAN); return value.equals("true"); }
    public void nextNull() throws IOException { expect(JsonToken.NULL); }
    public double nextDouble() throws IOException {
        JsonToken t = peek();
        if (t != JsonToken.STRING && t != JsonToken.NUMBER) throw new IllegalStateException("Expected a double but was " + t);
        double d = Double.parseDouble(value);
        token = null;
        return d;
    }
    public long nextLong() throws IOException {
        JsonToken t = peek();
        if (t != JsonToken.STRING && t != JsonToken.NUMBER) throw new IllegalStateException("Expected a long but was " + t);
        long l;
        try { l = Long.parseLong(value); }
        catch (NumberFormatException e) {
            double d = Double.parseDouble(value);
            l = (long) d;
            if (l != d) throw new NumberFormatException(value);
        }
        token = null;
        return l;
    }
    public int nextInt() throws IOException {
        long l = nextLong();
        if ((int) l != l) throw new NumberFormatException(value);
        return (int) l;
    }
    public void skipValue() throws IOException {
        int depth = 0;
        do {
            JsonToken t = peek();
            token = null;
            if (t == JsonToken.BEGIN_ARRAY || t == JsonToken.BEGIN_OBJECT) ++depth;
            else if (t == JsonToken.END_ARRAY || t == JsonToken.END_OBJECT) --depth;
        } while (depth > 0);
    }
    @Override public void close() throws IOException {
        token = null;
        stack.clear();
        stack.add(Scope.CLOSED);
        in.close();
    }
}
