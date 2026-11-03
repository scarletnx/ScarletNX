// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware;

import android.view.SurfaceHolder;
import java.util.LinkedHashMap;

/** The old camera API on a device with no camera: there is none to open. */
@Deprecated
public class Camera {
    public static final String ACTION_NEW_PICTURE = "android.hardware.action.NEW_PICTURE",
        ACTION_NEW_VIDEO = "android.hardware.action.NEW_VIDEO";
    public static final int CAMERA_ERROR_UNKNOWN = 1, CAMERA_ERROR_EVICTED = 2, CAMERA_ERROR_SERVER_DIED = 100;

    private Camera() {}

    public static int getNumberOfCameras() { return 0; }

    public static void getCameraInfo(int cameraId, CameraInfo cameraInfo) {
        throw new RuntimeException("Unknown camera ID " + cameraId);
    }

    /** No camera service has a camera to connect to. */
    public static Camera open(int cameraId) { throw new RuntimeException("Fail to connect to camera service"); }

    /** Null: the device has no back camera. */
    public static Camera open() { return null; }

    public final void release() {}
    public final void setPreviewDisplay(SurfaceHolder holder) throws java.io.IOException {}
    public final void startPreview() {}
    public final void stopPreview() {}
    public final void setOneShotPreviewCallback(PreviewCallback cb) {}
    public final void setPreviewCallback(PreviewCallback cb) {}
    public final void autoFocus(AutoFocusCallback cb) {}
    public final void cancelAutoFocus() {}
    public final void setDisplayOrientation(int degrees) {}
    public void setParameters(Parameters params) {}
    public Parameters getParameters() { return new Parameters(); }

    public interface AutoFocusCallback { void onAutoFocus(boolean success, Camera camera); }
    public interface PreviewCallback { void onPreviewFrame(byte[] data, Camera camera); }

    public static class CameraInfo {
        public static final int CAMERA_FACING_BACK = 0, CAMERA_FACING_FRONT = 1;
        public int facing;
        public int orientation;
        public boolean canDisableShutterSound;

        public CameraInfo() {}
    }

    /** The settings of a camera, as "key=value" pairs that flatten() joins with ";". */
    public class Parameters {
        public static final String FLASH_MODE_OFF = "off", FLASH_MODE_AUTO = "auto", FLASH_MODE_ON = "on",
            FLASH_MODE_RED_EYE = "red-eye", FLASH_MODE_TORCH = "torch";
        public static final String FOCUS_MODE_AUTO = "auto", FOCUS_MODE_INFINITY = "infinity", FOCUS_MODE_MACRO = "macro",
            FOCUS_MODE_FIXED = "fixed", FOCUS_MODE_EDOF = "edof", FOCUS_MODE_CONTINUOUS_VIDEO = "continuous-video",
            FOCUS_MODE_CONTINUOUS_PICTURE = "continuous-picture";

        private final LinkedHashMap<String, String> map = new LinkedHashMap<>();

        private Parameters() {}

        public String flatten() {
            StringBuilder b = new StringBuilder();
            for (var e : map.entrySet()) {
                if (b.length() > 0) b.append(';');
                b.append(e.getKey()).append('=').append(e.getValue());
            }
            return b.toString();
        }

        public void unflatten(String flattened) {
            map.clear();
            for (String kv : flattened.split(";")) {
                int i = kv.indexOf('=');
                if (i > 0) map.put(kv.substring(0, i), kv.substring(i + 1));
            }
        }

        public void remove(String key) { map.remove(key); }

        public void set(String key, String value) {
            if (key.indexOf('=') != -1 || key.indexOf(';') != -1 || key.indexOf(0) != -1) return;
            if (value.indexOf('=') != -1 || value.indexOf(';') != -1 || value.indexOf(0) != -1) return;
            map.put(key, value);
        }

        public void set(String key, int value) { map.put(key, Integer.toString(value)); }
        public String get(String key) { return map.get(key); }

        public int getInt(String key) { return Integer.parseInt(map.get(key)); }

        public void setPreviewSize(int width, int height) { set("preview-size", width + "x" + height); }
        public String getFlashMode() { return get("flash-mode"); }
        public void setFlashMode(String value) { if (value != null) set("flash-mode", value); else remove("flash-mode"); }
        public String getFocusMode() { return get("focus-mode"); }
        public void setFocusMode(String value) { if (value != null) set("focus-mode", value); else remove("focus-mode"); }
        public java.util.List<String> getSupportedFlashModes() { return null; }
        public java.util.List<String> getSupportedFocusModes() { return new java.util.ArrayList<>(); }
    }
}
