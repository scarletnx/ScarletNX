// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** How a dataset shows: kept, as none shows */
public final class Presentations {
    private Presentations() {}
    public static final class Builder {
        public Builder() {}
        public Builder setMenuPresentation(android.widget.RemoteViews menuPresentation) { return this; }
        public Builder setInlinePresentation(InlinePresentation inlinePresentation) { return this; }
        public Builder setDialogPresentation(android.widget.RemoteViews dialogPresentation) { return this; }
        public Presentations build() { return new Presentations(); }
    }
}
