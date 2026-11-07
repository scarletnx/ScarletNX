// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.IOException;
import java.io.Writer;
import java.util.ArrayList;

/** Writes one JSON value as a stream: compact, or indented after setIndent. */
public final class JsonWriter implements java.io.Closeable {
    private enum Scope { EMPTY_ARRAY, NONEMPTY_ARRAY, EMPTY_OBJECT, DANGLING_NAME, NONEMPTY_OBJECT, EMPTY_DOCUMENT, NONEMPTY_DOCUMENT, CLOSED }

    private final Writer out;
    private final ArrayList<Scope> stack = new ArrayList<>();
    private String indent, separator = ":";
    private boolean lenient;

    public JsonWriter(Writer out) {
        if (out == null) throw new NullPointerException("out == null");
        this.out = out;
        stack.add(Scope.EMPTY_DOCUMENT);
    }
    public void setIndent(String indent) {
        this.indent = indent.isEmpty() ? null : indent;
        separator = indent.isEmpty() ? ":" : ": ";
    }
    public void setLenient(boolean lenient) { this.lenient = lenient; }
    public boolean isLenient() { return lenient; }

    public JsonWriter beginArray() throws IOException { return open(Scope.EMPTY_ARRAY, "["); }
    public JsonWriter endArray() throws IOException { return close(Scope.EMPTY_ARRAY, Scope.NONEMPTY_ARRAY, "]"); }
    public JsonWriter beginObject() throws IOException { return open(Scope.EMPTY_OBJECT, "{"); }
    public JsonWriter endObject() throws IOException { return close(Scope.EMPTY_OBJECT, Scope.NONEMPTY_OBJECT, "}"); }

    private JsonWriter open(Scope empty, String bracket) throws IOException {
        beforeValue(true);
        stack.add(empty);
        out.write(bracket);
        return this;
    }
    private JsonWriter close(Scope empty, Scope nonempty, String bracket) throws IOException {
        Scope context = peek();
        if (context != nonempty && context != empty) throw new IllegalStateException("Nesting problem: " + stack);
        stack.remove(stack.size() - 1);
        if (context == nonempty) newline();
        out.write(bracket);
        return this;
    }
    private Scope peek() {
        if (stack.isEmpty()) throw new IllegalStateException("JsonWriter is closed.");
        return stack.get(stack.size() - 1);
    }
    private void replaceTop(Scope s) { stack.set(stack.size() - 1, s); }

    public JsonWriter name(String name) throws IOException {
        if (name == null) throw new NullPointerException("name == null");
        Scope context = peek();
        if (context == Scope.NONEMPTY_OBJECT) out.write(',');
        else if (context != Scope.EMPTY_OBJECT) throw new IllegalStateException("Nesting problem: " + stack);
        newline();
        replaceTop(Scope.DANGLING_NAME);
        string(name);
        return this;
    }
    public JsonWriter value(String value) throws IOException {
        if (value == null) return nullValue();
        beforeValue(false);
        string(value);
        return this;
    }
    public JsonWriter nullValue() throws IOException { beforeValue(false); out.write("null"); return this; }
    public JsonWriter value(boolean value) throws IOException { beforeValue(false); out.write(value ? "true" : "false"); return this; }
    public JsonWriter value(double value) throws IOException {
        if (!lenient && (Double.isNaN(value) || Double.isInfinite(value))) throw new IllegalArgumentException("Numeric values must be finite, but was " + value);
        beforeValue(false);
        out.append(Double.toString(value));
        return this;
    }
    public JsonWriter value(long value) throws IOException { beforeValue(false); out.write(Long.toString(value)); return this; }
    public JsonWriter value(Number value) throws IOException {
        if (value == null) return nullValue();
        String string = value.toString();
        if (!lenient && (string.equals("-Infinity") || string.equals("Infinity") || string.equals("NaN")))
            throw new IllegalArgumentException("Numeric values must be finite, but was " + value);
        beforeValue(false);
        out.append(string);
        return this;
    }
    public void flush() throws IOException { out.flush(); }
    @Override public void close() throws IOException {
        out.close();
        if (peek() != Scope.NONEMPTY_DOCUMENT) throw new IOException("Incomplete document");
        stack.clear();
    }

    private void string(String value) throws IOException {
        out.write('"');
        for (int i = 0; i < value.length(); ++i) {
            char c = value.charAt(i);
            switch (c) {
                case '"': case '\\': out.write('\\'); out.write(c); break;
                case '\t': out.write("\\t"); break;
                case '\b': out.write("\\b"); break;
                case '\n': out.write("\\n"); break;
                case '\r': out.write("\\r"); break;
                case '\f': out.write("\\f"); break;
                case '\u2028': case '\u2029': out.write(String.format("\\u%04x", (int) c)); break;
                default:
                    if (c <= 0x1f) out.write(String.format("\\u%04x", (int) c));
                    else out.write(c);
            }
        }
        out.write('"');
    }
    private void newline() throws IOException {
        if (indent == null) return;
        out.write("\n");
        for (int i = 1; i < stack.size(); ++i) out.write(indent);
    }
    private void beforeValue(boolean root) throws IOException {
        switch (peek()) {
            case EMPTY_DOCUMENT -> {
                if (!lenient && !root) throw new IllegalStateException("JSON must start with an array or an object.");
                replaceTop(Scope.NONEMPTY_DOCUMENT);
            }
            case EMPTY_ARRAY -> { replaceTop(Scope.NONEMPTY_ARRAY); newline(); }
            case NONEMPTY_ARRAY -> { out.append(','); newline(); }
            case DANGLING_NAME -> { out.append(separator); replaceTop(Scope.NONEMPTY_OBJECT); }
            case NONEMPTY_DOCUMENT -> throw new IllegalStateException("JSON must have only one top-level value.");
            default -> throw new IllegalStateException("Nesting problem: " + stack);
        }
    }
}
