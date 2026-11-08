// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** A dialog, as its listeners see it. */
public interface DialogInterface {
    int BUTTON_POSITIVE = -1, BUTTON_NEGATIVE = -2, BUTTON_NEUTRAL = -3;
    void cancel();
    void dismiss();
    interface OnCancelListener { void onCancel(DialogInterface dialog); }
    interface OnDismissListener { void onDismiss(DialogInterface dialog); }
    interface OnShowListener { void onShow(DialogInterface dialog); }
    interface OnClickListener { void onClick(DialogInterface dialog, int which); }
    interface OnMultiChoiceClickListener { void onClick(DialogInterface dialog, int which, boolean isChecked); }
    interface OnKeyListener { boolean onKey(DialogInterface dialog, int keyCode, android.view.KeyEvent event); }
}
