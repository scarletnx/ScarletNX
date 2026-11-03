// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** A dialog of a title, a message or a view, and up to three buttons, laid out in a column. */
public class AlertDialog extends Dialog implements DialogInterface {
    private CharSequence title, message;
    private View view;
    private TextView titleView, messageView;
    private final CharSequence[] buttonText = new CharSequence[3];
    private final OnClickListener[] buttonListener = new OnClickListener[3];
    private final Button[] buttons = new Button[3];

    protected AlertDialog(Context context) { this(context, 0); }
    protected AlertDialog(Context context, int themeResId) { super(context, themeResId); }
    protected AlertDialog(Context context, boolean cancelable, OnCancelListener cancelListener) { super(context, cancelable, cancelListener); }

    @Override public void setTitle(CharSequence title) { this.title = title; super.setTitle(title); display(titleView, title); }
    public void setMessage(CharSequence message) { this.message = message; display(messageView, message); }
    public void setView(View view) { this.view = view; }
    public void setView(View view, int viewSpacingLeft, int viewSpacingTop, int viewSpacingRight, int viewSpacingBottom) {
        if (view != null) view.setPadding(viewSpacingLeft, viewSpacingTop, viewSpacingRight, viewSpacingBottom);
        this.view = view;
    }
    public void setButton(int whichButton, CharSequence text, OnClickListener listener) {
        int i = slot(whichButton);
        buttonText[i] = text;
        buttonListener[i] = listener;
    }
    /** The button of BUTTON_POSITIVE, BUTTON_NEGATIVE or BUTTON_NEUTRAL, or null before onCreate or when it has no text. */
    public Button getButton(int whichButton) { return buttons[slot(whichButton)]; }
    public void setIcon(int resId) {}
    public void setIcon(android.graphics.drawable.Drawable icon) {}

    private static int slot(int which) { return -1 - which; }
    private static void display(TextView v, CharSequence text) {
        if (v == null) return;
        v.setText(text);
        v.setVisibility(text != null && text.length() > 0 ? View.VISIBLE : View.GONE);
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Context c = getContext();
        int pad = (int) (24 * c.getResources().getDisplayMetrics().density);
        LinearLayout root = new LinearLayout(c);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad / 3);
        titleView = new TextView(c);
        titleView.setTextSize(20);
        display(titleView, title);
        root.addView(titleView);
        messageView = new TextView(c);
        display(messageView, message);
        root.addView(messageView);
        if (view != null) root.addView(view);
        LinearLayout bar = new LinearLayout(c);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        for (int which : new int[] {BUTTON_NEUTRAL, BUTTON_NEGATIVE, BUTTON_POSITIVE}) {
            int i = slot(which);
            if (buttonText[i] == null) continue;
            Button b = new Button(c);
            b.setText(buttonText[i]);
            b.setOnClickListener(v -> {
                if (buttonListener[i] != null) buttonListener[i].onClick(this, which);
                dismiss();
            });
            buttons[i] = b;
            bar.addView(b);
        }
        root.addView(bar);
        setContentView(root);
    }

    public static class Builder {
        private final Context context;
        private final int themeResId;
        private CharSequence title, message;
        private View view;
        private int viewLayoutResId;
        private final CharSequence[] buttonText = new CharSequence[3];
        private final OnClickListener[] buttonListener = new OnClickListener[3];
        private boolean cancelable = true;
        private OnCancelListener onCancel;
        private OnDismissListener onDismiss;
        private OnKeyListener onKey;

        public Builder(Context context) { this(context, 0); }
        public Builder(Context context, int themeResId) { this.context = context; this.themeResId = themeResId; }
        public Context getContext() { return context; }
        public Builder setTitle(CharSequence title) { this.title = title; return this; }
        public Builder setTitle(int titleId) { return setTitle(context.getText(titleId)); }
        public Builder setMessage(CharSequence message) { this.message = message; return this; }
        public Builder setMessage(int messageId) { return setMessage(context.getText(messageId)); }
        public Builder setIcon(int iconId) { return this; }
        public Builder setIcon(android.graphics.drawable.Drawable icon) { return this; }
        private Builder button(int which, CharSequence text, OnClickListener listener) {
            buttonText[slot(which)] = text;
            buttonListener[slot(which)] = listener;
            return this;
        }
        public Builder setPositiveButton(CharSequence text, OnClickListener listener) { return button(BUTTON_POSITIVE, text, listener); }
        public Builder setPositiveButton(int textId, OnClickListener listener) { return setPositiveButton(context.getText(textId), listener); }
        public Builder setNegativeButton(CharSequence text, OnClickListener listener) { return button(BUTTON_NEGATIVE, text, listener); }
        public Builder setNegativeButton(int textId, OnClickListener listener) { return setNegativeButton(context.getText(textId), listener); }
        public Builder setNeutralButton(CharSequence text, OnClickListener listener) { return button(BUTTON_NEUTRAL, text, listener); }
        public Builder setNeutralButton(int textId, OnClickListener listener) { return setNeutralButton(context.getText(textId), listener); }
        public Builder setCancelable(boolean cancelable) { this.cancelable = cancelable; return this; }
        public Builder setOnCancelListener(OnCancelListener onCancelListener) { onCancel = onCancelListener; return this; }
        public Builder setOnDismissListener(OnDismissListener onDismissListener) { onDismiss = onDismissListener; return this; }
        public Builder setOnKeyListener(OnKeyListener onKeyListener) { onKey = onKeyListener; return this; }
        public Builder setView(View view) { this.view = view; viewLayoutResId = 0; return this; }
        public Builder setView(int layoutResId) { view = null; viewLayoutResId = layoutResId; return this; }
        public AlertDialog create() {
            AlertDialog d = new AlertDialog(context, themeResId);
            if (title != null) d.setTitle(title);
            d.setMessage(message);
            d.setView(viewLayoutResId != 0 ? android.view.LayoutInflater.from(d.getContext()).inflate(viewLayoutResId, null) : view);
            for (int i = 0; i < 3; ++i) if (buttonText[i] != null) d.setButton(-1 - i, buttonText[i], buttonListener[i]);
            d.setCancelable(cancelable);
            if (cancelable) d.setCanceledOnTouchOutside(true);
            d.setOnCancelListener(onCancel);
            d.setOnDismissListener(onDismiss);
            d.setOnKeyListener(onKey);
            return d;
        }
        public AlertDialog show() {
            AlertDialog d = create();
            d.show();
            return d;
        }
    }
}
