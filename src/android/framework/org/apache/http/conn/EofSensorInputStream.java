// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn;

import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;

/** A response's content stream, which abortConnection closes at once. */
public class EofSensorInputStream extends FilterInputStream {
    public EofSensorInputStream(InputStream in) { super(in); }
    public void releaseConnection() throws IOException { close(); }
    public void abortConnection() throws IOException { close(); }
}
