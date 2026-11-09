// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.transition;

/** The default transition: views fade out, move, then fade in, in order. */
public class AutoTransition extends TransitionSet {
    public AutoTransition() { init(); }
    public AutoTransition(android.content.Context context, android.util.AttributeSet attrs) { super(context, attrs); init(); }
    private void init() {
        setOrdering(ORDERING_SEQUENTIAL);
        addTransition(new Fade(Fade.OUT)).addTransition(new ChangeBounds()).addTransition(new Fade(Fade.IN));
    }
}
