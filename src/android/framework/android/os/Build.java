// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** The device: Android 16 (API 36) on ScarletNX. */
public class Build {
    public static final String UNKNOWN = "unknown";
    public static final String ID = "BP2A.250605.031";
    public static final String DISPLAY = "ScarletNX";
    public static final String PRODUCT = "scarletnx";
    public static final String DEVICE = "scarletnx";
    public static final String BOARD = "scarletnx";
    public static final String CPU_ABI = "arm64-v8a";
    public static final String CPU_ABI2 = "";
    public static final String MANUFACTURER = "ScarletNX";
    public static final String BRAND = "scarletnx";
    public static final String MODEL = "ScarletNX";
    public static final String SOC_MANUFACTURER = UNKNOWN;
    public static final String SOC_MODEL = UNKNOWN;
    public static final String BOOTLOADER = UNKNOWN;
    public static final String RADIO = UNKNOWN;
    public static final String HARDWARE = "scarletnx";
    public static final String SKU = UNKNOWN;
    public static final String ODM_SKU = UNKNOWN;
    public static final boolean IS_EMULATOR = false;
    public static final String SERIAL = UNKNOWN;
    public static final String[] SUPPORTED_ABIS = {"arm64-v8a"};
    public static final String[] SUPPORTED_32_BIT_ABIS = {};
    public static final String[] SUPPORTED_64_BIT_ABIS = {"arm64-v8a"};
    public static final String TYPE = "user";
    public static final String TAGS = "release-keys";
    public static final String FINGERPRINT = "scarletnx/scarletnx/scarletnx:16/BP2A.250605.031/1:user/release-keys";
    public static final long TIME = 0;
    public static final String USER = "scarletnx";
    public static final String HOST = "scarletnx";
    public static final boolean IS_DEBUGGABLE = false;

    public static String getSerial() { return UNKNOWN; }
    public static String getRadioVersion() { return null; }

    public static class VERSION {
        public static final String INCREMENTAL = "1";
        public static final String RELEASE = "16";
        public static final String RELEASE_OR_CODENAME = "16";
        public static final String RELEASE_OR_PREVIEW_DISPLAY = "16";
        public static final String BASE_OS = "";
        public static final String SECURITY_PATCH = "2025-06-05";
        public static final int MEDIA_PERFORMANCE_CLASS = 0;
        @Deprecated public static final String SDK = "36";
        public static final int SDK_INT = 36;
        public static final int SDK_INT_FULL = 3600000;
        public static final int PREVIEW_SDK_INT = 0;
        public static final String CODENAME = "REL";
    }

    public static class VERSION_CODES {
        public static final int CUR_DEVELOPMENT = 10000;
        public static final int BASE = 1, BASE_1_1 = 2, CUPCAKE = 3, DONUT = 4, ECLAIR = 5, ECLAIR_0_1 = 6,
            ECLAIR_MR1 = 7, FROYO = 8, GINGERBREAD = 9, GINGERBREAD_MR1 = 10, HONEYCOMB = 11, HONEYCOMB_MR1 = 12,
            HONEYCOMB_MR2 = 13, ICE_CREAM_SANDWICH = 14, ICE_CREAM_SANDWICH_MR1 = 15, JELLY_BEAN = 16,
            JELLY_BEAN_MR1 = 17, JELLY_BEAN_MR2 = 18, KITKAT = 19, KITKAT_WATCH = 20, LOLLIPOP = 21,
            LOLLIPOP_MR1 = 22, M = 23, N = 24, N_MR1 = 25, O = 26, O_MR1 = 27, P = 28, Q = 29, R = 30, S = 31,
            S_V2 = 32, TIRAMISU = 33, UPSIDE_DOWN_CAKE = 34, VANILLA_ICE_CREAM = 35, BAKLAVA = 36;
    }

    public static class VERSION_CODES_FULL {
        public static final int BAKLAVA = 3600000;
    }

    public static class Partition {
        public static final String PARTITION_NAME_SYSTEM = "system";
    }
}
