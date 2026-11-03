// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** A list of groups with children. A packed position holds a type bit, a 31-bit group and a 32-bit child. */
public class ExpandableListView extends ListView {
    public static final int PACKED_POSITION_TYPE_GROUP = 0, PACKED_POSITION_TYPE_CHILD = 1, PACKED_POSITION_TYPE_NULL = 2;
    public static final long PACKED_POSITION_VALUE_NULL = 0x00000000FFFFFFFFL;
    public static final int CHILD_INDICATOR_INHERIT = -1;
    private static final long TYPE_MASK = 0x8000000000000000L, GROUP_MASK = 0x7FFFFFFF00000000L, CHILD_MASK = 0x00000000FFFFFFFFL;

    public ExpandableListView(Context context) { super(context); }
    public ExpandableListView(Context context, AttributeSet attrs) { super(context, attrs); }
    public ExpandableListView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public ExpandableListView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public static int getPackedPositionType(long packedPosition) {
        if (packedPosition == PACKED_POSITION_VALUE_NULL) return PACKED_POSITION_TYPE_NULL;
        return (packedPosition & TYPE_MASK) != 0 ? PACKED_POSITION_TYPE_CHILD : PACKED_POSITION_TYPE_GROUP;
    }
    public static int getPackedPositionGroup(long packedPosition) {
        if (packedPosition == PACKED_POSITION_VALUE_NULL) return -1;
        return (int) ((packedPosition & GROUP_MASK) >>> 32);
    }
    public static int getPackedPositionChild(long packedPosition) {
        if (packedPosition == PACKED_POSITION_VALUE_NULL || (packedPosition & TYPE_MASK) == 0) return -1;
        return (int) (packedPosition & CHILD_MASK);
    }
    public static long getPackedPositionForChild(int groupPosition, int childPosition) {
        return TYPE_MASK | ((long) groupPosition & 0x7FFFFFFFL) << 32 | (long) childPosition & CHILD_MASK;
    }
    public static long getPackedPositionForGroup(int groupPosition) { return ((long) groupPosition & 0x7FFFFFFFL) << 32; }
    @Override public CharSequence getAccessibilityClassName() { return ExpandableListView.class.getName(); }
}
