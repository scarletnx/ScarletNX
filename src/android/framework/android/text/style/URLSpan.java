// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.view.View;

/** A link to a URL. */
public class URLSpan extends ClickableSpan {
    private final String url;

    public URLSpan(String url) { this.url = url; }
    public String getURL() { return url; }
    @Override public void onClick(View widget) {
        android.content.Intent intent = new android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url));
        widget.getContext().startActivity(intent);
    }
}
