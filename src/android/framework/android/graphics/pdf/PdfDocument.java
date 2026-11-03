// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.pdf;

import android.graphics.Canvas;
import android.graphics.Rect;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * A PDF document of pages. The canvas of a page records for the GPU, and nothing here reads the drawing back, so
 * writeTo writes each page at its size, but blank.
 */
public class PdfDocument {
    private final List<PageInfo> pages = new ArrayList<>();
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
        pages.add(page.info);
        current = null;
    }
    public void writeTo(OutputStream out) throws IOException {
        if (closed) throw new IllegalStateException("document is closed!");
        if (current != null) throw new IllegalStateException("Current page not finished!");
        if (out == null) throw new IllegalArgumentException("out cannot be null!");
        StringBuilder pdf = new StringBuilder("%PDF-1.4\n");
        List<Integer> offsets = new ArrayList<>();
        int n = pages.size();
        StringBuilder kids = new StringBuilder();
        for (int i = 0; i < n; ++i) kids.append(3 + i).append(" 0 R ");
        offsets.add(pdf.length());
        pdf.append("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
        offsets.add(pdf.length());
        pdf.append("2 0 obj\n<< /Type /Pages /Kids [").append(kids).append("] /Count ").append(n).append(" >>\nendobj\n");
        for (int i = 0; i < n; ++i) {
            PageInfo p = pages.get(i);
            offsets.add(pdf.length());
            pdf.append(3 + i).append(" 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ").append(p.width).append(' ')
                .append(p.height).append("] >>\nendobj\n");
        }
        int xref = pdf.length();
        pdf.append("xref\n0 ").append(n + 3).append("\n0000000000 65535 f \n");
        for (int off : offsets) pdf.append(String.format("%010d 00000 n \n", off));
        pdf.append("trailer\n<< /Size ").append(n + 3).append(" /Root 1 0 R >>\nstartxref\n").append(xref).append("\n%%EOF\n");
        out.write(pdf.toString().getBytes(StandardCharsets.US_ASCII));
    }
    public List<PageInfo> getPages() { return Collections.unmodifiableList(pages); }
    public void close() {
        closed = true;
        current = null;
    }

    public static class Page {
        private final PageInfo info;
        private final Canvas canvas = new Canvas();
        private boolean finished;

        private Page(PageInfo info) { this.info = info; }
        public Canvas getCanvas() { return canvas; }
        public PageInfo getInfo() { return info; }
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
