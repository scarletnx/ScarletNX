// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A contextual mode of actions over the content (text selection's toolbar). */
public abstract class ActionMode {
    public static final int TYPE_PRIMARY = 0, TYPE_FLOATING = 1;
    private Object tag;

    public interface Callback {
        boolean onCreateActionMode(ActionMode mode, Menu menu);
        boolean onPrepareActionMode(ActionMode mode, Menu menu);
        boolean onActionItemClicked(ActionMode mode, MenuItem item);
        void onDestroyActionMode(ActionMode mode);
    }

    public abstract static class Callback2 implements Callback {
        public void onGetContentRect(ActionMode mode, View view, android.graphics.Rect outRect) {
            outRect.set(0, 0, view.getWidth(), view.getHeight());
        }
    }

    public void setTag(Object tag) { this.tag = tag; }
    public Object getTag() { return tag; }
    public abstract void setTitle(CharSequence title);
    public abstract void setTitle(int resId);
    public abstract void setSubtitle(CharSequence subtitle);
    public abstract void setSubtitle(int resId);
    public abstract void setCustomView(View view);
    public abstract void invalidate();
    public void invalidateContentRect() {}
    public abstract void finish();
    public abstract Menu getMenu();
    public abstract CharSequence getTitle();
    public abstract CharSequence getSubtitle();
    public abstract View getCustomView();
    public abstract MenuInflater getMenuInflater();
    public int getType() { return TYPE_PRIMARY; }
    public void setType(int type) {}
    public void hide(long duration) {}
}
