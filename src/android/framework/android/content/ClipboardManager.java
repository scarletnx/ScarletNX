// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import java.util.concurrent.CopyOnWriteArrayList;

/** The clipboard: the process's own (snx: the host's clipboard is a later step). */
public class ClipboardManager extends android.text.ClipboardManager {
    private ClipData primary;
    private final CopyOnWriteArrayList<OnPrimaryClipChangedListener> listeners = new CopyOnWriteArrayList<>();

    public interface OnPrimaryClipChangedListener { void onPrimaryClipChanged(); }

    public void setPrimaryClip(ClipData clip) {
        primary = clip;
        for (OnPrimaryClipChangedListener l : listeners) l.onPrimaryClipChanged();
    }
    public void clearPrimaryClip() { setPrimaryClip(null); }
    public ClipData getPrimaryClip() { return primary; }
    public ClipDescription getPrimaryClipDescription() { return primary != null ? primary.getDescription() : null; }
    public boolean hasPrimaryClip() { return primary != null; }
    public void addPrimaryClipChangedListener(OnPrimaryClipChangedListener what) { listeners.add(what); }
    public void removePrimaryClipChangedListener(OnPrimaryClipChangedListener what) { listeners.remove(what); }
    @Override @Deprecated public CharSequence getText() { return primary != null && primary.getItemCount() > 0 ? primary.getItemAt(0).getText() : null; }
    @Override @Deprecated public void setText(CharSequence text) { setPrimaryClip(ClipData.newPlainText(null, text)); }
    @Override @Deprecated public boolean hasText() { return getText() != null; }
}
