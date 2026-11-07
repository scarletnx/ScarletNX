// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** A cursor that fills a window with its rows. */
public interface CrossProcessCursor extends Cursor {
    CursorWindow getWindow();

    void fillWindow(int position, CursorWindow window);

    boolean onMove(int oldPosition, int newPosition);
}
