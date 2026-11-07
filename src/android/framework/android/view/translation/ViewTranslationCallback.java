// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.translation;

/** How a view shows its translation. */
public interface ViewTranslationCallback {
    boolean onShowTranslation(android.view.View view);
    boolean onHideTranslation(android.view.View view);
    boolean onClearTranslation(android.view.View view);
}
