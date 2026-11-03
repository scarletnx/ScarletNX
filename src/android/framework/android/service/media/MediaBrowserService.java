// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.media;

import android.app.Service;
import android.content.Intent;
import android.media.browse.MediaBrowser;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.os.Bundle;
import android.os.IBinder;
import java.util.List;

/**
 * A media app's browse service. No other app connects to it, so no client calls onGetRoot or onLoadChildren, and a
 * change of children notifies nobody.
 */
public abstract class MediaBrowserService extends Service {
    public static final String SERVICE_INTERFACE = "android.media.browse.MediaBrowserService";
    private MediaSession.Token sessionToken;

    public MediaBrowserService() {}
    @Override public void onCreate() { super.onCreate(); }
    @Override public IBinder onBind(Intent intent) { return null; }

    public abstract BrowserRoot onGetRoot(String clientPackageName, int clientUid, Bundle rootHints);
    public abstract void onLoadChildren(String parentId, Result<List<MediaBrowser.MediaItem>> result);
    public void onLoadChildren(String parentId, Result<List<MediaBrowser.MediaItem>> result, Bundle options) {
        onLoadChildren(parentId, result);
    }
    public void onLoadItem(String itemId, Result<MediaBrowser.MediaItem> result) {
        result.sendResult(null);
    }

    public void setSessionToken(MediaSession.Token token) {
        if (token == null) throw new IllegalArgumentException("Session token may not be null.");
        if (sessionToken != null) throw new IllegalStateException("The session token has already been set.");
        sessionToken = token;
    }
    public MediaSession.Token getSessionToken() { return sessionToken; }

    /** Android throws outside onGetRoot, onLoadChildren and onLoadItem. No client calls them here, so each call is outside. */
    public final Bundle getBrowserRootHints() {
        throw new IllegalStateException("This should be called inside of onGetRoot or onLoadChildren or onLoadItem methods");
    }
    public final MediaSessionManager.RemoteUserInfo getCurrentBrowserInfo() {
        throw new IllegalStateException("This should be called inside of onGetRoot or onLoadChildren or onLoadItem methods");
    }

    public void notifyChildrenChanged(String parentId) { notifyChildrenChanged(parentId, null); }
    public void notifyChildrenChanged(String parentId, Bundle options) {
        if (parentId == null) throw new IllegalArgumentException("parentId cannot be null in notifyChildrenChanged");
    }

    /** The root of the browse tree that a client sees. */
    public static final class BrowserRoot {
        public static final String EXTRA_RECENT = "android.service.media.extra.RECENT";
        public static final String EXTRA_OFFLINE = "android.service.media.extra.OFFLINE";
        public static final String EXTRA_SUGGESTED = "android.service.media.extra.SUGGESTED";
        private final String rootId;
        private final Bundle extras;

        public BrowserRoot(String rootId, Bundle extras) {
            if (rootId == null) throw new IllegalArgumentException("The root id in BrowserRoot cannot be null. Use null for BrowserRoot instead.");
            this.rootId = rootId; this.extras = extras;
        }
        public String getRootId() { return rootId; }
        public Bundle getExtras() { return extras; }
    }

    /** The result of a load, which the service can send later after detach. */
    public class Result<T> {
        private final Object debug;
        private boolean detached, sent;

        Result(Object debug) { this.debug = debug; }
        public void sendResult(T result) {
            if (sent) throw new IllegalStateException("sendResult() called twice for: " + debug);
            sent = true;
        }
        public void detach() {
            if (detached) throw new IllegalStateException("detach() called when detach() had already been called for: " + debug);
            if (sent) throw new IllegalStateException("detach() called when sendResult() had already been called for: " + debug);
            detached = true;
        }
    }
}
