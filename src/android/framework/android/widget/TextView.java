// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.animation.StateListAnimator;
import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.os.LocaleList;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.Layout;
import android.text.NoCopySpan;
import android.text.PrecomputedText;
import android.text.Selection;
import android.text.Spannable;
import android.text.Spanned;
import android.text.SpannableStringBuilder;
import android.text.StaticLayout;
import android.text.TextDirectionHeuristic;
import android.text.TextDirectionHeuristics;
import android.text.TextPaint;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.text.method.KeyListener;
import android.text.method.TransformationMethod;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.ActionMode;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.textclassifier.TextClassifier;
import java.util.ArrayList;
import java.util.Locale;

/** A view of text: one StaticLayout of it, with compound drawables around it. */
public class TextView extends View {
    public enum BufferType { NORMAL, SPANNABLE, EDITABLE }
    public static final int AUTO_SIZE_TEXT_TYPE_NONE = 0, AUTO_SIZE_TEXT_TYPE_UNIFORM = 1;

    public interface OnEditorActionListener { boolean onEditorAction(TextView v, int actionId, KeyEvent event); }

    private static final InputFilter[] NO_FILTERS = new InputFilter[0];
    // android.R.attr: textSize, textStyle, textColor, textAllCaps, in ascending order as TypedArray needs them
    private static final int[] APPEARANCE_ATTRS = {0x01010095, 0x01010097, 0x01010098, 0x0101038c};

    private CharSequence text = "";
    private CharSequence hint;
    private final TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private Layout layout;
    private ColorStateList textColors = ColorStateList.valueOf(0xFF000000), hintColors, linkColors;
    private int gravity = Gravity.TOP | Gravity.START;
    private int maxLines = Integer.MAX_VALUE, minLines, maxWidth = Integer.MAX_VALUE;
    private boolean singleLine, allCaps, includePad = true, linksClickable = true, horizontallyScrolling;
    private TextUtils.TruncateAt ellipsize;
    private float spacingAdd, spacingMult = 1;
    private int breakStrategy, hyphenationFrequency, justificationMode, highlightColor = 0x6633B5E5;
    private InputFilter[] filters = NO_FILTERS;
    private TransformationMethod transformation;
    private KeyListener keyListener;
    private int inputType = InputType.TYPE_NULL, imeOptions = EditorInfo.IME_NULL;
    private OnEditorActionListener editorActionListener;
    private ArrayList<TextWatcher> listeners;
    private Editable.Factory editableFactory = Editable.Factory.getInstance();
    private Spannable.Factory spannableFactory = Spannable.Factory.getInstance();
    private final Drawable[] compound = new Drawable[4];
    private int drawablePadding;
    private ColorStateList compoundTint;
    private PorterDuff.Mode compoundTintMode;
    private int autoSizeType = AUTO_SIZE_TEXT_TYPE_NONE, autoSizeGranularity = -1;
    private int[] autoSizes = new int[0];
    private Locale textLocale;
    private ActionMode.Callback selectionCallback;
    private TextClassifier textClassifier;
    private String fontVariation;
    private StateListAnimator stateListAnimator;
    private int batchDepth;

    public TextView(Context context) {
        super(context);
        defaults(context);
    }

    /** The paint's size and density for the display: 14 sp */
    private void defaults(Context context) {
        paint.setTextSize(14 * context.getResources().getDisplayMetrics().scaledDensity);
        paint.density = context.getResources().getDisplayMetrics().density;
    }
    /** android.R.attr.textViewStyle, the theme's style of a TextView */
    private static final int TEXT_VIEW_STYLE = 0x01010084;
    public TextView(Context context, AttributeSet attrs) { this(context, attrs, TEXT_VIEW_STYLE); }
    public TextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public TextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        defaults(context);
        if (attrs != null) attributes(context, attrs, defStyleAttr, defStyleRes);
    }

    /** android:textAppearance, text, hint, textSize and textColor, by their resource IDs (android.R.attr) */
    private static final int[] TEXT_ATTRS = {0x01010034, 0x0101014f, 0x01010150, 0x01010095, 0x01010098};

    /**
     * A layout's attributes of a TextView, as Android's constructor reads them: its text appearance first, which
     * textSize and textColor then override, and its text and hint
     */
    private void attributes(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        TypedArray a = context.obtainStyledAttributes(attrs, TEXT_ATTRS, defStyleAttr, defStyleRes);
        try {
            int appearance = a.getResourceId(0, 0);
            if (appearance != 0) setTextAppearance(context, appearance);
            if (a.hasValue(3)) setRawTextSize(a.getDimensionPixelSize(3, (int) getTextSize()));
            if (a.hasValue(4)) {
                ColorStateList c = a.getColorStateList(4);
                if (c != null) setTextColor(c);
            }
            if (a.hasValue(2)) setHint(a.getText(2));
            if (a.hasValue(1)) setText(a.getText(1));
        } finally {
            a.recycle();
        }
    }

    // ---- text ----
    public void setText(CharSequence text) { setText(text, BufferType.NORMAL); }
    public void setText(CharSequence text, BufferType type) {
        if (text == null) text = "";
        for (InputFilter f : filters) {
            CharSequence out = f.filter(text, 0, text.length(), new SpannableStringBuilder(), 0, 0);
            if (out != null) text = out;
        }
        CharSequence old = this.text;
        int oldLength = old.length();
        sendBeforeTextChanged(old, 0, oldLength, text.length());
        if (type == BufferType.EDITABLE || getDefaultEditable() || keyListener != null) {
            Editable e = editableFactory.newEditable(text);
            e.setFilters(filters);
            e.setSpan(new ChangeWatcher(), 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE | (100 << Spanned.SPAN_PRIORITY_SHIFT));
            text = e;
        } else if (type == BufferType.SPANNABLE) {
            text = spannableFactory.newSpannable(text);
        }
        this.text = text;
        textChanged();
        sendOnTextChanged(text, 0, oldLength, text.length());
        sendAfterTextChanged(text instanceof Editable e ? e : null);
    }
    public final void setText(char[] text, int start, int len) { setText(new String(text, start, len)); }
    public final void setText(int resid) { setText(getContext().getText(resid)); }
    public final void setText(int resid, BufferType type) { setText(getContext().getText(resid), type); }
    public CharSequence getText() { return text; }
    public int length() { return text.length(); }
    public Editable getEditableText() { return text instanceof Editable e ? e : null; }
    protected boolean getDefaultEditable() { return false; }
    public final void setEditableFactory(Editable.Factory factory) { editableFactory = factory; setText(text); }
    public final void setSpannableFactory(Spannable.Factory factory) { spannableFactory = factory; setText(text); }
    private void textChanged() { layout = null; requestLayout(); invalidate(); }
    @Override public void setTextAlignment(int textAlignment) { super.setTextAlignment(textAlignment); textChanged(); }

    public final void setHint(CharSequence hint) { this.hint = hint; if (text.length() == 0) textChanged(); }
    public final void setHint(int resid) { setHint(getContext().getText(resid)); }
    public CharSequence getHint() { return hint; }

    // ---- text watchers ----
    public void addTextChangedListener(TextWatcher watcher) { if (listeners == null) listeners = new ArrayList<>(); listeners.add(watcher); }
    public void removeTextChangedListener(TextWatcher watcher) { if (listeners != null) listeners.remove(watcher); }
    private TextWatcher[] watchers() { return listeners == null ? new TextWatcher[0] : listeners.toArray(new TextWatcher[0]); }
    private void sendBeforeTextChanged(CharSequence s, int start, int before, int after) { for (TextWatcher w : watchers()) w.beforeTextChanged(s, start, before, after); }
    private void sendOnTextChanged(CharSequence s, int start, int before, int after) {
        onTextChanged(s, start, before, after);
        for (TextWatcher w : watchers()) w.onTextChanged(s, start, before, after);
    }
    private void sendAfterTextChanged(Editable s) { if (s != null) for (TextWatcher w : watchers()) w.afterTextChanged(s); }
    protected void onTextChanged(CharSequence text, int start, int lengthBefore, int lengthAfter) {}
    public void beginBatchEdit() { ++batchDepth; }
    public void endBatchEdit() { if (batchDepth > 0) --batchDepth; }

    /** Sends the edits of the editable text to the watchers of this view, as the editable itself reports them. */
    private final class ChangeWatcher implements TextWatcher, NoCopySpan {
        @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { if (s == text) sendBeforeTextChanged(s, start, count, after); }
        @Override public void onTextChanged(CharSequence s, int start, int before, int count) { if (s == text) { textChanged(); sendOnTextChanged(s, start, before, count); } }
        @Override public void afterTextChanged(android.text.Editable s) { if (s == text) sendAfterTextChanged(s); }
    }

    // ---- selection ----
    public int getSelectionStart() { return Selection.getSelectionStart(text); }
    public int getSelectionEnd() { return Selection.getSelectionEnd(text); }
    public boolean hasSelection() { int s = getSelectionStart(), e = getSelectionEnd(); return s >= 0 && s != e; }
    public boolean onTextContextMenuItem(int id) {
        if (id == 0x0102001f && text instanceof Spannable s) { Selection.setSelection(s, 0, s.length()); return true; } // android.R.id.selectAll
        return false;
    }
    public void setCustomSelectionActionModeCallback(ActionMode.Callback actionModeCallback) { selectionCallback = actionModeCallback; }
    public ActionMode.Callback getCustomSelectionActionModeCallback() { return selectionCallback; }
    public void setHighlightColor(int color) { highlightColor = color; invalidate(); }
    public int getHighlightColor() { return highlightColor; }
    public void setTextClassifier(TextClassifier textClassifier) { this.textClassifier = textClassifier; }
    public TextClassifier getTextClassifier() { return textClassifier != null ? textClassifier : TextClassifier.NO_OP; }

    // ---- input ----
    public void setInputType(int type) {
        inputType = type;
        boolean multiLine = (type & (InputType.TYPE_MASK_CLASS | InputType.TYPE_TEXT_FLAG_MULTI_LINE)) == (InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        if ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT) singleLine = !multiLine;
        textChanged();
    }
    public void setRawInputType(int type) { inputType = type; }
    public int getInputType() { return inputType; }
    public void setImeOptions(int imeOptions) { this.imeOptions = imeOptions; }
    public int getImeOptions() { return imeOptions; }
    /* the input method's action key, its label and id, and options private to an input method: kept for the
       EditorInfo, which no input method reads here */
    private CharSequence imeActionLabel;
    private int imeActionId;
    private String privateImeOptions;
    public void setImeActionLabel(CharSequence label, int actionId) { imeActionLabel = label; imeActionId = actionId; }
    public CharSequence getImeActionLabel() { return imeActionLabel; }
    public int getImeActionId() { return imeActionId; }
    public void setPrivateImeOptions(String type) { privateImeOptions = type; }
    public String getPrivateImeOptions() { return privateImeOptions; }
    private boolean selectAllOnFocus;
    /** whether focus selects all the text, as Android's does for an editable one */
    public void setSelectAllOnFocus(boolean selectAllOnFocus) { this.selectAllOnFocus = selectAllOnFocus; }
    @Override protected void onFocusChanged(boolean focused, int direction, android.graphics.Rect previouslyFocusedRect) {
        super.onFocusChanged(focused, direction, previouslyFocusedRect);
        if (focused && selectAllOnFocus && text instanceof Spannable s) Selection.setSelection(s, 0, s.length());
    }
    public void setOnEditorActionListener(OnEditorActionListener l) { editorActionListener = l; }
    public void onEditorAction(int actionCode) {
        if (editorActionListener != null && editorActionListener.onEditorAction(this, actionCode, null)) return;
        if (actionCode == EditorInfo.IME_ACTION_NEXT) { View v = focusSearch(FOCUS_FORWARD); if (v != null) v.requestFocus(FOCUS_FORWARD); }
        else if (actionCode == EditorInfo.IME_ACTION_DONE) clearFocus();
    }
    public void setKeyListener(KeyListener input) {
        keyListener = input;
        if (input != null && !(text instanceof Editable)) setText(text);
        inputType = input != null ? input.getInputType() : InputType.TYPE_NULL;
        setFocusable(input != null);
        setFocusableInTouchMode(input != null);
    }
    public final KeyListener getKeyListener() { return keyListener; }
    public void setFilters(InputFilter[] filters) {
        if (filters == null) throw new IllegalArgumentException();
        this.filters = filters;
        if (text instanceof Editable e) e.setFilters(filters);
    }
    public InputFilter[] getFilters() { return filters; }

    /** Typed keys edit editable text at its selection: the host keyboard arrives as key events. */
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!(text instanceof Editable e) || !isEnabled() || !(getDefaultEditable() || keyListener != null)) return super.onKeyDown(keyCode, event);
        int a = Selection.getSelectionStart(e), b = Selection.getSelectionEnd(e);
        if (a < 0) a = b = e.length();
        int start = Math.min(a, b), end = Math.max(a, b);
        if (keyCode == KeyEvent.KEYCODE_DEL) {
            if (start == end && start > 0) start = Character.offsetByCodePoints(e, start, -1);
            if (start != end) e.delete(start, end);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) {
            if (singleLine) { int action = imeOptions & EditorInfo.IME_MASK_ACTION; onEditorAction(action == EditorInfo.IME_ACTION_UNSPECIFIED ? EditorInfo.IME_ACTION_DONE : action); return true; }
        }
        int c = event.getUnicodeChar();
        if (c == 0) return super.onKeyDown(keyCode, event);
        e.replace(start, end, new String(Character.toChars(c)));
        return true;
    }

    // ---- appearance ----
    public void setTextColor(int color) { setTextColor(ColorStateList.valueOf(color)); }
    public void setTextColor(ColorStateList colors) {
        if (colors == null) throw new NullPointerException();
        textColors = colors;
        updateTextColor();
    }
    public final ColorStateList getTextColors() { return textColors; }
    public final int getCurrentTextColor() { return paint.getColor(); }
    public final void setHintTextColor(int color) { hintColors = ColorStateList.valueOf(color); invalidate(); }
    public final void setHintTextColor(ColorStateList colors) { hintColors = colors; invalidate(); }
    public final ColorStateList getHintTextColors() { return hintColors; }
    public final void setLinkTextColor(int color) { setLinkTextColor(ColorStateList.valueOf(color)); }
    public final void setLinkTextColor(ColorStateList colors) { linkColors = colors; updateTextColor(); }
    public final ColorStateList getLinkTextColors() { return linkColors; }
    private void updateTextColor() {
        int[] state = getDrawableState();
        paint.setColor(textColors.getColorForState(state, textColors.getDefaultColor()));
        if (linkColors != null) paint.linkColor = linkColors.getColorForState(state, linkColors.getDefaultColor());
        paint.drawableState = state;
        invalidate();
    }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (textColors != null) updateTextColor();
        int[] state = getDrawableState();
        if (compound != null) for (Drawable d : compound) if (d != null && d.isStateful()) d.setState(state); // null while View constructs
    }
    public void setTextSize(float size) { setTextSize(TypedValue.COMPLEX_UNIT_SP, size); }
    public void setTextSize(int unit, float size) { setRawTextSize(TypedValue.applyDimension(unit, size, getResources().getDisplayMetrics())); }
    private void setRawTextSize(float px) { if (px != paint.getTextSize()) { paint.setTextSize(px); textChanged(); } }
    public float getTextSize() { return paint.getTextSize(); }
    public TextPaint getPaint() { return paint; }
    public final Layout getLayout() { return layout; }
    public void setTypeface(Typeface tf) { if (paint.getTypeface() != tf) { paint.setTypeface(tf); textChanged(); } }
    public void setTypeface(Typeface tf, int style) {
        if (style <= 0) { paint.setFakeBoldText(false); paint.setTextSkewX(0); setTypeface(tf); return; }
        tf = tf == null ? Typeface.defaultFromStyle(style) : Typeface.create(tf, style);
        setTypeface(tf);
        int need = style & ~(tf != null ? tf.getStyle() : 0);
        paint.setFakeBoldText((need & Typeface.BOLD) != 0);
        paint.setTextSkewX((need & Typeface.ITALIC) != 0 ? -0.25f : 0);
    }
    public Typeface getTypeface() { return paint.getTypeface(); }
    public boolean setFontVariationSettings(String fontVariationSettings) {
        fontVariation = fontVariationSettings;
        return paint.setFontVariationSettings(fontVariationSettings);
    }
    public String getFontVariationSettings() { return fontVariation; }
    public void setTextAppearance(int resId) { setTextAppearance(getContext(), resId); }
    @Deprecated public void setTextAppearance(Context context, int resId) {
        TypedArray a = context.obtainStyledAttributes(resId, APPEARANCE_ATTRS);
        if (a.hasValue(0)) setRawTextSize(a.getDimensionPixelSize(0, (int) getTextSize()));
        if (a.hasValue(1)) setTypeface(getTypeface(), a.getInt(1, Typeface.NORMAL));
        if (a.hasValue(2)) { ColorStateList c = a.getColorStateList(2); if (c != null) setTextColor(c); }
        if (a.hasValue(3)) setAllCaps(a.getBoolean(3, false));
        a.recycle();
    }
    public void setAllCaps(boolean allCaps) { this.allCaps = allCaps; textChanged(); }
    public boolean isAllCaps() { return allCaps; }
    public final void setTransformationMethod(TransformationMethod method) { transformation = method; textChanged(); }
    public final TransformationMethod getTransformationMethod() { return transformation; }
    public void setTextLocale(Locale locale) { textLocale = locale; paint.setTextLocale(locale); textChanged(); }
    public void setTextLocales(LocaleList locales) { textLocale = locales != null && locales.size() > 0 ? locales.get(0) : null; paint.setTextLocales(locales); textChanged(); }
    public Locale getTextLocale() { return textLocale != null ? textLocale : Locale.getDefault(); }
    public LocaleList getTextLocales() { return new LocaleList(getTextLocale()); }
    public TextDirectionHeuristic getTextDirectionHeuristic() { return TextDirectionHeuristics.FIRSTSTRONG_LTR; }
    public PrecomputedText.Params getTextMetricsParams() { return new PrecomputedText.Params.Builder(paint).build(); }
    private android.text.method.MovementMethod movementMethod;
    public final void setMovementMethod(android.text.method.MovementMethod movement) { movementMethod = movement; }
    public final android.text.method.MovementMethod getMovementMethod() { return movementMethod; }
    public final void setLinksClickable(boolean whether) { linksClickable = whether; }
    public final boolean getLinksClickable() { return linksClickable; }
    public void setStateListAnimator(StateListAnimator stateListAnimator) { this.stateListAnimator = stateListAnimator; }
    public StateListAnimator getStateListAnimator() { return stateListAnimator; }

    // ---- layout parameters ----
    public void setGravity(int gravity) {
        if ((gravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) == 0) gravity |= Gravity.START;
        if ((gravity & Gravity.VERTICAL_GRAVITY_MASK) == 0) gravity |= Gravity.TOP;
        if (gravity != this.gravity) { this.gravity = gravity; textChanged(); }
    }
    public int getGravity() { return gravity; }
    public void setSingleLine() { setSingleLine(true); }
    public void setSingleLine(boolean singleLine) { this.singleLine = singleLine; if (singleLine) maxLines = 1; else if (maxLines == 1) maxLines = Integer.MAX_VALUE; textChanged(); }
    public void setLines(int lines) { maxLines = minLines = lines; textChanged(); }
    public void setMaxLines(int maxLines) { this.maxLines = maxLines; textChanged(); }
    public int getMaxLines() { return maxLines; }
    public void setMinLines(int minLines) { this.minLines = minLines; textChanged(); }
    public int getMinLines() { return minLines; }
    public void setMaxWidth(int maxPixels) { maxWidth = maxPixels; textChanged(); }
    public int getMaxWidth() { return maxWidth; }
    /* a minimum in pixels or in ems (the line height), as Android's; a width in ems is a maximum as well */
    private int minWidth, minHeight, minEms, maxEms = -1;
    public void setMinWidth(int minPixels) { minWidth = minPixels; minEms = 0; textChanged(); }
    public int getMinWidth() { return minWidth; }
    public void setMinHeight(int minPixels) { minHeight = minPixels; textChanged(); }
    public int getMinHeight() { return minHeight; }
    private int maxHeight = Integer.MAX_VALUE;
    public void setMaxHeight(int maxPixels) { maxHeight = maxPixels; textChanged(); }
    public int getMaxHeight() { return maxHeight; }
    /** exactly so many pixels tall or wide, as Android's: the minimum and the maximum */
    public void setHeight(int pixels) { minHeight = maxHeight = pixels; textChanged(); }
    public void setWidth(int pixels) { minWidth = maxWidth = pixels; minEms = 0; maxEms = -1; textChanged(); }
    public void setMinEms(int minEms) { this.minEms = minEms; minWidth = 0; textChanged(); }
    public int getMinEms() { return minEms; }
    public void setMaxEms(int maxEms) { this.maxEms = maxEms; maxWidth = Integer.MAX_VALUE; textChanged(); }
    public int getMaxEms() { return maxEms; }
    public void setEms(int ems) { minEms = maxEms = ems; minWidth = 0; maxWidth = Integer.MAX_VALUE; textChanged(); }
    public int getLineCount() { return layout != null ? layout.getLineCount() : 0; }
    /** the baseline of the line, with its bounds in the view's coordinates */
    public int getLineBounds(int line, android.graphics.Rect bounds) {
        if (layout == null) { if (bounds != null) bounds.set(0, 0, 0, 0); return 0; }
        int base = layout.getLineBounds(line, bounds);
        int dx = getCompoundPaddingLeft(), dy = getTotalPaddingTop();
        if (bounds != null) bounds.offset(dx, dy);
        return base + dy;
    }
    public float getLetterSpacing() { return paint.getLetterSpacing(); }
    public void setLetterSpacing(float letterSpacing) { paint.setLetterSpacing(letterSpacing); textChanged(); }
    public void setImeHintLocales(android.os.LocaleList hintLocales) {}
    public android.os.LocaleList getImeHintLocales() { return null; }
    private int marqueeRepeatLimit = 3;
    public void setMarqueeRepeatLimit(int marqueeLimit) { marqueeRepeatLimit = marqueeLimit; }
    public int getMarqueeRepeatLimit() { return marqueeRepeatLimit; }
    /* no cursor is drawn */
    private android.graphics.drawable.Drawable cursorDrawable;
    public android.graphics.drawable.Drawable getTextCursorDrawable() { return cursorDrawable; }
    public void setTextCursorDrawable(android.graphics.drawable.Drawable drawable) { cursorDrawable = drawable; }
    public void setTextCursorDrawable(int res) { cursorDrawable = res != 0 ? getContext().getDrawable(res) : null; }
    public void setEllipsize(TextUtils.TruncateAt where) { ellipsize = where; textChanged(); }
    public TextUtils.TruncateAt getEllipsize() { return ellipsize; }
    public void setLineSpacing(float add, float mult) { spacingAdd = add; spacingMult = mult; textChanged(); }
    public float getLineSpacingExtra() { return spacingAdd; }
    public float getLineSpacingMultiplier() { return spacingMult; }
    /** the distance between two lines' baselines: the spacing added to the font's own height, as Android sets it */
    public void setLineHeight(int lineHeight) {
        if (lineHeight < 0) throw new IllegalArgumentException("line height must not be negative");
        int fontHeight = paint.getFontMetricsInt(null);
        if (lineHeight != fontHeight) setLineSpacing(lineHeight - fontHeight, 1f);
    }
    /** setLineHeight in a unit of TypedValue's (API level 34) */
    public void setLineHeight(int unit, float lineHeight) {
        setLineHeight(Math.round(android.util.TypedValue.applyDimension(unit, lineHeight, getResources().getDisplayMetrics())));
    }
    public int getLineHeight() { return Math.round(paint.getFontMetricsInt(null) * spacingMult + spacingAdd); }
    public void setIncludeFontPadding(boolean includepad) { includePad = includepad; textChanged(); }
    public boolean getIncludeFontPadding() { return includePad; }
    public void setBreakStrategy(int breakStrategy) { this.breakStrategy = breakStrategy; textChanged(); }
    public int getBreakStrategy() { return breakStrategy; }
    public void setHyphenationFrequency(int hyphenationFrequency) { this.hyphenationFrequency = hyphenationFrequency; textChanged(); }
    public int getHyphenationFrequency() { return hyphenationFrequency; }
    /** Layout.JUSTIFICATION_MODE_*: kept and given to the layout, which lays lines out unjustified */
    public void setJustificationMode(int justificationMode) { this.justificationMode = justificationMode; textChanged(); }
    public int getJustificationMode() { return justificationMode; }
    public void setHorizontallyScrolling(boolean whether) { horizontallyScrolling = whether; textChanged(); }
    public final boolean isHorizontallyScrollable() { return horizontallyScrolling; }
    /** The top padding becomes what puts the first baseline at this distance from the top. */
    public void setFirstBaselineToTopHeight(int firstBaselineToTopHeight) {
        if (firstBaselineToTopHeight < 0) throw new IllegalArgumentException("firstBaselineToTopHeight must be non-negative");
        Paint.FontMetricsInt fm = paint.getFontMetricsInt();
        int top = includePad ? fm.top : fm.ascent;
        if (firstBaselineToTopHeight > Math.abs(top)) setPadding(getPaddingLeft(), firstBaselineToTopHeight + top, getPaddingRight(), getPaddingBottom());
    }
    public void setLastBaselineToBottomHeight(int lastBaselineToBottomHeight) {
        if (lastBaselineToBottomHeight < 0) throw new IllegalArgumentException("lastBaselineToBottomHeight must be non-negative");
        Paint.FontMetricsInt fm = paint.getFontMetricsInt();
        int bottom = includePad ? fm.bottom : fm.descent;
        if (lastBaselineToBottomHeight > Math.abs(bottom)) setPadding(getPaddingLeft(), getPaddingTop(), getPaddingRight(), lastBaselineToBottomHeight - bottom);
    }

    // ---- auto-size ----
    public void setAutoSizeTextTypeWithDefaults(int autoSizeTextType) {
        if (autoSizeTextType == AUTO_SIZE_TEXT_TYPE_NONE) { clearAutoSize(); return; }
        if (autoSizeTextType != AUTO_SIZE_TEXT_TYPE_UNIFORM) throw new IllegalArgumentException("Unknown auto-size text type: " + autoSizeTextType);
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        setUniform(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 12, dm), TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 112, dm), 1);
    }
    public void setAutoSizeTextTypeUniformWithConfiguration(int autoSizeMinTextSize, int autoSizeMaxTextSize, int autoSizeStepGranularity, int unit) {
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        float min = TypedValue.applyDimension(unit, autoSizeMinTextSize, dm), max = TypedValue.applyDimension(unit, autoSizeMaxTextSize, dm);
        float step = TypedValue.applyDimension(unit, autoSizeStepGranularity, dm);
        if (min <= 0) throw new IllegalArgumentException("Minimum auto-size text size (" + min + "px) is less or equal to (0px)");
        if (max <= min) throw new IllegalArgumentException("Maximum auto-size text size (" + max + "px) is less or equal to minimum auto-size text size (" + min + "px)");
        if (step <= 0) throw new IllegalArgumentException("The auto-size step granularity (" + step + "px) is less or equal to (0px)");
        setUniform(min, max, step);
    }
    public void setAutoSizeTextTypeUniformWithPresetSizes(int[] presetSizes, int unit) {
        if (presetSizes == null || presetSizes.length == 0) { setAutoSizeTextTypeWithDefaults(AUTO_SIZE_TEXT_TYPE_UNIFORM); return; }
        int[] px = new int[presetSizes.length];
        for (int i = 0; i < px.length; ++i) px[i] = Math.round(TypedValue.applyDimension(unit, presetSizes[i], getResources().getDisplayMetrics()));
        java.util.Arrays.sort(px);
        autoSizes = java.util.Arrays.stream(px).filter(v -> v > 0).distinct().toArray();
        autoSizeGranularity = -1;
        autoSizeType = AUTO_SIZE_TEXT_TYPE_UNIFORM;
        textChanged();
    }
    private void setUniform(float min, float max, float step) {
        int n = (int) Math.floor((max - min) / step) + 1;
        autoSizes = new int[n];
        for (int i = 0; i < n; ++i) autoSizes[i] = Math.round(min + i * step);
        autoSizes = java.util.Arrays.stream(autoSizes).distinct().toArray();
        autoSizeGranularity = Math.round(step);
        autoSizeType = AUTO_SIZE_TEXT_TYPE_UNIFORM;
        textChanged();
    }
    private void clearAutoSize() { autoSizeType = AUTO_SIZE_TEXT_TYPE_NONE; autoSizes = new int[0]; autoSizeGranularity = -1; }
    public int getAutoSizeTextType() { return autoSizeType; }
    public int getAutoSizeStepGranularity() { return autoSizeGranularity; }
    public int getAutoSizeMinTextSize() { return autoSizes.length > 0 ? autoSizes[0] : -1; }
    public int getAutoSizeMaxTextSize() { return autoSizes.length > 0 ? autoSizes[autoSizes.length - 1] : -1; }
    public int[] getAutoSizeTextAvailableSizes() { return autoSizes.clone(); }

    // ---- compound drawables ----
    public void setCompoundDrawables(Drawable left, Drawable top, Drawable right, Drawable bottom) {
        Drawable[] d = {left, top, right, bottom};
        for (int i = 0; i < 4; ++i) {
            if (compound[i] != null && compound[i] != d[i]) compound[i].setCallback(null);
            compound[i] = d[i];
            if (d[i] != null) {
                d[i].setCallback(this);
                if (compoundTint != null) d[i].setTintList(compoundTint);
                if (compoundTintMode != null) d[i].setTintMode(compoundTintMode);
                if (d[i].isStateful()) d[i].setState(getDrawableState());
            }
        }
        textChanged();
    }
    public void setCompoundDrawablesWithIntrinsicBounds(Drawable left, Drawable top, Drawable right, Drawable bottom) {
        for (Drawable d : new Drawable[] {left, top, right, bottom}) if (d != null) d.setBounds(0, 0, d.getIntrinsicWidth(), d.getIntrinsicHeight());
        setCompoundDrawables(left, top, right, bottom);
    }
    public void setCompoundDrawablesWithIntrinsicBounds(int left, int top, int right, int bottom) {
        setCompoundDrawablesWithIntrinsicBounds(load(left), load(top), load(right), load(bottom));
    }
    private Drawable load(int id) { return id != 0 ? getContext().getDrawable(id) : null; }
    public void setCompoundDrawablesRelative(Drawable start, Drawable top, Drawable end, Drawable bottom) { setCompoundDrawables(start, top, end, bottom); }
    public void setCompoundDrawablesRelativeWithIntrinsicBounds(Drawable start, Drawable top, Drawable end, Drawable bottom) { setCompoundDrawablesWithIntrinsicBounds(start, top, end, bottom); }
    public void setCompoundDrawablesRelativeWithIntrinsicBounds(int start, int top, int end, int bottom) { setCompoundDrawablesWithIntrinsicBounds(start, top, end, bottom); }
    public Drawable[] getCompoundDrawables() { return compound.clone(); }
    public Drawable[] getCompoundDrawablesRelative() { return compound.clone(); }
    public void setCompoundDrawablePadding(int pad) { drawablePadding = pad; textChanged(); }
    public int getCompoundDrawablePadding() { return drawablePadding; }
    public void setCompoundDrawableTintList(ColorStateList tint) { compoundTint = tint; for (Drawable d : compound) if (d != null) d.setTintList(tint); }
    public ColorStateList getCompoundDrawableTintList() { return compoundTint; }
    public void setCompoundDrawableTintMode(PorterDuff.Mode tintMode) { compoundTintMode = tintMode; for (Drawable d : compound) if (d != null) d.setTintMode(tintMode); }
    public PorterDuff.Mode getCompoundDrawableTintMode() { return compoundTintMode; }
    @Override protected boolean verifyDrawable(Drawable who) {
        for (Drawable d : compound) if (d == who) return true;
        return super.verifyDrawable(who);
    }
    private int side(int i, boolean horizontal) {
        Drawable d = compound[i];
        if (d == null) return 0;
        return (horizontal ? d.getBounds().width() : d.getBounds().height()) + drawablePadding;
    }
    public int getCompoundPaddingLeft() { return getPaddingLeft() + side(0, true); }
    public int getCompoundPaddingTop() { return getPaddingTop() + side(1, false); }
    public int getCompoundPaddingRight() { return getPaddingRight() + side(2, true); }
    public int getCompoundPaddingBottom() { return getPaddingBottom() + side(3, false); }
    public int getCompoundPaddingStart() { return getCompoundPaddingLeft(); }
    public int getCompoundPaddingEnd() { return getCompoundPaddingRight(); }
    public int getTotalPaddingLeft() { return getCompoundPaddingLeft(); }
    public int getTotalPaddingRight() { return getCompoundPaddingRight(); }
    public int getTotalPaddingStart() { return getTotalPaddingLeft(); }
    public int getTotalPaddingEnd() { return getTotalPaddingRight(); }
    public int getTotalPaddingTop() { return getCompoundPaddingTop() + verticalOffset(); }
    public int getTotalPaddingBottom() {
        int inner = getHeight() - getCompoundPaddingTop() - getCompoundPaddingBottom();
        return getCompoundPaddingBottom() + (layout == null ? 0 : Math.max(0, inner - layout.getHeight()) - verticalOffset());
    }
    private int verticalOffset() {
        if (layout == null) return 0;
        int space = getHeight() - getCompoundPaddingTop() - getCompoundPaddingBottom() - shownHeight();
        if (space <= 0) return 0;
        int g = gravity & Gravity.VERTICAL_GRAVITY_MASK;
        return g == Gravity.BOTTOM ? space : g == Gravity.CENTER_VERTICAL ? space / 2 : 0;
    }

    // ---- measure and draw ----
    /** The text that the layout shows: the hint for empty text, through the transformation and the caps. */
    private CharSequence shownText() {
        CharSequence t = text.length() == 0 && hint != null ? hint : text;
        if (transformation != null) t = transformation.getTransformation(t, this);
        if (allCaps) t = t.toString().toUpperCase(getTextLocale());
        if (singleLine && transformation == null && TextUtils.indexOf(t, '\n') >= 0) t = t.toString().replace('\n', ' ');
        return t;
    }
    private Layout makeLayout(CharSequence t, int width) {
        // the text alignment when the view has one, else the gravity's, as Android's TextView
        boolean rtl = getLayoutDirection() == LAYOUT_DIRECTION_RTL;
        Layout.Alignment align = switch (getTextAlignment()) {
            case TEXT_ALIGNMENT_TEXT_START -> Layout.Alignment.ALIGN_NORMAL;
            case TEXT_ALIGNMENT_TEXT_END -> Layout.Alignment.ALIGN_OPPOSITE;
            case TEXT_ALIGNMENT_CENTER -> Layout.Alignment.ALIGN_CENTER;
            case TEXT_ALIGNMENT_VIEW_START -> rtl ? Layout.Alignment.ALIGN_OPPOSITE : Layout.Alignment.ALIGN_NORMAL;
            case TEXT_ALIGNMENT_VIEW_END -> rtl ? Layout.Alignment.ALIGN_NORMAL : Layout.Alignment.ALIGN_OPPOSITE;
            default -> switch (Gravity.getAbsoluteGravity(gravity, getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK) {
                case Gravity.CENTER_HORIZONTAL -> Layout.Alignment.ALIGN_CENTER;
                case Gravity.RIGHT -> Layout.Alignment.ALIGN_OPPOSITE;
                default -> Layout.Alignment.ALIGN_NORMAL;
            };
        };
        StaticLayout.Builder b = StaticLayout.Builder.obtain(t, 0, t.length(), paint, Math.max(0, width))
            .setAlignment(align).setLineSpacing(spacingAdd, spacingMult).setIncludePad(includePad)
            .setBreakStrategy(breakStrategy).setHyphenationFrequency(hyphenationFrequency)
            .setJustificationMode(justificationMode);
        if (maxLines != Integer.MAX_VALUE) b.setMaxLines(maxLines);
        if (ellipsize != null) b.setEllipsize(ellipsize).setEllipsizedWidth(width);
        return b.build();
    }
    private int shownHeight() {
        int lines = layout.getLineCount();
        int h = lines > maxLines ? layout.getLineTop(maxLines) : layout.getHeight();
        if (lines < minLines) h += (minLines - lines) * getLineHeight();
        return h;
    }
    private static int textWidth(Layout l) {
        float w = 0;
        for (int i = 0; i < l.getLineCount(); ++i) w = Math.max(w, l.getLineWidth(i));
        return (int) Math.ceil(w);
    }
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int wmode = MeasureSpec.getMode(widthMeasureSpec), wsize = MeasureSpec.getSize(widthMeasureSpec);
        int hmode = MeasureSpec.getMode(heightMeasureSpec), hsize = MeasureSpec.getSize(heightMeasureSpec);
        int padH = getCompoundPaddingLeft() + getCompoundPaddingRight(), padV = getCompoundPaddingTop() + getCompoundPaddingBottom();
        int avail = wmode == MeasureSpec.UNSPECIFIED || horizontallyScrolling ? Integer.MAX_VALUE / 4 : Math.max(0, wsize - padH);
        avail = Math.min(avail, maxWidth == Integer.MAX_VALUE ? avail : Math.max(0, maxWidth - padH));
        if (maxEms > 0) avail = Math.min(avail, maxEms * getLineHeight());
        CharSequence t = shownText();
        if (autoSizeType == AUTO_SIZE_TEXT_TYPE_UNIFORM && wmode != MeasureSpec.UNSPECIFIED && hmode != MeasureSpec.UNSPECIFIED && autoSizes.length > 0) {
            // the largest available size whose layout fits in the space that the parent gives
            int availH = Math.max(0, hsize - padV);
            for (int i = autoSizes.length - 1; i >= 0; --i) {
                paint.setTextSize(autoSizes[i]);
                Layout l = makeLayout(t, avail);
                if (i == 0 || (l.getHeight() <= availH && l.getLineCount() <= maxLines && textWidth(l) <= avail)) break;
            }
        }
        layout = makeLayout(t, avail);
        int w = textWidth(layout) + padH;
        int h = shownHeight() + padV;
        if (compound[0] != null) h = Math.max(h, compound[0].getBounds().height() + getPaddingTop() + getPaddingBottom());
        if (compound[2] != null) h = Math.max(h, compound[2].getBounds().height() + getPaddingTop() + getPaddingBottom());
        w = Math.max(w, Math.max(getSuggestedMinimumWidth(), minEms > 0 ? minEms * getLineHeight() + padH : minWidth));
        h = Math.max(Math.min(h, maxHeight), Math.max(getSuggestedMinimumHeight(), minHeight));
        if (wmode != MeasureSpec.UNSPECIFIED && w > wsize) w = wsize;
        setMeasuredDimension(resolveSize(w, widthMeasureSpec), resolveSize(h, heightMeasureSpec));
        int inner = Math.max(0, getMeasuredWidth() - padH);
        if (inner != avail && !horizontallyScrolling) layout = makeLayout(t, inner);
    }
    public int getOffsetForPosition(float x, float y) {
        if (layout == null) return -1;
        int line = layout.getLineForVertical((int) (y - getTotalPaddingTop() + getScrollY()));
        return layout.getOffsetForHorizontal(line, x - getTotalPaddingLeft() + getScrollX());
    }
    @Override protected void onDraw(Canvas canvas) {
        int w = getWidth(), h = getHeight();
        int top = getCompoundPaddingTop(), bottom = h - getCompoundPaddingBottom();
        int left = getCompoundPaddingLeft(), right = w - getCompoundPaddingRight();
        drawCompound(canvas, 0, getPaddingLeft(), top + (bottom - top - height(0)) / 2);
        drawCompound(canvas, 2, w - getPaddingRight() - width(2), top + (bottom - top - height(2)) / 2);
        drawCompound(canvas, 1, left + (right - left - width(1)) / 2, getPaddingTop());
        drawCompound(canvas, 3, left + (right - left - width(3)) / 2, h - getPaddingBottom() - height(3));
        if (layout == null) return;
        boolean showHint = text.length() == 0 && hint != null && hintColors != null;
        int color = paint.getColor();
        if (showHint) paint.setColor(hintColors.getColorForState(getDrawableState(), hintColors.getDefaultColor()));
        canvas.save();
        canvas.clipRect(left + getScrollX(), getPaddingTop() + getScrollY(), right + getScrollX(), h - getPaddingBottom() + getScrollY());
        canvas.translate(left, top + verticalOffset());
        layout.draw(canvas);
        canvas.restore();
        paint.setColor(color);
    }
    private int width(int i) { return compound[i] != null ? compound[i].getBounds().width() : 0; }
    private int height(int i) { return compound[i] != null ? compound[i].getBounds().height() : 0; }
    private void drawCompound(Canvas canvas, int i, int x, int y) {
        Drawable d = compound[i];
        if (d == null) return;
        canvas.save();
        canvas.translate(x - d.getBounds().left, y - d.getBounds().top);
        d.draw(canvas);
        canvas.restore();
    }
    @Override public CharSequence getAccessibilityClassName() { return TextView.class.getName(); }
}
