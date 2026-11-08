// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2013 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.pdf;

import android.graphics.Canvas;
import android.graphics.Rect;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * A PDF document of pages. A page's canvas records a display list, as any canvas does, and finishPage makes it the
 * page's content (snx.android.hwui.Pdf, the HLE of Skia's PDF backend), which writeTo writes with the fonts and images
 * the pages use.
 */
public class PdfDocument {
    private final List<PageInfo> pages = new ArrayList<>();
    private final snx.android.hwui.Pdf pdf = new snx.android.hwui.Pdf();
    private Page current;
    private boolean closed;

    public PdfDocument() {}

    public Page startPage(PageInfo pageInfo) {
        if (closed) throw new IllegalStateException("document is closed!");
        if (current != null) throw new IllegalStateException("Previous page not finished!");
        if (pageInfo == null) throw new IllegalArgumentException("page cannot be null");
        return current = new Page(pageInfo);
    }
    public void finishPage(Page page) {
        if (closed) throw new IllegalStateException("document is closed!");
        if (page == null) throw new IllegalArgumentException("page cannot be null");
        if (page != current) throw new IllegalStateException("invalid page");
        if (page.finished) throw new IllegalStateException("page already finished");
        page.finished = true;
        pdf.addPage(page.list, page.info.width, page.info.height);
        pages.add(page.info);
        current = null;
    }
    public void writeTo(OutputStream out) throws IOException {
        if (closed) throw new IllegalStateException("document is closed!");
        if (current != null) throw new IllegalStateException("Current page not finished!");
        if (out == null) throw new IllegalArgumentException("out cannot be null!");
        pdf.writeTo(out);
    }
    public List<PageInfo> getPages() { return Collections.unmodifiableList(pages); }
    public void close() {
        closed = true;
        current = null;
    }

    public static class Page {
        private final PageInfo info;
        private final snx.android.hwui.DisplayList list = new snx.android.hwui.DisplayList();
        private final Canvas canvas;
        private boolean finished;

        private Page(PageInfo info) {
            this.info = info;
            canvas = new PageCanvas(list, info.width, info.height);
        }
        public Canvas getCanvas() { return canvas; }
        public PageInfo getInfo() { return info; }
    }

    /** A canvas that records the page's drawing into its list, at the page's size */
    private static final class PageCanvas extends Canvas {
        PageCanvas(snx.android.hwui.DisplayList list, int width, int height) { super(list, width, height); }
    }

    public static final class PageInfo {
        private final int width, height, number;
        private Rect content;

        private PageInfo(int width, int height, int number) {
            this.width = width;
            this.height = height;
            this.number = number;
            content = new Rect(0, 0, width, height);
        }
        public int getPageWidth() { return width; }
        public int getPageHeight() { return height; }
        public Rect getContentRect() { return content; }
        public int getPageNumber() { return number; }

        public static final class Builder {
            private final PageInfo info;

            public Builder(int pageWidth, int pageHeight, int pageNumber) {
                if (pageWidth <= 0) throw new IllegalArgumentException("page width must be positive");
                if (pageHeight <= 0) throw new IllegalArgumentException("page width must be positive");
                if (pageNumber < 0) throw new IllegalArgumentException("pageNumber must be non negative");
                info = new PageInfo(pageWidth, pageHeight, pageNumber);
            }
            public Builder setContentRect(Rect contentRect) {
                if (contentRect != null && (contentRect.left < 0 || contentRect.top < 0 || contentRect.right > info.width || contentRect.bottom > info.height))
                    throw new IllegalArgumentException("contentRect does not fit the page");
                info.content = contentRect;
                return this;
            }
            public PageInfo create() {
                if (info.content == null) info.content = new Rect(0, 0, info.width, info.height);
                return info;
            }
        }
    }
}
