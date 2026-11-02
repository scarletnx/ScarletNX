// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** View state sets: a spec's positive states must be set, its negative ones (as -state) must not. */
public class StateSet {
    public static final int[] WILD_CARD = new int[0];
    public static final int[] NOTHING = new int[] {0};

    public static boolean isWildCard(int[] stateSetOrSpec) {
        return stateSetOrSpec.length == 0 || stateSetOrSpec[0] == 0;
    }

    public static boolean stateSetMatches(int[] spec, int[] stateSet) {
        if (stateSet == null) return spec == null || isWildCard(spec);
        for (int s : spec) {
            if (s == 0) return true;
            boolean mustMatch = s > 0, found = false;
            int want = mustMatch ? s : -s;
            for (int t : stateSet) {
                if (t == 0) break;
                if (t == want) { found = true; break; }
            }
            if (found != mustMatch) return false;
        }
        return true;
    }

    public static boolean stateSetMatches(int[] spec, int state) {
        for (int s : spec) {
            if (s == 0) return true;
            if (s > 0 ? state != s : state == -s) return false;
        }
        return true;
    }

    public static int[] trimStateSet(int[] states, int newSize) {
        if (states.length == newSize) return states;
        return java.util.Arrays.copyOf(states, newSize);
    }

    public static String dump(int[] states) {
        return java.util.Arrays.toString(states);
    }
}
