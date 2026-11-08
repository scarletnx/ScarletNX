// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.text.style.CharacterStyle;
import android.text.style.LeadingMarginSpan;
import android.text.style.LineHeightSpan;
import android.text.style.MetricAffectingSpan;
import android.text.style.ReplacementSpan;
import java.text.BreakIterator;

/**
 * Text broken into lines, measured and drawn with its spans: greedy breaks at the line boundaries of
 * java.text.BreakIterator, each line as high as its tallest run, LineHeightSpan and leading margins applied, and the
 * last line ellipsized at the end when the lines run out. Left to right only.
 */
public abstract class Layout {
    public enum Alignment { ALIGN_NORMAL, ALIGN_OPPOSITE, ALIGN_CENTER, ALIGN_LEFT, ALIGN_RIGHT }

    public static final int DIR_LEFT_TO_RIGHT = 1, DIR_RIGHT_TO_LEFT = -1;
    public static final int BREAK_STRATEGY_SIMPLE = 0, BREAK_STRATEGY_HIGH_QUALITY = 1, BREAK_STRATEGY_BALANCED = 2;
    public static final int HYPHENATION_FREQUENCY_NONE = 0, HYPHENATION_FREQUENCY_NORMAL = 1, HYPHENATION_FREQUENCY_FULL = 2,
        HYPHENATION_FREQUENCY_NORMAL_FAST = 3, HYPHENATION_FREQUENCY_FULL_FAST = 4;
    public static final int JUSTIFICATION_MODE_NONE = 0, JUSTIFICATION_MODE_INTER_WORD = 1, JUSTIFICATION_MODE_INTER_CHARACTER = 2;
    public static final float DEFAULT_LINESPACING_MULTIPLIER = 1.0f, DEFAULT_LINESPACING_ADDITION = 0.0f;

    public interface TextInclusionStrategy {
        boolean isSegmentInside(RectF segmentBounds, RectF area);
    }

    public static final TextInclusionStrategy INCLUSION_STRATEGY_ANY_OVERLAP = RectF::intersects;
    public static final TextInclusionStrategy INCLUSION_STRATEGY_CONTAINS_CENTER = (s, a) -> a.contains(s.centerX(), s.centerY());
    public static final TextInclusionStrategy INCLUSION_STRATEGY_CONTAINS_ALL = (s, a) -> a.contains(s);

    private CharSequence text;
    private TextPaint paint;
    private int width;
    private Alignment alignment;
    private float spacingMult, spacingAdd;

    int lineCount;
    int[] starts = new int[2], tops = new int[2], ascents = new int[1], descents = new int[1], ellipsisStarts = new int[1],
        ellipsisCounts = new int[1], margins = new int[1];
    float[] lineWidths = new float[1];
    float[] advances = new float[0];
    int topPadding, bottomPadding;
    private int[] leftIndents, rightIndents;

    protected Layout(CharSequence text, TextPaint paint, int width, Alignment align, float spacingMult, float spacingAdd) {
        if (width < 0) throw new IllegalArgumentException("Layout: " + width + " < 0");
        this.text = text;
        this.paint = paint;
        this.width = width;
        alignment = align != null ? align : Alignment.ALIGN_NORMAL;
        this.spacingMult = spacingMult;
        this.spacingAdd = spacingAdd;
    }

    // ---- measuring ----

    private static TextPaint measuring(TextPaint base, Spanned sp, int start, int end, boolean draw) {
        TextPaint wp = new TextPaint();
        wp.set(base);
        if (sp == null) return wp;
        for (MetricAffectingSpan s : covering(sp, start, end, MetricAffectingSpan.class)) s.updateMeasureState(wp);
        if (draw) for (CharacterStyle s : covering(sp, start, end, CharacterStyle.class)) s.updateDrawState(wp);
        return wp;
    }
    /**
     * The spans of a type over some of a run's text. getSpans also gives the empty spans at its ends and the spans
     * that only touch it, which Android's text drawing (SpanSet) leaves out: a transparent color span that a typing
     * animation moves to the end would hide the last line
     */
    @SuppressWarnings("unchecked")
    private static <T> T[] covering(Spanned sp, int start, int end, Class<T> type) {
        T[] all = sp.getSpans(start, end, type);
        int n = 0;
        for (T s : all) {
            int a = sp.getSpanStart(s), b = sp.getSpanEnd(s);
            if (a < b && a < end && b > start) all[n++] = s;
        }
        return n == all.length ? all : java.util.Arrays.copyOf(all, n);
    }

    /** Each char's advance with the spans' measures, and each run's font metrics into fm (by char). */
    private Paint.FontMetricsInt[] measure(CharSequence t, int start, int end) {
        advances = new float[t.length()];
        Paint.FontMetricsInt[] fm = new Paint.FontMetricsInt[t.length() + 1];
        Spanned sp = t instanceof Spanned s ? s : null;
        char[] chars = new char[t.length()];
        TextUtils.getChars(t, 0, t.length(), chars, 0);
        Paint.FontMetricsInt base = paint.getFontMetricsInt();
        for (int i = start; i < end;) {
            int next = sp != null ? sp.nextSpanTransition(i, end, MetricAffectingSpan.class) : end;
            TextPaint wp = measuring(paint, sp, i, next, false);
            Paint.FontMetricsInt m = wp.getFontMetricsInt();
            ReplacementSpan[] rep = sp != null ? covering(sp, i, next, ReplacementSpan.class) : new ReplacementSpan[0];
            if (rep.length > 0) {
                Paint.FontMetricsInt rm = new Paint.FontMetricsInt();
                rm.set(m);
                advances[i] = rep[0].getSize(wp, t, i, next, rm);
                m = rm;
            } else {
                // each paragraph's text separately, its newline measured as nothing
                for (int p = i; p < next;) {
                    int nl = p;
                    while (nl < next && chars[nl] != '\n') ++nl;
                    if (nl > p) wp.getTextRunAdvances(chars, p, nl - p, p, nl - p, false, advances, p);
                    p = nl < next ? nl + 1 : next;
                }
            }
            for (int k = i; k < next; ++k) fm[k] = m;
            i = next;
        }
        fm[t.length()] = base;
        for (int k = 0; k < t.length(); ++k) if (fm[k] == null) fm[k] = base;
        return fm;
    }

    private void ensure(int lines) {
        if (lines + 1 < starts.length) return;
        int n = Math.max(lines + 2, starts.length * 2);
        starts = java.util.Arrays.copyOf(starts, n);
        tops = java.util.Arrays.copyOf(tops, n);
        ascents = java.util.Arrays.copyOf(ascents, n);
        descents = java.util.Arrays.copyOf(descents, n);
        ellipsisStarts = java.util.Arrays.copyOf(ellipsisStarts, n);
        ellipsisCounts = java.util.Arrays.copyOf(ellipsisCounts, n);
        margins = java.util.Arrays.copyOf(margins, n);
        lineWidths = java.util.Arrays.copyOf(lineWidths, n);
    }

    /** The lines of text[start, end): the work of StaticLayout and BoringLayout. */
    void generate(int start, int end, int maxLines, TextUtils.TruncateAt ellipsize, int ellipsizedWidth, boolean includePad,
                  int[] leftIndents, int[] rightIndents) {
        this.leftIndents = leftIndents;
        this.rightIndents = rightIndents;
        CharSequence t = text;
        Paint.FontMetricsInt[] fm = measure(t, start, end);
        Spanned sp = t instanceof Spanned s ? s : null;
        String str = t.toString();
        boolean[] breakBefore = new boolean[t.length() + 1];
        BreakIterator lb = BreakIterator.getLineInstance(paint.getTextLocale());
        lb.setText(str);
        for (int b = lb.first(); b != BreakIterator.DONE; b = lb.next()) breakBefore[b] = true;
        lineCount = 0;
        int pos = start, v = 0;
        if (maxLines <= 0) maxLines = Integer.MAX_VALUE;
        do {
            ensure(lineCount);
            int paraStart = pos;
            while (paraStart > start && str.charAt(paraStart - 1) != '\n') --paraStart;
            int paraEnd = str.indexOf('\n', pos);
            if (paraEnd < 0 || paraEnd >= end) paraEnd = end;
            int margin = 0;
            if (sp != null) for (LeadingMarginSpan m : sp.getSpans(paraStart, paraEnd, LeadingMarginSpan.class))
                margin += m.getLeadingMargin(pos == paraStart);
            int indent = indent(leftIndents, lineCount) + indent(rightIndents, lineCount);
            float avail = Math.max(1, width - margin - indent);
            int i = pos;
            float w = 0;
            int lastBreak = -1;
            while (i < paraEnd) {
                char ch = str.charAt(i);
                if (w + advances[i] > avail && !Character.isWhitespace(ch) && i > pos) break;
                w += advances[i];
                ++i;
                if (breakBefore[i] && i < paraEnd) lastBreak = i;
            }
            int lineEnd;
            if (i < paraEnd) lineEnd = lastBreak > pos ? lastBreak : Math.max(i, pos + 1);
            else lineEnd = paraEnd < end ? paraEnd + 1 : paraEnd;
            boolean last = lineEnd >= end;
            int ellStart = 0, ellCount = 0;
            if (lineCount == maxLines - 1 && !last) {
                if (ellipsize == TextUtils.TruncateAt.END || ellipsize == TextUtils.TruncateAt.END_SMALL) {
                    float ellW = paint.measureText("…");
                    float room = (ellipsizedWidth > 0 ? Math.min(ellipsizedWidth, avail) : avail) - ellW;
                    int visible = lineEnd;
                    while (visible > pos && (str.charAt(visible - 1) == '\n' || Character.isWhitespace(str.charAt(visible - 1)))) --visible;
                    float sum = 0;
                    int k = pos;
                    while (k < visible && sum + advances[k] <= room) sum += advances[k++];
                    int textEnd = paraEnd;
                    if (k < textEnd || lineEnd < end) {
                        ellStart = k - pos;
                        ellCount = textEnd - k;
                        lineEnd = textEnd;
                    }
                }
                last = true;
            }
            // the line's height: its tallest run, then its LineHeightSpans
            Paint.FontMetricsInt m = new Paint.FontMetricsInt();
            m.set(fm[Math.min(pos, t.length())]);
            for (int k = pos; k < lineEnd && k < t.length(); ++k) {
                m.ascent = Math.min(m.ascent, fm[k].ascent);
                m.descent = Math.max(m.descent, fm[k].descent);
                m.top = Math.min(m.top, fm[k].top);
                m.bottom = Math.max(m.bottom, fm[k].bottom);
            }
            if (sp != null) {
                for (LineHeightSpan h : sp.getSpans(pos, lineEnd, LineHeightSpan.class)) {
                    int spanStart = sp.getSpanStart(h);
                    int spanTop = tops[Math.max(0, lineCount == 0 ? 0 : getLineForOffsetIn(spanStart, lineCount))];
                    if (h instanceof LineHeightSpan.WithDensity wd) wd.chooseHeight(t, pos, lineEnd, spanTop, v, m, paint);
                    else h.chooseHeight(t, pos, lineEnd, spanTop, v, m);
                }
            }
            int ascent = m.ascent, descent = m.descent;
            if (includePad && lineCount == 0) { topPadding = m.top - m.ascent; ascent = m.top; }
            if (includePad && last) { bottomPadding = m.bottom - m.descent; descent = m.bottom; }
            int extra = last ? 0 : Math.round((descent - ascent) * (spacingMult - 1) + spacingAdd);
            starts[lineCount] = pos;
            tops[lineCount] = v;
            ascents[lineCount] = ascent;
            descents[lineCount] = descent + extra;
            ellipsisStarts[lineCount] = ellStart;
            ellipsisCounts[lineCount] = ellCount;
            margins[lineCount] = margin;
            float lw = 0;
            int visibleEnd = ellCount > 0 ? pos + ellStart : lineEnd;
            int trim = visibleEnd;
            while (trim > pos && (str.charAt(trim - 1) == '\n' || Character.isWhitespace(str.charAt(trim - 1)))) --trim;
            for (int k = pos; k < trim; ++k) lw += advances[k];
            if (ellCount > 0) lw += paint.measureText("…");
            lineWidths[lineCount] = lw;
            v += descent + extra - ascent;
            ++lineCount;
            pos = lineEnd;
            if (last) break;
        } while (pos < end || (pos == end && end > start && str.charAt(end - 1) == '\n' && lineCount < maxLines));
        ensure(lineCount);
        starts[lineCount] = pos;
        tops[lineCount] = v;
    }

    private int getLineForOffsetIn(int offset, int count) {
        int line = 0;
        while (line + 1 < count && starts[line + 1] <= offset) ++line;
        return line;
    }

    private static int indent(int[] indents, int line) {
        if (indents == null || indents.length == 0) return 0;
        return indents[Math.min(line, indents.length - 1)];
    }

    // ---- queries ----

    public final CharSequence getText() { return text; }
    public final TextPaint getPaint() { return paint; }
    public final int getWidth() { return width; }
    public int getEllipsizedWidth() { return width; }
    public final void increaseWidthTo(int wid) { width = Math.max(width, wid); }
    public int getHeight() { return getLineTop(getLineCount()); }
    public int getHeight(boolean cap) { return getHeight(); }
    public final Alignment getAlignment() { return alignment; }
    public final float getSpacingMultiplier() { return spacingMult; }
    public final float getSpacingAdd() { return spacingAdd; }
    public final float getLineSpacingMultiplier() { return spacingMult; }
    public final float getLineSpacingAmount() { return spacingAdd; }
    public int getLineCount() { return lineCount; }
    public int getLineTop(int line) { return tops[Math.max(0, Math.min(line, lineCount))]; }
    public int getLineDescent(int line) { return descents[line]; }
    public int getLineAscent(int line) { return ascents[line]; }
    public int getLineStart(int line) { return starts[Math.max(0, Math.min(line, lineCount))]; }
    public final int getLineEnd(int line) { return getLineStart(line + 1); }
    public int getLineVisibleEnd(int line) {
        int end = getLineEnd(line), start = getLineStart(line);
        if (ellipsisCounts[line] > 0) return start + ellipsisStarts[line];
        while (end > start && (text.charAt(end - 1) == '\n' || Character.isWhitespace(text.charAt(end - 1)))) --end;
        return end;
    }
    public final int getLineBottom(int line) { return getLineTop(line + 1); }
    public final int getLineBottom(int line, boolean includeLineSpacing) { return getLineBottom(line); }
    public final int getLineBaseline(int line) { return getLineTop(line + 1) - getLineDescent(line); }
    /** the line's baseline, with its bounds across the layout's width */
    public int getLineBounds(int line, android.graphics.Rect bounds) {
        if (bounds != null) bounds.set(0, getLineTop(line), width, getLineTop(line + 1));
        return getLineBaseline(line);
    }
    public int getTopPadding() { return topPadding; }
    public int getBottomPadding() { return bottomPadding; }
    public int getEllipsisStart(int line) { return ellipsisStarts[line]; }
    public int getEllipsisCount(int line) { return ellipsisCounts[line]; }
    public int getParagraphDirection(int line) { return DIR_LEFT_TO_RIGHT; }
    public final Alignment getParagraphAlignment(int line) { return alignment; }
    public final int getParagraphLeft(int line) { return 0; }
    public final int getParagraphRight(int line) { return width; }
    public boolean getLineContainsTab(int line) { return false; }
    public boolean isRtlCharAt(int offset) { return false; }
    public boolean isLevelBoundary(int offset) { return false; }
    public boolean isFallbackLineSpacingEnabled() { return false; }
    public float getLineMax(int line) { return lineWidths[line]; }
    public float getLineWidth(int line) { return lineWidths[line]; }
    public int getIndentAdjust(int line, Alignment alignment) { return 0; }

    public float getLineLeft(int line) {
        int left = indent(leftIndents, line) + margins[line], right = indent(rightIndents, line);
        float lw = lineWidths[line];
        switch (alignment) {
            case ALIGN_OPPOSITE:
            case ALIGN_RIGHT: return width - right - lw;
            case ALIGN_CENTER: return left + (width - left - right - lw) / 2f;
            default: return left;
        }
    }

    public float getLineRight(int line) { return getLineLeft(line) + lineWidths[line]; }

    public int getLineForOffset(int offset) {
        int low = 0, high = lineCount - 1;
        while (low < high) {
            int mid = (low + high + 1) >>> 1;
            if (starts[mid] <= offset) low = mid; else high = mid - 1;
        }
        return Math.max(0, low);
    }

    public int getLineForVertical(int vertical) {
        int low = 0, high = lineCount - 1;
        while (low < high) {
            int mid = (low + high + 1) >>> 1;
            if (tops[mid] <= vertical) low = mid; else high = mid - 1;
        }
        return Math.max(0, low);
    }

    public float getPrimaryHorizontal(int offset) {
        int line = getLineForOffset(offset);
        float x = getLineLeft(line);
        int end = Math.min(offset, getLineVisibleEnd(line) + (ellipsisCounts[line] > 0 ? 0 : 1));
        for (int k = getLineStart(line); k < end && k < offset && k < advances.length; ++k) x += advances[k];
        return x;
    }
    public float getPrimaryHorizontal(int offset, boolean clamped) { return getPrimaryHorizontal(offset); }
    public float getSecondaryHorizontal(int offset) { return getPrimaryHorizontal(offset); }
    public float getSecondaryHorizontal(int offset, boolean clamped) { return getPrimaryHorizontal(offset); }

    public int getOffsetForHorizontal(int line, float horiz) {
        int start = getLineStart(line), end = getLineVisibleEnd(line);
        float x = getLineLeft(line);
        for (int k = start; k < end; ++k) {
            float a = advances[k];
            if (horiz < x + a / 2) return k;
            x += a;
        }
        return end;
    }
    public int getOffsetForHorizontal(int line, float horiz, boolean primary) { return getOffsetForHorizontal(line, horiz); }

    public int getOffsetToLeftOf(int offset) { return Math.max(0, offset - 1); }
    public int getOffsetToRightOf(int offset) { return Math.min(text.length(), offset + 1); }

    public static float getDesiredWidth(CharSequence source, TextPaint paint) { return getDesiredWidth(source, 0, source.length(), paint); }

    public static float getDesiredWidth(CharSequence source, int start, int end, TextPaint paint) {
        float max = 0;
        StaticLayout l = new StaticLayout(source.subSequence(start, end), paint, Integer.MAX_VALUE / 4, Alignment.ALIGN_NORMAL, 1, 0, false);
        for (int i = 0; i < l.getLineCount(); ++i) max = Math.max(max, l.getLineWidth(i));
        return max;
    }

    public static float getDesiredWidth(CharSequence source, int start, int end, TextPaint paint, TextDirectionHeuristic textDir) {
        return getDesiredWidth(source, start, end, paint);
    }

    public int[] getRangeForRect(RectF area, SegmentFinder segmentFinder, TextInclusionStrategy inclusionStrategy) {
        int first = getLineForVertical((int) area.top), last = getLineForVertical((int) area.bottom);
        int start = -1, end = -1;
        for (int line = first; line <= last; ++line) {
            int a = getOffsetForHorizontal(line, area.left), b = getOffsetForHorizontal(line, area.right);
            if (a < b) {
                if (start < 0) start = a;
                end = b;
            }
        }
        return start < 0 ? null : new int[] {start, end};
    }

    public void getSelectionPath(int start, int end, Path dest) {
        dest.reset();
        if (start == end) return;
        if (end < start) { int t = end; end = start; start = t; }
        int first = getLineForOffset(start), last = getLineForOffset(end);
        for (int line = first; line <= last; ++line) {
            float l = line == first ? getPrimaryHorizontal(start) : getLineLeft(line);
            float r = line == last ? getPrimaryHorizontal(end) : getLineRight(line);
            dest.addRect(l, getLineTop(line), Math.max(l, r), getLineBottom(line), Path.Direction.CW);
        }
    }

    public void getCursorPath(int point, Path dest, CharSequence editingBuffer) {
        dest.reset();
        int line = getLineForOffset(point);
        float x = getPrimaryHorizontal(point) - 0.5f;
        dest.moveTo(x, getLineTop(line));
        dest.lineTo(x, getLineBottom(line));
    }

    // ---- drawing ----

    public void draw(Canvas canvas) { draw(canvas, null, null, 0); }

    public void draw(Canvas c, Path highlight, Paint highlightPaint, int cursorOffsetVertical) {
        if (highlight != null && highlightPaint != null) {
            c.translate(0, cursorOffsetVertical);
            c.drawPath(highlight, highlightPaint);
            c.translate(0, -cursorOffsetVertical);
        }
        Spanned sp = text instanceof Spanned s ? s : null;
        for (int line = 0; line < lineCount; ++line) {
            int start = getLineStart(line), end = getLineVisibleEnd(line);
            int top = getLineTop(line), bottom = getLineBottom(line), baseline = getLineBaseline(line);
            float x = getLineLeft(line);
            TextPaint lastPaint = paint;
            for (int i = start; i < end;) {
                int next = sp != null ? sp.nextSpanTransition(i, end, CharacterStyle.class) : end;
                TextPaint wp = measuring(paint, sp, i, next, true);
                lastPaint = wp;
                float w = 0;
                for (int k = i; k < next; ++k) w += advances[k];
                if (wp.bgColor != 0) {
                    Paint bg = new Paint();
                    bg.setColor(wp.bgColor);
                    c.drawRect(x, top, x + w, bottom, bg);
                }
                ReplacementSpan[] rep = sp != null ? covering(sp, i, next, ReplacementSpan.class) : new ReplacementSpan[0];
                if (rep.length > 0) rep[0].draw(c, text, i, next, x, top, baseline, bottom, wp);
                else c.drawTextRun(text, i, next, i, next, x, baseline + wp.baselineShift, false, wp);
                x += w;
                i = next;
            }
            if (ellipsisCounts[line] > 0) c.drawText("…", x, baseline, lastPaint);
        }
    }

    public void drawText(Canvas canvas) { draw(canvas); }
    public void drawBackground(Canvas canvas) {}
}
