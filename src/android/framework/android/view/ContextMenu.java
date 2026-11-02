// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A context menu. */
public interface ContextMenu extends Menu {
    interface ContextMenuInfo {}
    ContextMenu setHeaderTitle(CharSequence title);
    void clearHeader();
}
