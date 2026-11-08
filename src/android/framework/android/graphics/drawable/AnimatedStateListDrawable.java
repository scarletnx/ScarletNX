// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

/**
 * A state list whose changes may animate. The transitions are kept but not played: the state's drawable shows at
 * once, as a transition that has ended would leave it.
 */
public class AnimatedStateListDrawable extends StateListDrawable {
    public AnimatedStateListDrawable() {}

    public void addState(int[] stateSet, Drawable drawable, int id) {
        if (drawable == null) throw new IllegalArgumentException("Drawable must not be null");
        addState(stateSet, drawable);
    }
    public <T extends Drawable & Animatable> void addTransition(int fromId, int toId, T transition, boolean reversible) {
        if (transition == null) throw new IllegalArgumentException("Transition drawable must not be null");
    }
    @Override void addItem(int[] states, Drawable d, int id) { addState(states, d, id); }
}
