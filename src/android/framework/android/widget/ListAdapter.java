// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

/** An adapter whose items a list can enable or disable. */
public interface ListAdapter extends Adapter {
    boolean areAllItemsEnabled();
    boolean isEnabled(int position);
}
