// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;

/** A column of radio buttons of which one at most is checked. */
public class RadioGroup extends LinearLayout {
    private int checkedId = View.NO_ID;
    private boolean protectFromCheckedChange;
    private OnCheckedChangeListener onCheckedChangeListener;
    private final CompoundButton.OnCheckedChangeListener childListener = (button, isChecked) -> {
        if (protectFromCheckedChange) return;
        protectFromCheckedChange = true;
        if (checkedId != View.NO_ID) setCheckedStateForView(checkedId, false);
        protectFromCheckedChange = false;
        setCheckedId(button.getId());
    };

    public interface OnCheckedChangeListener { void onCheckedChanged(RadioGroup group, int checkedId); }

    public RadioGroup(Context context) { super(context); setOrientation(VERTICAL); }
    public RadioGroup(Context context, AttributeSet attrs) {
        super(context, attrs);
        if (attrs == null || attrs.getAttributeValue("http://schemas.android.com/apk/res/android", "orientation") == null) setOrientation(VERTICAL);
    }

    @Override protected void onViewAdded(View child) {
        super.onViewAdded(child);
        if (!(child instanceof RadioButton b)) return;
        if (b.getId() == View.NO_ID) b.setId(View.generateViewId());
        b.widgetListener = childListener;
        if (b.isChecked()) {
            protectFromCheckedChange = true;
            if (checkedId != View.NO_ID) setCheckedStateForView(checkedId, false);
            protectFromCheckedChange = false;
            setCheckedId(b.getId());
        }
    }
    @Override protected void onViewRemoved(View child) {
        if (child instanceof RadioButton b) b.widgetListener = null;
        super.onViewRemoved(child);
    }

    public void check(int id) {
        if (id != View.NO_ID && id == checkedId) return;
        if (checkedId != View.NO_ID) setCheckedStateForView(checkedId, false);
        if (id != View.NO_ID) setCheckedStateForView(id, true);
        setCheckedId(id);
    }
    public void clearCheck() { check(View.NO_ID); }
    public int getCheckedRadioButtonId() { return checkedId; }
    public void setOnCheckedChangeListener(OnCheckedChangeListener listener) { onCheckedChangeListener = listener; }
    private void setCheckedId(int id) {
        if (id == checkedId) return;
        checkedId = id;
        if (onCheckedChangeListener != null) onCheckedChangeListener.onCheckedChanged(this, id);
    }
    private void setCheckedStateForView(int viewId, boolean checked) {
        View v = findViewById(viewId);
        if (v instanceof RadioButton b) {
            boolean old = protectFromCheckedChange;
            protectFromCheckedChange = true;
            b.setChecked(checked);
            protectFromCheckedChange = old;
        }
    }
    @Override public CharSequence getAccessibilityClassName() { return RadioGroup.class.getName(); }
}
