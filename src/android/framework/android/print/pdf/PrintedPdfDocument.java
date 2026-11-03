// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.print.pdf;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.pdf.PdfDocument;
import android.print.PrintAttributes;

/** A PDF document with the page size and margins of print attributes, in points (1/72 inch). */
public class PrintedPdfDocument extends PdfDocument {
    private final int width, height;
    private final Rect content;

    public PrintedPdfDocument(Context context, PrintAttributes attributes) {
        PrintAttributes.MediaSize size = attributes.getMediaSize();
        PrintAttributes.Margins margins = attributes.getMinMargins();
        width = points(size.getWidthMils());
        height = points(size.getHeightMils());
        content = margins == null ? new Rect(0, 0, width, height) : new Rect(points(margins.getLeftMils()), points(margins.getTopMils()),
            width - points(margins.getRightMils()), height - points(margins.getBottomMils()));
    }
    private static int points(int mils) { return (int) (mils * 72L / 1000); }

    public Page startPage(int pageNumber) {
        return startPage(new PageInfo.Builder(width, height, pageNumber).setContentRect(content).create());
    }
    public int getPageWidth() { return width; }
    public int getPageHeight() { return height; }
    public Rect getPageContentRect() { return content; }
}
