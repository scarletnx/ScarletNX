// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import java.util.ArrayList;

/** Animators for the drawable states of a view. Here the view's state changes at once. */
public class StateListAnimator implements Cloneable {
    private ArrayList<int[]> specs = new ArrayList<>();
    private ArrayList<Animator> animators = new ArrayList<>();

    public StateListAnimator() {}
    public void addState(int[] specs, Animator animator) { this.specs.add(specs); animators.add(animator); }
    public void jumpToCurrentState() { for (Animator a : animators) if (a.isRunning()) a.end(); }
    @Override public StateListAnimator clone() {
        StateListAnimator s = new StateListAnimator();
        for (int i = 0; i < animators.size(); ++i) s.addState(specs.get(i), animators.get(i).clone());
        return s;
    }
}
