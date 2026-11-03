// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.Intent;
import android.view.View;

/**
 * The share action of a menu, which lists the apps that take a share intent. No other apps are installed beside the
 * app here, so the list has no targets: the action view is empty and has no submenu.
 */
public class ShareActionProvider extends android.view.ActionProvider {
    public static final String DEFAULT_SHARE_HISTORY_FILE_NAME = "share_history.xml";
    private final Context context;
    private String historyFileName = DEFAULT_SHARE_HISTORY_FILE_NAME;
    private Intent shareIntent;
    private OnShareTargetSelectedListener listener;

    public interface OnShareTargetSelectedListener { boolean onShareTargetSelected(ShareActionProvider source, Intent intent); }

    public ShareActionProvider(Context context) { super(context); this.context = context; }

    public void setOnShareTargetSelectedListener(OnShareTargetSelectedListener listener) { this.listener = listener; }
    @Override public View onCreateActionView() { View v = new View(context); v.setVisibility(View.GONE); return v; }
    @Override public boolean hasSubMenu() { return false; }
    public void setShareHistoryFileName(String shareHistoryFile) { historyFileName = shareHistoryFile; }
    public void setShareIntent(Intent shareIntent) { this.shareIntent = shareIntent; }
}
