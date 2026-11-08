// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A search request's event. */
public class SearchEvent {
    private final InputDevice device;
    public SearchEvent(InputDevice inputDevice) { device = inputDevice; }
    public InputDevice getInputDevice() { return device; }
}
