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
    /** enters a scene, at once, with the default transition's listeners told */
    public static void go(Scene scene) { go(scene, new AutoTransition()); }
    public static void go(Scene scene, Transition transition) {
        scene.enter();
        if (transition != null) scene.getSceneRoot().post(transition.clone()::runAtOnce);
    }
    /* the transitions between scenes, kept: every change shows at once */
    private final java.util.HashMap<Scene, Transition> transitions = new java.util.HashMap<>();
    public void setTransition(Scene scene, Transition transition) { transitions.put(scene, transition); }
    public void setTransition(Scene fromScene, Scene toScene, Transition transition) { transitions.put(toScene, transition); }
    public void transitionTo(Scene scene) { go(scene, transitions.get(scene)); }
}
