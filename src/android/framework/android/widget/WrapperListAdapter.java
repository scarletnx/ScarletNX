// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

/** A list adapter that wraps another one. */
public interface WrapperListAdapter extends ListAdapter {
    ListAdapter getWrappedAdapter();
}
