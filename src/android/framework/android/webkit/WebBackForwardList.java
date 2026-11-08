// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import java.util.ArrayList;
import java.util.List;

/** A copy of a WebView's history and where it is in it. */
public class WebBackForwardList implements Cloneable, java.io.Serializable {
    private final ArrayList<WebHistoryItem> items;
    private final int current;

    WebBackForwardList(List<WebHistoryItem> items, int current) {
        this.items = new ArrayList<>(items);
        this.current = current;
    }
    public synchronized WebHistoryItem getCurrentItem() { return current >= 0 && current < items.size() ? items.get(current) : null; }
    public synchronized int getCurrentIndex() { return current; }
    public synchronized WebHistoryItem getItemAtIndex(int index) { return index >= 0 && index < items.size() ? items.get(index) : null; }
    public synchronized int getSize() { return items.size(); }
    @Override protected synchronized WebBackForwardList clone() { return new WebBackForwardList(items, current); }
}
