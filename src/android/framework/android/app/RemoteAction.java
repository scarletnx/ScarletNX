// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.graphics.drawable.Icon;

/** An action that another surface (a picture-in-picture window, a text selection) offers to start. */
public final class RemoteAction implements android.os.Parcelable {
    private final Icon icon;
    private final CharSequence title, contentDescription;
    private final PendingIntent actionIntent;
    private boolean enabled = true, showIcon = true;

    public RemoteAction(Icon icon, CharSequence title, CharSequence contentDescription, PendingIntent intent) {
        if (icon == null || title == null || contentDescription == null || intent == null)
            throw new IllegalArgumentException("Expected icon, title, content description and action callback");
        this.icon = icon;
        this.title = title;
        this.contentDescription = contentDescription;
        actionIntent = intent;
    }
    public Icon getIcon() { return icon; }
    public CharSequence getTitle() { return title; }
    public CharSequence getContentDescription() { return contentDescription; }
    public PendingIntent getActionIntent() { return actionIntent; }
    public void setEnabled(boolean enabled) { this.enabled = enabled; }
    public boolean isEnabled() { return enabled; }
    public void setShouldShowIcon(boolean shouldShowIcon) { showIcon = shouldShowIcon; }
    public boolean shouldShowIcon() { return showIcon; }
    @Override public RemoteAction clone() {
        RemoteAction a = new RemoteAction(icon, title, contentDescription, actionIntent);
        a.enabled = enabled;
        a.showIcon = showIcon;
        return a;
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
