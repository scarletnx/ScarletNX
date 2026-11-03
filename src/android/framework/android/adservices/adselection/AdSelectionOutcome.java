// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.adselection;

import android.net.Uri;

/** The result of an ad selection. No selection runs, so an app never gets one. */
public class AdSelectionOutcome {
    private final long id;
    private final Uri renderUri;

    private AdSelectionOutcome(long id, Uri renderUri) { this.id = id; this.renderUri = renderUri; }
    public long getAdSelectionId() { return id; }
    public Uri getRenderUri() { return renderUri; }
}
