// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

/** A state of a scene root's views: a layout that entering puts in the root, or what an enter action makes. */
public final class Scene {
    private final ViewGroup sceneRoot;
    private int layoutId;
    private Context context;
    private View layout;
    private Runnable enterAction, exitAction;

    public Scene(ViewGroup sceneRoot) { this.sceneRoot = sceneRoot; }
    public Scene(ViewGroup sceneRoot, View layout) { this.sceneRoot = sceneRoot; this.layout = layout; }
    @Deprecated public Scene(ViewGroup sceneRoot, ViewGroup layout) { this(sceneRoot, (View) layout); }
    private Scene(ViewGroup sceneRoot, int layoutId, Context context) {
        this.sceneRoot = sceneRoot;
        this.layoutId = layoutId;
        this.context = context;
    }

    /* each root's scenes by layout, as Android keeps them in a tag of the root */
    private static final java.util.WeakHashMap<ViewGroup, android.util.SparseArray<Scene>> scenes = new java.util.WeakHashMap<>();

    /** the scene of a layout in a root, one for each layout */
    public static Scene getSceneForLayout(ViewGroup sceneRoot, int layoutId, Context context) {
        android.util.SparseArray<Scene> list = scenes.computeIfAbsent(sceneRoot, r -> new android.util.SparseArray<>());
        Scene scene = list.get(layoutId);
        if (scene == null) {
            scene = new Scene(sceneRoot, layoutId, context);
            list.put(layoutId, scene);
        }
        return scene;
    }
    public ViewGroup getSceneRoot() { return sceneRoot; }
    public void enter() {
        if (layoutId > 0 || layout != null) {
            sceneRoot.removeAllViews();
            if (layoutId > 0) LayoutInflater.from(context).inflate(layoutId, sceneRoot);
            else sceneRoot.addView(layout);
        }
        if (enterAction != null) enterAction.run();
    }
    public void exit() { if (exitAction != null) exitAction.run(); }
    public void setEnterAction(Runnable action) { enterAction = action; }
    public void setExitAction(Runnable action) { exitAction = action; }
}
