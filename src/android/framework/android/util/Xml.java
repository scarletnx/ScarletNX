// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.io.BufferedWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Reader;
import java.io.Writer;
import java.util.ArrayList;
import javax.xml.stream.XMLInputFactory;
import javax.xml.stream.XMLStreamConstants;
import javax.xml.stream.XMLStreamException;
import javax.xml.stream.XMLStreamReader;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.xmlpull.v1.XmlSerializer;

/**
 * XML helpers. A compiled parser is already its own AttributeSet. A text XML parser is the JDK's StAX reader behind
 * the pull parser API, with namespaces on, as Android makes it. A serializer writes the text itself.
 */
public class Xml {
    public static String FEATURE_RELAXED = "http://xmlpull.org/v1/doc/features.html#relaxed";

    public static AttributeSet asAttributeSet(XmlPullParser parser) {
        if (parser instanceof AttributeSet a) return a;
        throw new UnsupportedOperationException("snx: Xml.asAttributeSet of a text XML parser");
    }

    public static XmlPullParser newPullParser() { return new TextParser(); }

    public static XmlSerializer newSerializer() { return new TextSerializer(); }

    private static final class TextParser implements XmlPullParser {
        private XMLStreamReader reader;
        private String encoding, text;
        private int event = START_DOCUMENT, depth;
        private boolean namespaces = true;

        @Override public void setFeature(String name, boolean state) throws XmlPullParserException {
            if (FEATURE_PROCESS_NAMESPACES.equals(name)) {
                if (event != START_DOCUMENT || reader != null) throw new XmlPullParserException("namespace processing feature can only be changed before parsing");
                namespaces = state;
            } else if (FEATURE_VALIDATION.equals(name) && state) {
                throw new XmlPullParserException("unsupported feature: " + name);
            }
        }
        @Override public boolean getFeature(String name) {
            return FEATURE_PROCESS_NAMESPACES.equals(name) ? namespaces : FEATURE_PROCESS_DOCDECL.equals(name);
        }
        @Override public void setProperty(String name, Object value) throws XmlPullParserException {
            throw new XmlPullParserException("unsupported property: " + name);
        }
        @Override public Object getProperty(String name) { return null; }

        private XMLInputFactory factory() {
            XMLInputFactory f = XMLInputFactory.newFactory();
            f.setProperty(XMLInputFactory.IS_NAMESPACE_AWARE, namespaces);
            f.setProperty(XMLInputFactory.IS_COALESCING, true);
            f.setProperty(XMLInputFactory.IS_SUPPORTING_EXTERNAL_ENTITIES, false);
            f.setProperty(XMLInputFactory.IS_REPLACING_ENTITY_REFERENCES, true);
            return f;
        }

        private void reset() { event = START_DOCUMENT; depth = 0; text = null; }

        @Override public void setInput(Reader in) throws XmlPullParserException {
            reset();
            try {
                reader = in != null ? factory().createXMLStreamReader(in) : null;
                encoding = null;
            } catch (XMLStreamException e) {
                throw new XmlPullParserException(e.getMessage(), this, e);
            }
        }
        @Override public void setInput(InputStream in, String inputEncoding) throws XmlPullParserException {
            if (in == null) throw new IllegalArgumentException("input stream can not be null");
            reset();
            try {
                reader = inputEncoding != null ? factory().createXMLStreamReader(in, inputEncoding) : factory().createXMLStreamReader(in);
                encoding = inputEncoding != null ? inputEncoding : reader.getEncoding();
            } catch (XMLStreamException e) {
                throw new XmlPullParserException(e.getMessage(), this, e);
            }
        }
        @Override public String getInputEncoding() { return encoding; }
        @Override public void defineEntityReplacementText(String entityName, String replacementText) {}

        private boolean onTag() { return event == START_TAG || event == END_TAG; }

        @Override public int getNamespaceCount(int d) { return event == START_TAG && d == depth ? reader.getNamespaceCount() : 0; }
        @Override public String getNamespacePrefix(int pos) { return reader.getNamespacePrefix(pos); }
        @Override public String getNamespaceUri(int pos) { return reader.getNamespaceURI(pos); }
        @Override public String getNamespace(String prefix) { return onTag() ? reader.getNamespaceURI(prefix) : null; }
        @Override public int getDepth() { return depth; }
        @Override public int getLineNumber() { return reader != null ? reader.getLocation().getLineNumber() : -1; }
        @Override public int getColumnNumber() { return reader != null ? reader.getLocation().getColumnNumber() : -1; }
        @Override public String getPositionDescription() {
            return TYPES[event] + (onTag() ? " <" + getName() + ">" : "") + " @" + getLineNumber() + ":" + getColumnNumber();
        }

        @Override public boolean isWhitespace() throws XmlPullParserException {
            if (event != TEXT && event != IGNORABLE_WHITESPACE && event != CDSECT) throw new XmlPullParserException("no content available to check for whitespaces");
            for (int i = 0; i < text.length(); ++i) {
                char c = text.charAt(i);
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
            }
            return true;
        }
        @Override public String getText() { return event == TEXT ? text : null; }
        @Override public char[] getTextCharacters(int[] holder) {
            if (event != TEXT) { holder[0] = -1; holder[1] = -1; return null; }
            holder[0] = 0;
            holder[1] = text.length();
            return text.toCharArray();
        }

        @Override public String getNamespace() {
            if (!onTag()) return null;
            String ns = reader.getNamespaceURI();
            return ns != null ? ns : NO_NAMESPACE;
        }
        @Override public String getName() {
            if (!onTag()) return null;
            if (namespaces) return reader.getLocalName();
            String prefix = reader.getPrefix();
            return prefix != null && !prefix.isEmpty() ? prefix + ":" + reader.getLocalName() : reader.getLocalName();
        }
        @Override public String getPrefix() {
            if (!onTag()) return null;
            String p = reader.getPrefix();
            return p != null && !p.isEmpty() ? p : null;
        }
        @Override public boolean isEmptyElementTag() throws XmlPullParserException {
            if (event != START_TAG) throw new XmlPullParserException("parser must be on START_TAG to check for empty element");
            return false;
        }

        @Override public int getAttributeCount() { return event == START_TAG ? reader.getAttributeCount() : -1; }
        @Override public String getAttributeNamespace(int index) {
            String ns = reader.getAttributeNamespace(index);
            return ns != null ? ns : NO_NAMESPACE;
        }
        @Override public String getAttributeName(int index) { return reader.getAttributeLocalName(index); }
        @Override public String getAttributePrefix(int index) {
            String p = reader.getAttributePrefix(index);
            return p != null && !p.isEmpty() ? p : null;
        }
        @Override public String getAttributeType(int index) { return "CDATA"; }
        @Override public boolean isAttributeDefault(int index) { return false; }
        @Override public String getAttributeValue(int index) { return reader.getAttributeValue(index); }
        @Override public String getAttributeValue(String namespace, String name) {
            if (event != START_TAG) throw new IndexOutOfBoundsException("only START_TAG can have attributes");
            for (int i = 0; i < reader.getAttributeCount(); ++i) {
                if (!name.equals(reader.getAttributeLocalName(i))) continue;
                if (namespace == null || namespace.equals(getAttributeNamespace(i))) return reader.getAttributeValue(i);
            }
            return null;
        }
        @Override public int getEventType() { return event; }

        @Override public int next() throws XmlPullParserException, IOException {
            if (reader == null) throw new XmlPullParserException("setInput() must be called first.");
            if (event == END_TAG) --depth;
            text = null;
            try {
                while (true) {
                    if (!reader.hasNext()) return event = END_DOCUMENT;
                    switch (reader.next()) {
                        case XMLStreamConstants.START_ELEMENT -> { ++depth; return event = START_TAG; }
                        case XMLStreamConstants.END_ELEMENT -> { return event = END_TAG; }
                        case XMLStreamConstants.CHARACTERS, XMLStreamConstants.CDATA, XMLStreamConstants.SPACE -> {
                            text = reader.getText();
                            return event = TEXT;
                        }
                        case XMLStreamConstants.END_DOCUMENT -> { return event = END_DOCUMENT; }
                        default -> {}
                    }
                }
            } catch (XMLStreamException e) {
                throw new XmlPullParserException(e.getMessage(), this, e);
            }
        }
        @Override public int nextToken() throws XmlPullParserException, IOException { return next(); }

        @Override public void require(int type, String namespace, String name) throws XmlPullParserException {
            if (type != event || (namespace != null && !namespace.equals(getNamespace())) || (name != null && !name.equals(getName()))) {
                throw new XmlPullParserException("expected " + TYPES[type] + " " + getPositionDescription());
            }
        }
        @Override public String nextText() throws XmlPullParserException, IOException {
            if (event != START_TAG) throw new XmlPullParserException("parser must be on START_TAG to read next text", this, null);
            int e = next();
            if (e == TEXT) {
                String result = text;
                if (next() != END_TAG) throw new XmlPullParserException("TEXT must be immediately followed by END_TAG and not " + TYPES[event], this, null);
                return result;
            }
            if (e == END_TAG) return "";
            throw new XmlPullParserException("parser must be on START_TAG or TEXT to read text", this, null);
        }
        @Override public int nextTag() throws XmlPullParserException, IOException {
            next();
            if (event == TEXT && isWhitespace()) next();
            if (event != START_TAG && event != END_TAG) throw new XmlPullParserException("expected start or end tag", this, null);
            return event;
        }
    }

    private static final class TextSerializer implements XmlSerializer {
        private static final String INDENT = "http://xmlpull.org/v1/doc/features.html#indent-output";

        private Writer out;
        private boolean indent, open, afterText, started;
        private int depth, generated;
        private final ArrayList<String[]> elements = new ArrayList<>();
        private final ArrayList<String[]> bindings = new ArrayList<>();
        private final ArrayList<String[]> pending = new ArrayList<>();

        @Override public void setFeature(String name, boolean state) {
            if (!INDENT.equals(name)) throw new IllegalArgumentException("unsupported feature: " + name);
            indent = state;
        }
        @Override public boolean getFeature(String name) { return INDENT.equals(name) && indent; }
        @Override public void setProperty(String name, Object value) { throw new IllegalArgumentException("unsupported property: " + name); }
        @Override public Object getProperty(String name) { return null; }

        @Override public void setOutput(OutputStream os, String encoding) {
            if (os == null) throw new IllegalArgumentException("output stream can not be null");
            setOutput(new BufferedWriter(new OutputStreamWriter(os, java.nio.charset.Charset.forName(encoding != null ? encoding : "UTF-8"))));
        }
        @Override public void setOutput(Writer writer) {
            out = writer;
            depth = 0;
            open = afterText = started = false;
            elements.clear();
            bindings.clear();
            pending.clear();
        }

        @Override public void startDocument(String encoding, Boolean standalone) throws IOException {
            out.write("<?xml version='1.0' encoding='" + (encoding != null ? encoding : "UTF-8") + "'"
                + (standalone != null ? " standalone='" + (standalone ? "yes" : "no") + "'" : "") + " ?>");
            started = true;
        }
        @Override public void endDocument() throws IOException {
            while (!elements.isEmpty()) {
                String[] e = elements.get(elements.size() - 1);
                endTag(e[0], e[1]);
            }
            flush();
        }

        @Override public void setPrefix(String prefix, String namespace) { pending.add(new String[] { prefix != null ? prefix : "", namespace }); }

        private String find(String namespace) {
            for (int i = pending.size() - 1; i >= 0; --i) if (pending.get(i)[1].equals(namespace)) return pending.get(i)[0];
            for (int i = bindings.size() - 1; i >= 0; --i) if (bindings.get(i)[1].equals(namespace)) return bindings.get(i)[0];
            return null;
        }

        @Override public String getPrefix(String namespace, boolean generatePrefix) {
            String p = find(namespace);
            if (p != null || !generatePrefix) return p;
            p = "n" + generated++;
            pending.add(new String[] { p, namespace });
            return p;
        }

        @Override public int getDepth() { return depth; }
        @Override public String getNamespace() { return elements.isEmpty() ? null : elements.get(elements.size() - 1)[0]; }
        @Override public String getName() { return elements.isEmpty() ? null : elements.get(elements.size() - 1)[1]; }

        private String qualify(String namespace, String name, boolean attribute) {
            if (namespace == null || namespace.isEmpty()) return name;
            String p = getPrefix(namespace, true);
            if (attribute && p.isEmpty()) {
                p = "n" + generated++;
                pending.add(new String[] { p, namespace });
            }
            return p.isEmpty() ? name : p + ":" + name;
        }

        private void writeBindings() throws IOException {
            for (String[] b : pending) {
                out.write(" xmlns" + (b[0].isEmpty() ? "" : ":" + b[0]) + "=\"");
                escape(b[1], true);
                out.write('"');
                bindings.add(new String[] { b[0], b[1], Integer.toString(depth) });
            }
            pending.clear();
        }

        private void closeStart() throws IOException {
            if (open) out.write('>');
            open = false;
        }

        private void newline() throws IOException {
            if (!indent || afterText || (!started && depth == 0)) return;
            out.write('\n');
            for (int i = 0; i < depth; ++i) out.write("  ");
        }

        @Override public XmlSerializer startTag(String namespace, String name) throws IOException {
            closeStart();
            newline();
            started = true;
            String q = qualify(namespace, name, false);
            out.write('<');
            out.write(q);
            ++depth;
            writeBindings();
            elements.add(new String[] { namespace, name, q });
            open = true;
            afterText = false;
            return this;
        }

        @Override public XmlSerializer attribute(String namespace, String name, String value) throws IOException {
            if (!open) throw new IllegalStateException("illegal position for attribute");
            String q = qualify(namespace, name, true);
            writeBindings();
            out.write(' ');
            out.write(q);
            out.write("=\"");
            escape(value, true);
            out.write('"');
            return this;
        }

        @Override public XmlSerializer endTag(String namespace, String name) throws IOException {
            if (elements.isEmpty()) throw new IllegalArgumentException("</" + name + "> does not close any element");
            String[] e = elements.remove(elements.size() - 1);
            --depth;
            if (open) {
                out.write(" />");
                open = false;
            } else {
                newline();
                out.write("</");
                out.write(e[2]);
                out.write('>');
            }
            final int d = depth;
            bindings.removeIf(b -> Integer.parseInt(b[2]) > d);
            afterText = false;
            return this;
        }

        @Override public XmlSerializer text(String text) throws IOException {
            closeStart();
            escape(text, false);
            afterText = true;
            return this;
        }
        @Override public XmlSerializer text(char[] buf, int start, int len) throws IOException { return text(new String(buf, start, len)); }
        @Override public void cdsect(String text) throws IOException { closeStart(); out.write("<![CDATA[" + text + "]]>"); afterText = true; }
        @Override public void entityRef(String text) throws IOException { closeStart(); out.write("&" + text + ";"); afterText = true; }
        @Override public void processingInstruction(String text) throws IOException { closeStart(); out.write("<?" + text + "?>"); }
        @Override public void comment(String text) throws IOException { closeStart(); out.write("<!--" + text + "-->"); }
        @Override public void docdecl(String text) throws IOException { out.write("<!DOCTYPE" + text + ">"); }
        @Override public void ignorableWhitespace(String text) throws IOException { closeStart(); out.write(text); }
        @Override public void flush() throws IOException { closeStart(); out.flush(); }

        private void escape(String s, boolean attribute) throws IOException {
            for (int i = 0; i < s.length(); ++i) {
                char c = s.charAt(i);
                switch (c) {
                    case '&' -> out.write("&amp;");
                    case '<' -> out.write("&lt;");
                    case '>' -> out.write("&gt;");
                    case '"' -> out.write(attribute ? "&quot;" : "\"");
                    case '\n', '\r', '\t' -> { if (attribute) out.write("&#" + (int) c + ";"); else out.write(c); }
                    default -> out.write(c);
                }
            }
        }
    }
}
