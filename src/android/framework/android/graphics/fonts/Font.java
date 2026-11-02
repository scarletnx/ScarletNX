// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.fonts;

import android.content.res.AssetManager;
import android.content.res.Resources;
import java.io.File;
import java.io.IOException;

/** A font face of a file. */
public final class Font {
    private final int handle;
    private final FontStyle style;
    private final String path;

    private Font(int handle, FontStyle style, String path) { this.handle = handle; this.style = style; this.path = path; }

    public int handle() { return handle; }
    public FontStyle getStyle() { return style; }
    public File getFile() { return path != null ? new File(path) : null; }
    public int getTtcIndex() { return 0; }
    public FontVariationAxis[] getAxes() { return null; }
    public java.util.Locale[] getLocaleList() { return new java.util.Locale[0]; }

    /** A font's bytes written to a file of the app's cache, named by their hash, for the runtime to open. */
    public static String spill(byte[] data) {
        try {
            String name = Integer.toHexString(java.util.Arrays.hashCode(data)) + "-" + data.length + ".ttf";
            File f = new File(snx.android.Main.dataRoot(), "fonts/" + name);
            if (!f.isFile()) {
                f.getParentFile().mkdirs();
                java.nio.file.Files.write(f.toPath(), data);
            }
            return f.getPath();
        } catch (IOException e) {
            return null;
        }
    }

    public static final class Builder {
        private String path;
        private int index, weight = 400, slant = 0;

        public Builder(File path) { this.path = path.getPath(); }
        public Builder(android.os.ParcelFileDescriptor fd) {
            byte[] data;
            try {
                // the descriptor stays the caller's: its stream is not closed
                data = new java.io.FileInputStream(fd.getFileDescriptor()).readAllBytes();
            } catch (IOException e) {
                data = new byte[0];
            }
            this.path = spill(data);
        }
        public Builder(java.nio.ByteBuffer buffer) {
            byte[] b = new byte[buffer.remaining()];
            buffer.duplicate().get(b);
            path = spill(b);
        }
        public Builder(AssetManager am, String path) { this.path = spill(snx.android.Apk.read("assets/" + path)); }
        public Builder(Resources res, int resId) throws IOException {
            android.util.TypedValue v = new android.util.TypedValue();
            res.getValue(resId, v, true);
            byte[] data = v.string != null ? snx.android.Apk.read(v.string.toString()) : null;
            if (data == null) throw new IOException("Font resource #0x" + Integer.toHexString(resId) + " not found");
            path = spill(data);
        }
        public Builder(Font font) { path = font.path; weight = font.style.getWeight(); slant = font.style.getSlant(); }
        public Builder setWeight(int weight) { this.weight = weight; return this; }
        public Builder setSlant(int slant) { this.slant = slant; return this; }
        public Builder setTtcIndex(int ttcIndex) { index = ttcIndex; return this; }
        public Builder setFontVariationSettings(String settings) { return this; }
        public Builder setFontVariationSettings(FontVariationAxis[] axes) { return this; }

        public Font build() throws IOException {
            int h = path != null ? snx.android.hwui.Text.loadFont(path, index) : 0;
            if (h == 0) throw new IOException("Failed to read font contents: " + path);
            return new Font(h, new FontStyle(weight, slant), path);
        }
    }
}
