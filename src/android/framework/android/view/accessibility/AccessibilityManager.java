// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.accessibility;

import java.util.Collections;
import java.util.List;

/** Accessibility services: none run, so it is off. */
public final class AccessibilityManager {
    public static final int FLAG_CONTENT_ICONS = 1, FLAG_CONTENT_TEXT = 2, FLAG_CONTENT_CONTROLS = 4;

    public interface AccessibilityStateChangeListener { void onAccessibilityStateChanged(boolean enabled); }
    public interface TouchExplorationStateChangeListener { void onTouchExplorationStateChanged(boolean enabled); }
    public interface AccessibilityServicesStateChangeListener { void onAccessibilityServicesStateChanged(AccessibilityManager manager); }
    public interface HighTextContrastChangeListener { void onHighTextContrastStateChanged(boolean enabled); }

    public boolean isEnabled() { return false; }
    public boolean isTouchExplorationEnabled() { return false; }
    public boolean isHighTextContrastEnabled() { return false; }
    public boolean isAudioDescriptionRequested() { return false; }
    public boolean isRequestFromAccessibilityTool() { return false; }
    public void sendAccessibilityEvent(AccessibilityEvent event) {}
    public void interrupt() {}
    public List<Object> getInstalledAccessibilityServiceList() { return Collections.emptyList(); }
    public List<Object> getEnabledAccessibilityServiceList(int feedbackTypeFlags) { return Collections.emptyList(); }
    public boolean addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) { return true; }
    public void addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener, android.os.Handler handler) {}
    public boolean removeAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) { return true; }
    public boolean addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) { return true; }
    public void addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener, android.os.Handler handler) {}
    public boolean removeTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) { return true; }
    public void addAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) {}
    public void addAccessibilityServicesStateChangeListener(java.util.concurrent.Executor executor, AccessibilityServicesStateChangeListener listener) {}
    public boolean removeAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) { return true; }
    public int getRecommendedTimeoutMillis(int originalTimeout, int uiContentFlags) { return originalTimeout; }
}
