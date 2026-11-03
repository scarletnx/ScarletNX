// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.input;

import android.os.Handler;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.VerifiedInputEvent;

/** The input devices. Events come from the host's window system, and no event carries a system signature. */
public final class InputManager {
    public static final String ACTION_QUERY_KEYBOARD_LAYOUTS = "android.hardware.input.action.QUERY_KEYBOARD_LAYOUTS";

    public interface InputDeviceListener {
        void onInputDeviceAdded(int deviceId);
        void onInputDeviceRemoved(int deviceId);
        void onInputDeviceChanged(int deviceId);
    }

    public InputManager() {}

    /** Null, as for an event that the system did not sign. */
    public VerifiedInputEvent verifyInputEvent(InputEvent event) { return null; }
    public int[] getInputDeviceIds() { return new int[0]; }
    public InputDevice getInputDevice(int id) { return null; }
    public void registerInputDeviceListener(InputDeviceListener listener, Handler handler) {}
    public void unregisterInputDeviceListener(InputDeviceListener listener) {}
}
