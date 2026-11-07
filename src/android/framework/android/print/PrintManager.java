// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.print;

/** The print system. There is no printer or print service, so print fails and answers null, as Android does on failure. */
public final class PrintManager {
    public PrintManager() {}
    public PrintJob print(String printJobName, PrintDocumentAdapter documentAdapter, PrintAttributes attributes) { return null; }
}
