// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import android.os.Bundle;
import java.util.List;

/** A view's virtual accessibility tree. */
public abstract class AccessibilityNodeProvider {
    public static final int HOST_VIEW_ID = -1;

    public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) { return null; }
    public void addExtraDataToAccessibilityNodeInfo(int virtualViewId, AccessibilityNodeInfo info, String extraDataKey, Bundle arguments) {}
    public boolean performAction(int virtualViewId, int action, Bundle arguments) { return false; }
    public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String text, int virtualViewId) { return null; }
    public AccessibilityNodeInfo findFocus(int focus) { return null; }
}
