// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import java.io.InputStream;
import java.io.Reader;
import org.xmlpull.v1.XmlPullParserException;

/**
 * A compiled XML document, parsed by the runtime's HLE of libandroidfw's ResXMLTree (src/android/res/axml.c). Its
 * parsers hold their place natively; their strings come from the document's pool, each made once.
 */
public final class XmlBlock implements AutoCloseable {
    private long handle;
    private final String[] strings;

    public XmlBlock(byte[] data) {
        handle = nativeCreate(data);
        if (handle == 0) throw new IllegalArgumentException("not compiled XML");
        strings = new String[nativeStringCount(handle)];
    }

    public String string(int index) {
        if (index < 0 || index >= strings.length) return null;
        String s = strings[index];
        if (s == null) strings[index] = s = nativeGetString(handle, index);
        return s;
    }

    public XmlResourceParser newParser() {
        return new Parser(this);
    }

    @Override
    public synchronized void close() {
        if (handle != 0) nativeDestroy(handle);
        handle = 0;
    }

    /** A typed attribute value that is not a string, as text. */
    static String coerce(int type, int data) {
        return android.util.TypedValue.coerceToString(type, data);
    }

    public final class Parser implements XmlResourceParser {
        private final XmlBlock block;
        private long state;
        private int event = START_DOCUMENT;

        /** The native parse state, for the attribute retrieval of AssetManager. */
        long state() {
            return state;
        }

        XmlBlock block() {
            return block;
        }

        Parser(XmlBlock block) {
            this.block = block;
            state = nativeCreateParseState(block.handle);
        }

        @Override public void setFeature(String name, boolean value) throws XmlPullParserException {
            if (FEATURE_PROCESS_NAMESPACES.equals(name) && value) return;
            if (FEATURE_REPORT_NAMESPACE_ATTRIBUTES.equals(name) && value) return;
            throw new XmlPullParserException("unsupported feature: " + name);
        }
        @Override public boolean getFeature(String name) {
            return FEATURE_PROCESS_NAMESPACES.equals(name) || FEATURE_REPORT_NAMESPACE_ATTRIBUTES.equals(name);
        }
        @Override public void setProperty(String name, Object value) throws XmlPullParserException {
            throw new XmlPullParserException("setProperty() not supported");
        }
        @Override public Object getProperty(String name) { return null; }
        @Override public void setInput(Reader in) throws XmlPullParserException {
            throw new XmlPullParserException("setInput() not supported");
        }
        @Override public void setInput(InputStream in, String encoding) throws XmlPullParserException {
            throw new XmlPullParserException("setInput() not supported");
        }
        @Override public void defineEntityReplacementText(String name, String text) throws XmlPullParserException {
            throw new XmlPullParserException("defineEntityReplacementText() not supported");
        }
        @Override public String getNamespacePrefix(int pos) { throw new UnsupportedOperationException(); }
        @Override public String getInputEncoding() { return null; }
        @Override public String getNamespace(String prefix) { throw new UnsupportedOperationException(); }
        @Override public int getNamespaceCount(int depth) { throw new UnsupportedOperationException(); }
        @Override public String getPositionDescription() { return "Binary XML file line #" + getLineNumber(); }
        @Override public String getNamespaceUri(int pos) { throw new UnsupportedOperationException(); }
        @Override public int getColumnNumber() { return -1; }
        @Override public int getDepth() { return nativeGetDepth(state); }
        @Override public String getText() { return block.string(nativeGetText(state)); }
        @Override public int getLineNumber() { return nativeGetLineNumber(state); }
        @Override public int getEventType() { return event; }
        @Override public boolean isWhitespace() {
            String text = getText();
            return text == null || text.isBlank();
        }
        @Override public String getPrefix() { throw new UnsupportedOperationException(); }
        @Override public char[] getTextCharacters(int[] holder) {
            String text = getText();
            if (text == null) return null;
            holder[0] = 0;
            holder[1] = text.length();
            return text.toCharArray();
        }
        @Override public String getNamespace() {
            String ns = block.string(nativeGetNamespace(state));
            return ns != null ? ns : "";
        }
        @Override public String getName() { return block.string(nativeGetName(state)); }
        @Override public String getAttributeNamespace(int index) {
            String ns = block.string(nativeGetAttributeNamespace(state, index));
            return ns != null ? ns : "";
        }
        @Override public String getAttributeName(int index) {
            return block.string(nativeGetAttributeName(state, index));
        }
        @Override public String getAttributePrefix(int index) { throw new UnsupportedOperationException(); }
        @Override public boolean isEmptyElementTag() { return false; }
        @Override public int getAttributeCount() { return event == START_TAG ? nativeGetAttributeCount(state) : -1; }
        @Override public String getAttributeValue(int index) {
            String raw = block.string(nativeGetAttributeStringValue(state, index));
            if (raw != null) return raw;
            return coerce(nativeGetAttributeDataType(state, index), nativeGetAttributeData(state, index));
        }
        @Override public String getAttributeType(int index) { return "CDATA"; }
        @Override public boolean isAttributeDefault(int index) { return false; }
        @Override public int nextToken() throws XmlPullParserException { return next(); }
        @Override public String getAttributeValue(String namespace, String name) {
            int i = nativeGetAttributeIndex(state, namespace, name);
            return i >= 0 ? getAttributeValue(i) : null;
        }
        @Override public int next() throws XmlPullParserException {
            if (state == 0) return END_DOCUMENT;
            event = nativeNext(state);
            return event;
        }
        @Override public void require(int type, String namespace, String name) throws XmlPullParserException {
            if (type != event || (namespace != null && !namespace.equals(getNamespace()))
                || (name != null && !name.equals(getName())))
                throw new XmlPullParserException("expected " + TYPES[type] + " " + getPositionDescription());
        }
        @Override public String nextText() throws XmlPullParserException {
            if (event != START_TAG)
                throw new XmlPullParserException(getPositionDescription() + ": parser must be on START_TAG");
            int e = next();
            if (e == TEXT) {
                String result = getText();
                if (next() != END_TAG)
                    throw new XmlPullParserException(getPositionDescription() + ": text must be followed by END_TAG");
                return result;
            }
            if (e == END_TAG) return "";
            throw new XmlPullParserException(getPositionDescription() + ": parser must be on START_TAG or TEXT");
        }
        @Override public int nextTag() throws XmlPullParserException {
            int e = next();
            if (e == TEXT && isWhitespace()) e = next();
            if (e != START_TAG && e != END_TAG)
                throw new XmlPullParserException(getPositionDescription() + ": expected start or end tag", this, null);
            return e;
        }
        @Override public int getAttributeNameResource(int index) { return nativeGetAttributeResource(state, index); }
        @Override public int getAttributeListValue(String namespace, String attribute, String[] options, int def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeListValue(i, options, def) : def;
        }
        @Override public boolean getAttributeBooleanValue(String namespace, String attribute, boolean def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeBooleanValue(i, def) : def;
        }
        @Override public int getAttributeResourceValue(String namespace, String attribute, int def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeResourceValue(i, def) : def;
        }
        @Override public int getAttributeIntValue(String namespace, String attribute, int def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeIntValue(i, def) : def;
        }
        @Override public int getAttributeUnsignedIntValue(String namespace, String attribute, int def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeUnsignedIntValue(i, def) : def;
        }
        @Override public float getAttributeFloatValue(String namespace, String attribute, float def) {
            int i = nativeGetAttributeIndex(state, namespace, attribute);
            return i >= 0 ? getAttributeFloatValue(i, def) : def;
        }
        @Override public int getAttributeListValue(int index, String[] options, int def) {
            int type = nativeGetAttributeDataType(state, index);
            int data = nativeGetAttributeData(state, index);
            if (type == 0x03) {
                String value = block.string(nativeGetAttributeStringValue(state, index));
                for (int i = 0; options != null && i < options.length; ++i)
                    if (options[i].equals(value)) return i;
                return def;
            }
            return type >= 0x10 && type <= 0x1f ? data : def;
        }
        @Override public boolean getAttributeBooleanValue(int index, boolean def) {
            int type = nativeGetAttributeDataType(state, index);
            return type >= 0x10 && type <= 0x1f ? nativeGetAttributeData(state, index) != 0 : def;
        }
        @Override public int getAttributeResourceValue(int index, int def) {
            int type = nativeGetAttributeDataType(state, index);
            return type == 0x01 ? nativeGetAttributeData(state, index) : def;
        }
        @Override public int getAttributeIntValue(int index, int def) {
            int type = nativeGetAttributeDataType(state, index);
            return type >= 0x10 && type <= 0x1f ? nativeGetAttributeData(state, index) : def;
        }
        @Override public int getAttributeUnsignedIntValue(int index, int def) {
            return getAttributeIntValue(index, def);
        }
        @Override public float getAttributeFloatValue(int index, float def) {
            int type = nativeGetAttributeDataType(state, index);
            return type == 0x04 ? Float.intBitsToFloat(nativeGetAttributeData(state, index)) : def;
        }
        @Override public String getIdAttribute() {
            int i = nativeGetIdAttribute(state);
            return i >= 0 ? getAttributeValue(i) : null;
        }
        @Override public String getClassAttribute() {
            int i = nativeGetClassAttribute(state);
            return i >= 0 ? getAttributeValue(i) : null;
        }
        @Override public int getIdAttributeResourceValue(int def) {
            int i = nativeGetIdAttribute(state);
            return i >= 0 ? getAttributeResourceValue(i, def) : def;
        }
        @Override public int getStyleAttribute() {
            int i = nativeGetStyleAttribute(state);
            return i >= 0 ? getAttributeResourceValue(i, 0) : 0;
        }

        /** The typed value of an attribute: its type, in the data of a TypedValue. */
        public int getAttributeDataType(int index) { return nativeGetAttributeDataType(state, index); }

        public int getAttributeData(int index) { return nativeGetAttributeData(state, index); }

        @Override public synchronized void close() {
            if (state != 0) nativeDestroyParseState(state);
            state = 0;
        }
    }

    private static native long nativeCreate(byte[] data);
    private static native int nativeStringCount(long block);
    private static native String nativeGetString(long block, int index);
    private static native void nativeDestroy(long block);
    private static native long nativeCreateParseState(long block);
    private static native void nativeDestroyParseState(long state);
    private static native int nativeNext(long state);
    private static native int nativeGetDepth(long state);
    private static native int nativeGetLineNumber(long state);
    private static native int nativeGetNamespace(long state);
    private static native int nativeGetName(long state);
    private static native int nativeGetText(long state);
    private static native int nativeGetAttributeCount(long state);
    private static native int nativeGetAttributeNamespace(long state, int index);
    private static native int nativeGetAttributeName(long state, int index);
    private static native int nativeGetAttributeResource(long state, int index);
    private static native int nativeGetAttributeDataType(long state, int index);
    private static native int nativeGetAttributeData(long state, int index);
    private static native int nativeGetAttributeStringValue(long state, int index);
    private static native int nativeGetAttributeIndex(long state, String namespace, String name);
    private static native int nativeGetIdAttribute(long state);
    private static native int nativeGetClassAttribute(long state);
    private static native int nativeGetStyleAttribute(long state);
}
