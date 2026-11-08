// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2019 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os.ext;

import java.util.Collections;
import java.util.Map;

/**
 * The versions of the SDK extensions, which the Play system updates install. None is installed here, so each
 * extension is at version 0 and an app takes the paths of the base API level.
 */
public class SdkExtensions {
    public static final int AD_SERVICES = 1000000;

    private SdkExtensions() {}

    public static int getExtensionVersion(int extension) {
        if (extension < 30) throw new IllegalArgumentException("not a valid extension: " + extension);
        return 0;
    }

    public static Map<Integer, Integer> getAllExtensionVersions() { return Collections.emptyMap(); }
}
