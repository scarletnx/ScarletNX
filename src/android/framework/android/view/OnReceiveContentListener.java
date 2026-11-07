// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Receives content (pasted, dropped or from the input method) for a view. */
public interface OnReceiveContentListener {
    ContentInfo onReceiveContent(View view, ContentInfo payload);
}
