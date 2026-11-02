// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony.cdma;

/** A CDMA cell's location */
public class CdmaCellLocation {
    public CdmaCellLocation() {}
    /** a quarter second of arc as degrees */
    public static double convertQuartSecToDecDegrees(int quartSec) {
        if (Double.isNaN(quartSec) || quartSec < -2592000 || quartSec > 2592000)
            throw new IllegalArgumentException("Invalid coordiante value:" + quartSec);
        return ((double) quartSec) / (3600 * 4);
    }
}
