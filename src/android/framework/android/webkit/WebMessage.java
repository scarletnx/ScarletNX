// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

/** A message to a page: its data and the ports it carries */
public class WebMessage {
    private final String data;
    private final WebMessagePort[] ports;

    public WebMessage(String data) { this(data, null); }
    public WebMessage(String data, WebMessagePort[] ports) {
        this.data = data;
        this.ports = ports;
    }
    public String getData() { return data; }
    public WebMessagePort[] getPorts() { return ports; }
}
