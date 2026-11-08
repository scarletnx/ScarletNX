// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.text;

import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.text.style.CharacterStyle;
import android.text.style.ForegroundColorSpan;
import android.text.style.ImageSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StrikethroughSpan;
import android.text.style.StyleSpan;
import android.text.style.SubscriptSpan;
import android.text.style.SuperscriptSpan;
import android.text.style.TypefaceSpan;
import android.text.style.URLSpan;
import android.text.style.UnderlineSpan;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Styled text from HTML, and HTML from styled text. The parser reads the tags that Android's documentation lists. A
 * TagHandler gets the other tags, with a null XMLReader.
 */
public class Html {
    public static final int TO_HTML_PARAGRAPH_LINES_CONSECUTIVE = 0, TO_HTML_PARAGRAPH_LINES_INDIVIDUAL = 1;
    public static final int FROM_HTML_SEPARATOR_LINE_BREAK_PARAGRAPH = 1, FROM_HTML_SEPARATOR_LINE_BREAK_HEADING = 2,
        FROM_HTML_SEPARATOR_LINE_BREAK_LIST_ITEM = 4, FROM_HTML_SEPARATOR_LINE_BREAK_LIST = 8,
        FROM_HTML_SEPARATOR_LINE_BREAK_DIV = 16, FROM_HTML_SEPARATOR_LINE_BREAK_BLOCKQUOTE = 32,
        FROM_HTML_OPTION_USE_CSS_COLORS = 256, FROM_HTML_MODE_LEGACY = 0, FROM_HTML_MODE_COMPACT = 63;

    public interface ImageGetter {
        Drawable getDrawable(String source);
    }

    public interface TagHandler {
        void handleTag(boolean opening, String tag, Editable output, org.xml.sax.XMLReader xmlReader);
    }

    private static final float[] HEADING_SIZES = {1.5f, 1.4f, 1.3f, 1.2f, 1.1f, 1f};
    private static final Pattern ATTR = Pattern.compile("([A-Za-z_:][-\\w:.]*)\\s*(?:=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+)))?");
    private static final Pattern ENTITY = Pattern.compile("&(#[0-9]+|#[xX][0-9a-fA-F]+|[A-Za-z]+);");

    private Html() {}

    @Deprecated public static Spanned fromHtml(String source) { return fromHtml(source, FROM_HTML_MODE_LEGACY, null, null); }
    public static Spanned fromHtml(String source, int flags) { return fromHtml(source, flags, null, null); }
    @Deprecated public static Spanned fromHtml(String source, ImageGetter imageGetter, TagHandler tagHandler) {
        return fromHtml(source, FROM_HTML_MODE_LEGACY, imageGetter, tagHandler);
    }

    private static final class Open {
        final String tag;
        final int start;
        final Map<String, String> attrs;

        Open(String tag, int start, Map<String, String> attrs) { this.tag = tag; this.start = start; this.attrs = attrs; }
    }

    public static Spanned fromHtml(String source, int flags, ImageGetter imageGetter, TagHandler tagHandler) {
        SpannableStringBuilder out = new SpannableStringBuilder();
        ArrayList<Open> open = new ArrayList<>();
        int i = 0, n = source.length();
        while (i < n) {
            char c = source.charAt(i);
            if (c == '<') {
                if (source.startsWith("<!--", i)) {
                    int e = source.indexOf("-->", i + 4);
                    i = e < 0 ? n : e + 3;
                    continue;
                }
                int e = source.indexOf('>', i);
                if (e < 0) e = n;
                String body = source.substring(i + 1, e).trim();
                i = e + 1;
                boolean closing = body.startsWith("/");
                if (closing) body = body.substring(1);
                boolean selfClosing = body.endsWith("/");
                if (selfClosing) body = body.substring(0, body.length() - 1);
                int sp = 0;
                while (sp < body.length() && !Character.isWhitespace(body.charAt(sp))) ++sp;
                String tag = body.substring(0, sp).toLowerCase(java.util.Locale.ROOT);
                if (tag.isEmpty() || tag.startsWith("!") || tag.startsWith("?")) continue;
                if (closing) {
                    end(out, open, tag, flags, tagHandler);
                } else {
                    Map<String, String> attrs = new HashMap<>();
                    Matcher m = ATTR.matcher(body.substring(sp));
                    while (m.find()) {
                        String v = m.group(2) != null ? m.group(2) : m.group(3) != null ? m.group(3) : m.group(4);
                        attrs.put(m.group(1).toLowerCase(java.util.Locale.ROOT), v != null ? decode(v) : "");
                    }
                    start(out, open, tag, attrs, flags, imageGetter, tagHandler);
                    if (selfClosing) end(out, open, tag, flags, tagHandler);
                }
                continue;
            }
            int e = source.indexOf('<', i);
            if (e < 0) e = n;
            String text = decode(source.substring(i, e));
            i = e;
            for (int k = 0; k < text.length(); ++k) {
                char t = text.charAt(k);
                if (t == ' ' || t == '\n' || t == '\t' || t == '\r') {
                    int len = out.length();
                    char last = len == 0 ? '\n' : out.charAt(len - 1);
                    if (last != ' ' && last != '\n') out.append(' ');
                } else {
                    out.append(t);
                }
            }
        }
        return out;
    }

    private static int separator(String tag, int flags) {
        int bit;
        switch (tag) {
            case "p": bit = FROM_HTML_SEPARATOR_LINE_BREAK_PARAGRAPH; break;
            case "div": bit = FROM_HTML_SEPARATOR_LINE_BREAK_DIV; break;
            case "li": bit = FROM_HTML_SEPARATOR_LINE_BREAK_LIST_ITEM; break;
            case "ul": case "ol": bit = FROM_HTML_SEPARATOR_LINE_BREAK_LIST; break;
            case "blockquote": bit = FROM_HTML_SEPARATOR_LINE_BREAK_BLOCKQUOTE; break;
            default:
                if (tag.length() == 2 && tag.charAt(0) == 'h' && tag.charAt(1) >= '1' && tag.charAt(1) <= '6') {
                    bit = FROM_HTML_SEPARATOR_LINE_BREAK_HEADING;
                    break;
                }
                return 0;
        }
        return (flags & bit) != 0 ? 1 : 2;
    }
    /** Ends the text with at least count line feeds, unless it is empty. */
    private static void newlines(SpannableStringBuilder out, int count) {
        int len = out.length();
        if (len == 0) return;
        int have = 0;
        while (have < len && out.charAt(len - 1 - have) == '\n') ++have;
        if (have == 0 && out.charAt(len - 1) == ' ') out.delete(len - 1, len);
        for (; have < count; ++have) out.append('\n');
    }

    private static void start(SpannableStringBuilder out, ArrayList<Open> open, String tag, Map<String, String> attrs, int flags,
                              ImageGetter getter, TagHandler handler) {
        int sep = separator(tag, flags);
        if (sep > 0) newlines(out, sep);
        switch (tag) {
            case "br":
                out.append('\n');
                return;
            case "img": {
                String src = attrs.get("src");
                Drawable d = getter != null ? getter.getDrawable(src) : null;
                if (d == null) d = new ColorDrawable(0);
                int len = out.length();
                out.append('\uFFFC');
                out.setSpan(new ImageSpan(d, src), len, len + 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                return;
            }
            case "p": case "div": case "ul": case "ol": case "li": case "blockquote": case "b": case "strong": case "i": case "em":
            case "cite": case "dfn": case "u": case "ins": case "s": case "strike": case "del": case "sub": case "sup": case "big":
            case "small": case "tt": case "a": case "font": case "span": case "h1": case "h2": case "h3": case "h4": case "h5": case "h6":
                open.add(new Open(tag, out.length(), attrs));
                return;
            default:
                if (handler != null) handler.handleTag(true, tag, out, null);
        }
    }

    private static void end(SpannableStringBuilder out, ArrayList<Open> open, String tag, int flags, TagHandler handler) {
        if (tag.equals("br") || tag.equals("img")) return;
        int at = open.size() - 1;
        while (at >= 0 && !open.get(at).tag.equals(tag)) --at;
        if (at < 0) {
            if (handler != null && separator(tag, flags) == 0) handler.handleTag(false, tag, out, null);
            return;
        }
        Open o = open.remove(at);
        int sep = separator(tag, flags);
        if (sep > 0) newlines(out, sep);
        int s = o.start, e = out.length();
        if (s == e) return;
        switch (tag) {
            case "b": case "strong": span(out, new StyleSpan(Typeface.BOLD), s, e); break;
            case "i": case "em": case "cite": case "dfn": span(out, new StyleSpan(Typeface.ITALIC), s, e); break;
            case "u": case "ins": span(out, new UnderlineSpan(), s, e); break;
            case "s": case "strike": case "del": span(out, new StrikethroughSpan(), s, e); break;
            case "sub": span(out, new SubscriptSpan(), s, e); break;
            case "sup": span(out, new SuperscriptSpan(), s, e); break;
            case "big": span(out, new RelativeSizeSpan(1.25f), s, e); break;
            case "small": span(out, new RelativeSizeSpan(0.8f), s, e); break;
            case "tt": span(out, new TypefaceSpan("monospace"), s, e); break;
            case "a": if (o.attrs.get("href") != null) span(out, new URLSpan(o.attrs.get("href")), s, e); break;
            case "font": {
                String face = o.attrs.get("face");
                if (face != null && !face.isEmpty()) span(out, new TypefaceSpan(face), s, e);
                color(out, o.attrs.get("color"), s, e);
                break;
            }
            case "span": case "p": case "div": {
                String style = o.attrs.get("style");
                Matcher m = style != null ? Pattern.compile("(?:^|;)\\s*color\\s*:\\s*([^;]+)").matcher(style) : null;
                if (m != null && m.find()) color(out, m.group(1).trim(), s, e);
                break;
            }
            default:
                if (tag.length() == 2 && tag.charAt(0) == 'h') {
                    int end = e;
                    while (end > s && out.charAt(end - 1) == '\n') --end;
                    if (end > s) {
                        span(out, new RelativeSizeSpan(HEADING_SIZES[tag.charAt(1) - '1']), s, end);
                        span(out, new StyleSpan(Typeface.BOLD), s, end);
                    }
                }
        }
    }
    private static void span(SpannableStringBuilder out, Object what, int start, int end) { out.setSpan(what, start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE); }
    private static void color(SpannableStringBuilder out, String c, int start, int end) {
        if (c == null || c.isEmpty()) return;
        try {
            span(out, new ForegroundColorSpan(Color.parseColor(c) | 0xff000000), start, end);
        } catch (IllegalArgumentException e) {
            /* a color that the parser does not know: no span, as on Android */
        }
    }
    private static String decode(String s) {
        if (s.indexOf('&') < 0) return s;
        Matcher m = ENTITY.matcher(s);
        StringBuilder b = new StringBuilder();
        int last = 0;
        while (m.find()) {
            b.append(s, last, m.start());
            String name = m.group(1);
            String r;
            try {
                if (name.startsWith("#x") || name.startsWith("#X")) r = new String(Character.toChars(Integer.parseInt(name.substring(2), 16)));
                else if (name.startsWith("#")) r = new String(Character.toChars(Integer.parseInt(name.substring(1))));
                else r = switch (name) {
                    case "lt" -> "<";
                    case "gt" -> ">";
                    case "amp" -> "&";
                    case "quot" -> "\"";
                    case "apos" -> "'";
                    case "nbsp" -> "\u00a0";
                    case "copy" -> "\u00a9";
                    case "reg" -> "\u00ae";
                    default -> m.group();
                };
            } catch (IllegalArgumentException e) {
                r = m.group();
            }
            b.append(r);
            last = m.end();
        }
        return b.append(s, last, s.length()).toString();
    }

    @Deprecated public static String toHtml(Spanned text) { return toHtml(text, TO_HTML_PARAGRAPH_LINES_CONSECUTIVE); }
    public static String toHtml(Spanned text, int option) {
        StringBuilder out = new StringBuilder();
        int len = text.length();
        if ((option & TO_HTML_PARAGRAPH_LINES_INDIVIDUAL) != 0) {
            for (int i = 0; i <= len; ) {
                int nl = TextUtils.indexOf(text, '\n', i, len);
                int end = nl < 0 ? len : nl;
                if (end == i) {
                    if (nl >= 0) out.append("<br>\n");
                } else {
                    out.append("<p dir=\"ltr\">");
                    withinStyle(out, text, i, end);
                    out.append("</p>\n");
                }
                if (nl < 0) break;
                i = nl + 1;
            }
            return out.toString();
        }
        out.append("<p dir=\"ltr\">");
        for (int i = 0; i < len; ) {
            int nl = TextUtils.indexOf(text, '\n', i, len);
            int end = nl < 0 ? len : nl;
            withinStyle(out, text, i, end);
            if (nl < 0) break;
            int count = 0;
            while (end < len && text.charAt(end) == '\n') { ++count; ++end; }
            if (count == 1) {
                out.append("<br>\n");
            } else if (end == len) {
                for (int k = 2; k < count; ++k) out.append("<br>");
                return out.append("</p>\n").toString();
            } else {
                out.append("</p>\n");
                for (int k = 2; k < count; ++k) out.append("<br>");
                out.append("<p dir=\"ltr\">");
            }
            i = end;
        }
        return out.append("</p>\n").toString();
    }

    private static void withinStyle(StringBuilder out, Spanned text, int start, int end) {
        for (int i = start, next; i < end; i = next) {
            next = text.nextSpanTransition(i, end, CharacterStyle.class);
            CharacterStyle[] spans = text.getSpans(i, next, CharacterStyle.class);
            ArrayList<String> closes = new ArrayList<>();
            boolean replaced = false;
            for (CharacterStyle s : spans) {
                if (s instanceof StyleSpan st) {
                    if ((st.getStyle() & Typeface.BOLD) != 0) { out.append("<b>"); closes.add("</b>"); }
                    if ((st.getStyle() & Typeface.ITALIC) != 0) { out.append("<i>"); closes.add("</i>"); }
                } else if (s instanceof TypefaceSpan t && "monospace".equals(t.getFamily())) {
                    out.append("<tt>"); closes.add("</tt>");
                } else if (s instanceof SuperscriptSpan) {
                    out.append("<sup>"); closes.add("</sup>");
                } else if (s instanceof SubscriptSpan) {
                    out.append("<sub>"); closes.add("</sub>");
                } else if (s instanceof UnderlineSpan) {
                    out.append("<u>"); closes.add("</u>");
                } else if (s instanceof StrikethroughSpan) {
                    out.append("<span style=\"text-decoration:line-through;\">"); closes.add("</span>");
                } else if (s instanceof URLSpan u) {
                    out.append("<a href=\"").append(u.getURL()).append("\">"); closes.add("</a>");
                } else if (s instanceof ForegroundColorSpan f) {
                    out.append(String.format("<span style=\"color:#%06X;\">", f.getForegroundColor() & 0xffffff)); closes.add("</span>");
                } else if (s instanceof ImageSpan img) {
                    out.append("<img src=\"").append(img.getSource()).append("\">");
                    replaced = true;
                }
            }
            if (!replaced) escape(out, text, i, next);
            for (int k = closes.size() - 1; k >= 0; --k) out.append(closes.get(k));
        }
    }

    public static String escapeHtml(CharSequence text) {
        StringBuilder out = new StringBuilder();
        escape(out, text, 0, text.length());
        return out.toString();
    }
    private static void escape(StringBuilder out, CharSequence text, int start, int end) {
        for (int i = start; i < end; ++i) {
            char c = text.charAt(i);
            if (c == '<') out.append("&lt;");
            else if (c == '>') out.append("&gt;");
            else if (c == '&') out.append("&amp;");
            else if (Character.isHighSurrogate(c) && i + 1 < end && Character.isLowSurrogate(text.charAt(i + 1)))
                out.append("&#").append(Character.toCodePoint(c, text.charAt(++i))).append(';');
            else if (c > 0x7e || c < ' ') out.append("&#").append((int) c).append(';');
            else if (c == ' ') {
                while (i + 1 < end && text.charAt(i + 1) == ' ') { out.append("&nbsp;"); ++i; }
                out.append(' ');
            } else out.append(c);
        }
    }
}
