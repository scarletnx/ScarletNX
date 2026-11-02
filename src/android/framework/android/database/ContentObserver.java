// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

import android.net.Uri;
import android.os.Handler;

/** Told when content changes. */
public abstract class ContentObserver {
    private final Handler handler;

    public ContentObserver(Handler handler) { this.handler = handler; }
    public boolean deliverSelfNotifications() { return false; }
    public void onChange(boolean selfChange) {}
    public void onChange(boolean selfChange, Uri uri) { onChange(selfChange); }
    public void onChange(boolean selfChange, Uri uri, int flags) { onChange(selfChange, uri); }
    public void onChange(boolean selfChange, java.util.Collection<Uri> uris, int flags) { for (Uri u : uris) onChange(selfChange, u, flags); }
    @Deprecated public final void dispatchChange(boolean selfChange) { dispatchChange(selfChange, null); }
    public final void dispatchChange(boolean selfChange, Uri uri) { dispatchChange(selfChange, uri, 0); }
    public final void dispatchChange(boolean selfChange, Uri uri, int flags) {
        if (handler == null) onChange(selfChange, uri, flags);
        else handler.post(() -> onChange(selfChange, uri, flags));
    }
}
