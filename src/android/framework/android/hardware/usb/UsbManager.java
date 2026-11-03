// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.usb;

/** USB host and accessory mode on a device with no USB devices for the app. */
public class UsbManager {
    public static final String ACTION_USB_DEVICE_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED",
        ACTION_USB_DEVICE_DETACHED = "android.hardware.usb.action.USB_DEVICE_DETACHED",
        ACTION_USB_ACCESSORY_ATTACHED = "android.hardware.usb.action.USB_ACCESSORY_ATTACHED",
        ACTION_USB_ACCESSORY_DETACHED = "android.hardware.usb.action.USB_ACCESSORY_DETACHED",
        EXTRA_DEVICE = "device", EXTRA_ACCESSORY = "accessory", EXTRA_PERMISSION_GRANTED = "permission";

    public UsbManager() {}
}
