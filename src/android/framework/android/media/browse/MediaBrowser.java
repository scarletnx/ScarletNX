// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media.browse;

import android.content.ComponentName;
import android.content.Context;
import android.media.MediaDescription;
import android.media.session.MediaSession;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.List;

/**
 * Browses the media of a MediaBrowserService. A browse service gives no binder here, so a connection always fails:
 * the callback hears onConnectionFailed, and the browser stays disconnected.
 */
public final class MediaBrowser {
    public static final String EXTRA_PAGE = "android.media.browse.extra.PAGE", EXTRA_PAGE_SIZE = "android.media.browse.extra.PAGE_SIZE";

    private static final int DISCONNECTED = 0, CONNECTING = 1;

    private final ComponentName serviceComponent;
    private final ConnectionCallback callback;
    private final Handler handler;
    private int state = DISCONNECTED;

    public MediaBrowser(Context context, ComponentName serviceComponent, ConnectionCallback callback, Bundle rootHints) {
        if (context == null) throw new IllegalArgumentException("context must not be null");
        if (serviceComponent == null) throw new IllegalArgumentException("service component must not be null");
        if (callback == null) throw new IllegalArgumentException("connection callback must not be null");
        this.serviceComponent = serviceComponent;
        this.callback = callback;
        Looper looper = Looper.myLooper();
        handler = new Handler(looper != null ? looper : Looper.getMainLooper());
    }

    public void connect() {
        if (state != DISCONNECTED) throw new IllegalStateException("connect() called while neither disconnecting nor disconnected (state=" + state + ")");
        state = CONNECTING;
        handler.post(() -> {
            if (state != CONNECTING) return;
            state = DISCONNECTED;
            callback.onConnectionFailed();
        });
    }

    public void disconnect() { state = DISCONNECTED; }
    public boolean isConnected() { return false; }

    public ComponentName getServiceComponent() { throw new IllegalStateException("getServiceComponent() called while not connected (state=" + state + ")"); }
    public String getRoot() { throw new IllegalStateException("getRoot() called while not connected(state=" + state + ")"); }
    public Bundle getExtras() { throw new IllegalStateException("getExtras() called while not connected (state=" + state + ")"); }
    public MediaSession.Token getSessionToken() { throw new IllegalStateException("getSessionToken() called while not connected(state=" + state + ")"); }

    public void subscribe(String parentId, SubscriptionCallback callback) { subscribe(parentId, null, callback); }

    /** A subscription waits for a connection, which never comes. */
    public void subscribe(String parentId, Bundle options, SubscriptionCallback callback) {
        if (parentId == null || parentId.isEmpty()) throw new IllegalArgumentException("parentId cannot be empty.");
        if (callback == null) throw new IllegalArgumentException("callback cannot be null");
    }

    public void unsubscribe(String parentId) {
        if (parentId == null || parentId.isEmpty()) throw new IllegalArgumentException("parentId cannot be empty.");
    }

    public void unsubscribe(String parentId, SubscriptionCallback callback) {
        if (parentId == null || parentId.isEmpty()) throw new IllegalArgumentException("parentId cannot be empty.");
        if (callback == null) throw new IllegalArgumentException("callback cannot be null");
    }

    public void getItem(String mediaId, ItemCallback cb) {
        if (mediaId == null || mediaId.isEmpty()) throw new IllegalArgumentException("mediaId cannot be empty.");
        if (cb == null) throw new IllegalArgumentException("cb cannot be null.");
        handler.post(() -> cb.onError(mediaId));
    }

    public static class MediaItem implements Parcelable {
        public static final int FLAG_BROWSABLE = 1, FLAG_PLAYABLE = 2;

        private final int flags;
        private final MediaDescription description;

        public MediaItem(MediaDescription description, int flags) {
            if (description == null) throw new IllegalArgumentException("description cannot be null");
            if (description.getMediaId() == null || description.getMediaId().isEmpty()) throw new IllegalArgumentException("description must have a non-empty media id");
            this.flags = flags;
            this.description = description;
        }

        public int getFlags() { return flags; }
        public boolean isBrowsable() { return (flags & FLAG_BROWSABLE) != 0; }
        public boolean isPlayable() { return (flags & FLAG_PLAYABLE) != 0; }
        public MediaDescription getDescription() { return description; }
        public String getMediaId() { return description.getMediaId(); }

        @Override public String toString() { return "MediaItem{mFlags=" + flags + ", mDescription=" + description + "}"; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel out, int flags) { out.writeInt(this.flags); description.writeToParcel(out, flags); }
        public static final Parcelable.Creator<MediaItem> CREATOR = new Parcelable.Creator<>() {
            @Override public MediaItem createFromParcel(Parcel in) { int f = in.readInt(); return new MediaItem(MediaDescription.CREATOR.createFromParcel(in), f); }
            @Override public MediaItem[] newArray(int size) { return new MediaItem[size]; }
        };
    }

    public static class ConnectionCallback {
        public ConnectionCallback() {}
        public void onConnected() {}
        public void onConnectionSuspended() {}
        public void onConnectionFailed() {}
    }

    public abstract static class SubscriptionCallback {
        public SubscriptionCallback() {}
        public void onChildrenLoaded(String parentId, List<MediaItem> children) {}
        public void onChildrenLoaded(String parentId, List<MediaItem> children, Bundle options) {}
        public void onError(String parentId) {}
        public void onError(String parentId, Bundle options) {}
    }

    public abstract static class ItemCallback {
        public ItemCallback() {}
        public void onItemLoaded(MediaItem item) {}
        public void onError(String mediaId) {}
    }
}
