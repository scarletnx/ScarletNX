// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.database.Cursor;

/** Gives the cursor of a filtered query. */
public interface FilterQueryProvider {
    Cursor runQuery(CharSequence constraint);
}
