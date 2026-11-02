// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** Told of a transaction's begin and end. */
public interface SQLiteTransactionListener {
    void onBegin();
    void onCommit();
    void onRollback();
}
