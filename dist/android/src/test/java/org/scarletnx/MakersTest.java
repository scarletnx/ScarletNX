// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.lang.reflect.Field;
import java.util.Locale;
import org.junit.Test;

/** Which maker's limit a phone gets (Makers.limits), from what real phones put in Build.MANUFACTURER and Build.BRAND */
public class MakersTest {
    private static String maker(String manufacturer, String brand) {
        String[] row = Makers.limits(manufacturer, brand);
        return row != null ? row[0] : null;
    }

    @Test
    public void aPhoneGetsItsMakersRowByManufacturerOrBrand() {
        assertEquals("samsung", maker("samsung", "samsung"));
        assertEquals("xiaomi", maker("Xiaomi", "POCO"));
        assertEquals("xiaomi", maker("Xiaomi", "Redmi"));
        assertEquals("oppo", maker("OPPO", "OPPO"));
        assertEquals("realme", maker("realme", "realme"));
        assertEquals("oneplus", maker("OnePlus", "OnePlus"));
        assertEquals("vivo", maker("vivo", "iQOO"));
        assertEquals("huawei", maker("HUAWEI", "HUAWEI"));
        assertEquals("honor", maker("HONOR", "HONOR"));
        assertEquals("asus", maker("asus", "asus"));
        assertEquals("tecno", maker("TECNO MOBILE LIMITED", "TECNO"));
        assertEquals("infinix", maker("INFINIX MOBILITY LIMITED", "Infinix"));
        assertEquals("itel", maker("ITEL MOBILE LIMITED", "Itel"));
    }

    @Test
    public void anOlderHonorMadeByHuaweiTakesHuaweisScreens() {
        assertEquals("huawei", maker("HUAWEI", "HONOR"));
    }

    @Test
    public void aMakerThatAddsNoLimitGetsNone() {
        assertNull(maker("Google", "google"));
        assertNull(maker("motorola", "motorola"));
        assertNull(maker("Sony", "Sony"));
        assertNull(maker("Nothing", "Nothing"));
        assertNull(maker("Fairphone", "Fairphone"));
        assertNull(maker("unknown", "Android")); // the emulator's
    }

    @Test
    public void oppoRealmeAndOneplusShareOneSetting() {
        String oppo = Makers.limits("OPPO", "OPPO")[1];
        assertEquals(oppo, Makers.limits("realme", "realme")[1]);
        assertEquals(oppo, Makers.limits("OnePlus", "OnePlus")[1]);
    }

    /** Makers.open tries a row's screens as package and activity pairs: an odd one out would never be tried */
    @Test
    public void everyRowNamesItsSettingThenScreensInPairs() throws ReflectiveOperationException {
        Field field = Makers.class.getDeclaredField("LIMITS");
        field.setAccessible(true);
        String[][] rows = (String[][])field.get(null);
        assertTrue(rows.length > 0);
        for (String[] row : rows) {
            assertTrue(row[0], row.length >= 2 && row.length % 2 == 0);
            // limits() looks for it in the lower-cased Build names
            assertEquals(row[0].toLowerCase(Locale.ROOT), row[0]);
            assertNotNull(row[0], row[1]);
            for (int i = 2; i < row.length; i += 2) {
                String pack = row[i], activity = row[i + 1];
                assertEquals(pack, pack.toLowerCase(Locale.ROOT));
                assertTrue(activity + " is a class",
                           Character.isUpperCase(activity.charAt(activity.lastIndexOf('.') + 1)));
            }
        }
    }
}
