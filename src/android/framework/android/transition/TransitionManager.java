// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

import android.view.ViewGroup;

/** Starts transitions of scenes. Here the changes show at once, and the listeners hear the start and the end. */
public class TransitionManager {
    public TransitionManager() {}

    public static void beginDelayedTransition(ViewGroup sceneRoot) { beginDelayedTransition(sceneRoot, null); }
    public static void beginDelayedTransition(ViewGroup sceneRoot, Transition transition) {
        if (transition == null || sceneRoot == null) return;
        Transition t = transition.clone();
        sceneRoot.post(t::runAtOnce);
    }
    public static void endTransitions(ViewGroup sceneRoot) {}
}
