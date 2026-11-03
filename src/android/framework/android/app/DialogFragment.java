// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.DialogInterface;
import android.os.Bundle;
import android.view.View;

/** A platform fragment (deprecated) that shows a dialog while it is started. */
@Deprecated
public class DialogFragment extends Fragment implements DialogInterface.OnCancelListener, DialogInterface.OnDismissListener {
    public static final int STYLE_NORMAL = 0, STYLE_NO_TITLE = 1, STYLE_NO_FRAME = 2, STYLE_NO_INPUT = 3;
    private int theme;
    private boolean cancelable = true, showsDialog = true, dismissed = true, destroyed;
    private Dialog dialog;

    public DialogFragment() {}
    public void setStyle(int style, int theme) { this.theme = theme; }
    public int getTheme() { return theme; }
    public void setCancelable(boolean cancelable) { this.cancelable = cancelable; if (dialog != null) dialog.setCancelable(cancelable); }
    public boolean isCancelable() { return cancelable; }
    public void setShowsDialog(boolean showsDialog) { this.showsDialog = showsDialog; }
    public boolean getShowsDialog() { return showsDialog; }
    public Dialog getDialog() { return dialog; }
    public Dialog onCreateDialog(Bundle savedInstanceState) { return new Dialog(getActivity(), getTheme()); }

    public void show(FragmentManager manager, String tag) {
        dismissed = false;
        manager.beginTransaction().add(this, tag).commit();
    }
    public int show(FragmentTransaction transaction, String tag) {
        dismissed = false;
        return transaction.add(this, tag).commit();
    }
    public void dismiss() { dismissInternal(); }
    public void dismissAllowingStateLoss() { dismissInternal(); }
    private void dismissInternal() {
        if (dismissed) return;
        dismissed = true;
        if (dialog != null) dialog.dismiss();
        FragmentManager m = getFragmentManager();
        if (m != null) m.remove(this);
    }

    @Override public void onActivityCreated(Bundle savedInstanceState) {
        super.onActivityCreated(savedInstanceState);
        if (!showsDialog) return;
        destroyed = false;
        dialog = onCreateDialog(savedInstanceState);
        View view = getView();
        if (view != null) dialog.setContentView(view);
        dialog.setOwnerActivity(getActivity());
        dialog.setCancelable(cancelable);
        dialog.setOnCancelListener(this);
        dialog.setOnDismissListener(this);
    }
    @Override public void onStart() {
        super.onStart();
        if (dialog != null) dialog.show();
    }
    @Override public void onStop() {
        super.onStop();
        if (dialog != null) dialog.hide();
    }
    @Override public void onCreate(Bundle savedInstanceState) { super.onCreate(savedInstanceState); }
    @Override public void onDestroy() {
        if (dialog != null) {
            destroyed = true;
            dialog.dismiss();
            dialog = null;
        }
        super.onDestroy();
    }
    @Override public void onDetach() {
        super.onDetach();
        dismissed = true;
    }
    @Override public void onCancel(DialogInterface dialog) {}
    /** The dialog went away by itself (a button, back): the fragment goes too. */
    @Override public void onDismiss(DialogInterface dialog) { if (!destroyed) dismissInternal(); }
}
