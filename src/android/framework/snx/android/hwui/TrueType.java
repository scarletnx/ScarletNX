// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.hwui;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.BitSet;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * A TrueType font (glyf outlines) as a PDF carries one: its metrics, a subset of the glyphs a document draws, and the
 * text each glyph stands for. A face of a collection (.ttc) is read from its own table directory.
 */
final class TrueType {
    private final ByteBuffer font;
    private final Map<String, int[]> tables = new TreeMap<>(); // a tag: its offset and length
    final int unitsPerEm, xMin, yMin, xMax, yMax, ascent, descent;
    private final int glyphCount, metricCount;
    private final boolean longOffsets;

    TrueType(byte[] file, int index) {
        font = ByteBuffer.wrap(file);
        int directory = 0;
        if (font.getInt(0) == 0x74746366) { // "ttcf": a collection, the face's directory at its offset
            if (index < 0 || index >= font.getInt(8))
                throw new IllegalArgumentException("snx: the font collection has " + font.getInt(8) + " faces, not "
                                                   + index);
            directory = font.getInt(12 + 4 * index);
        }
        int version = font.getInt(directory);
        if (version != 0x00010000 && version != 0x74727565)
            throw new IllegalArgumentException(String.format(Locale.ROOT,
                "snx: a PDF carries TrueType outlines; this font's version is 0x%08x (0x4f54544f is CFF)", version));
        int count = font.getShort(directory + 4) & 0xffff;
        for (int i = 0; i < count; ++i) {
            int at = directory + 12 + 16 * i;
            byte[] tag = new byte[4];
            font.get(at, tag);
            tables.put(new String(tag, java.nio.charset.StandardCharsets.ISO_8859_1),
                       new int[] {font.getInt(at + 8), font.getInt(at + 12)});
        }
        for (String need : new String[] {"head", "hhea", "hmtx", "maxp", "loca", "glyf"})
            if (!tables.containsKey(need))
                throw new IllegalArgumentException("snx: the font has no " + need + " table");
        int head = tables.get("head")[0];
        unitsPerEm = font.getShort(head + 18) & 0xffff;
        xMin = font.getShort(head + 36);
        yMin = font.getShort(head + 38);
        xMax = font.getShort(head + 40);
        yMax = font.getShort(head + 42);
        longOffsets = font.getShort(head + 50) != 0;
        int hhea = tables.get("hhea")[0];
        ascent = font.getShort(hhea + 4);
        descent = font.getShort(hhea + 6);
        metricCount = font.getShort(hhea + 34) & 0xffff;
        glyphCount = font.getShort(tables.get("maxp")[0] + 4) & 0xffff;
    }

    /** A glyph's advance, in font units */
    int advance(int glyph) {
        int hmtx = tables.get("hmtx")[0];
        return font.getShort(hmtx + 4 * Math.min(glyph, metricCount - 1)) & 0xffff;
    }

    /** The PostScript name (the name table's ID 6), letters and digits only, or "Font" */
    String name() {
        int[] t = tables.get("name");
        if (t != null) {
            int count = font.getShort(t[0] + 2) & 0xffff, strings = t[0] + (font.getShort(t[0] + 4) & 0xffff);
            for (int i = 0; i < count; ++i) {
                int r = t[0] + 6 + 12 * i;
                int platform = font.getShort(r) & 0xffff, id = font.getShort(r + 6) & 0xffff;
                int length = font.getShort(r + 8) & 0xffff, offset = font.getShort(r + 10) & 0xffff;
                if (id != 6) continue;
                byte[] raw = new byte[length];
                font.get(strings + offset, raw);
                String s = new String(raw, platform == 1 ? java.nio.charset.StandardCharsets.ISO_8859_1
                                                         : java.nio.charset.StandardCharsets.UTF_16BE);
                String clean = s.replaceAll("[^A-Za-z0-9-]", "");
                if (!clean.isEmpty()) return clean;
            }
        }
        return "Font";
    }

    private int glyphStart(int glyph) {
        int loca = tables.get("loca")[0];
        return longOffsets ? font.getInt(loca + 4 * glyph) : 2 * (font.getShort(loca + 2 * glyph) & 0xffff);
    }

    /**
     * The font with only these glyphs' outlines (and the components of composite ones; glyph 0, the missing glyph,
     * always): every other glyph is empty, so the glyph IDs stay as they are. The tables a PDF reader needs are kept:
     * head, hhea, hmtx, maxp, loca (long), glyf, and the hinting ones
     */
    byte[] subset(BitSet used) {
        BitSet keep = (BitSet) used.clone();
        keep.set(0);
        int glyf = tables.get("glyf")[0];
        // the components of composite glyphs, which may be composite themselves
        for (boolean grew = true; grew;) {
            grew = false;
            for (int g = keep.nextSetBit(0); g >= 0 && g < glyphCount; g = keep.nextSetBit(g + 1)) {
                int start = glyphStart(g), end = glyphStart(g + 1);
                if (end - start < 10 || font.getShort(glyf + start) >= 0) continue;
                int at = glyf + start + 10;
                for (int flags = 0x20; (flags & 0x20) != 0;) { // MORE_COMPONENTS
                    flags = font.getShort(at) & 0xffff;
                    int component = font.getShort(at + 2) & 0xffff;
                    if (!keep.get(component)) {
                        keep.set(component);
                        grew = true;
                    }
                    at += 4 + ((flags & 1) != 0 ? 4 : 2); // ARG_1_AND_2_ARE_WORDS
                    if ((flags & 8) != 0) at += 2; // WE_HAVE_A_SCALE
                    else if ((flags & 0x40) != 0) at += 4; // WE_HAVE_AN_X_AND_Y_SCALE
                    else if ((flags & 0x80) != 0) at += 8; // WE_HAVE_A_TWO_BY_TWO
                }
            }
        }
        ByteArrayOutputStream glyphs = new ByteArrayOutputStream();
        ByteBuffer loca = ByteBuffer.allocate(4 * (glyphCount + 1));
        for (int g = 0; g < glyphCount; ++g) {
            loca.putInt(glyphs.size());
            if (!keep.get(g)) continue;
            int start = glyphStart(g), end = glyphStart(g + 1);
            byte[] data = new byte[Math.max(0, end - start)];
            font.get(glyf + start, data);
            glyphs.write(data, 0, data.length);
            while ((glyphs.size() & 3) != 0) glyphs.write(0);
        }
        loca.putInt(glyphs.size());
        Map<String, byte[]> out = new TreeMap<>();
        for (String tag : new String[] {"head", "hhea", "hmtx", "maxp", "cvt ", "fpgm", "prep"}) {
            int[] t = tables.get(tag);
            if (t == null) continue;
            byte[] data = new byte[t[1]];
            font.get(t[0], data);
            out.put(tag, data);
        }
        ByteBuffer.wrap(out.get("head")).putShort(50, (short) 1).putInt(8, 0); // long offsets; checksum adjustment
        out.put("loca", loca.array());
        out.put("glyf", glyphs.toByteArray());
        return sfnt(out);
    }

    /** Tables as one font file: the directory, then each table at a multiple of 4, with its checksum */
    private static byte[] sfnt(Map<String, byte[]> tables) {
        int count = tables.size(), log = 31 - Integer.numberOfLeadingZeros(count);
        int size = 12 + 16 * count;
        for (byte[] t : tables.values()) size += (t.length + 3) & ~3;
        ByteBuffer f = ByteBuffer.allocate(size);
        f.putInt(0x00010000).putShort((short) count).putShort((short) (16 << log)).putShort((short) log)
            .putShort((short) (16 * count - (16 << log)));
        int offset = 12 + 16 * count, at = 12, head = 0;
        for (Map.Entry<String, byte[]> e : tables.entrySet()) {
            byte[] t = e.getValue();
            if (e.getKey().equals("head")) head = offset;
            f.put(at, e.getKey().getBytes(java.nio.charset.StandardCharsets.ISO_8859_1));
            f.putInt(at + 4, checksum(t)).putInt(at + 8, offset).putInt(at + 12, t.length);
            f.put(offset, t);
            offset += (t.length + 3) & ~3;
            at += 16;
        }
        // head's checksum adjustment: 0xB1B0AFBA less the sum of the whole file
        byte[] file = f.array();
        f.putInt(head + 8, 0xB1B0AFBA - checksum(file));
        return file;
    }

    private static int checksum(byte[] data) {
        int sum = 0;
        for (int i = 0; i < data.length; i += 4) {
            int word = 0;
            for (int k = 0; k < 4; ++k) word = word << 8 | (i + k < data.length ? data[i + k] & 0xff : 0);
            sum += word;
        }
        return sum;
    }

    /** Each glyph's code point by the cmap (format 4 or 12 of Unicode): the first that maps to it */
    private Map<Integer, Integer> codePoints() {
        Map<Integer, Integer> of = new TreeMap<>();
        int[] t = tables.get("cmap");
        if (t == null) return of;
        int count = font.getShort(t[0] + 2) & 0xffff, best = -1, bestFormat = 0;
        for (int i = 0; i < count; ++i) {
            int r = t[0] + 4 + 8 * i;
            int platform = font.getShort(r) & 0xffff, encoding = font.getShort(r + 2) & 0xffff;
            int sub = t[0] + font.getInt(r + 4), format = font.getShort(sub) & 0xffff;
            boolean unicode = platform == 0 || platform == 3 && (encoding == 1 || encoding == 10);
            if (unicode && (format == 12 || format == 4 && bestFormat != 12)) {
                best = sub;
                bestFormat = format;
            }
        }
        if (best < 0) return of;
        if (bestFormat == 12) {
            int groups = font.getInt(best + 12);
            for (int i = 0; i < groups; ++i) {
                int g = best + 16 + 12 * i, first = font.getInt(g), last = font.getInt(g + 4);
                int glyph = font.getInt(g + 8);
                for (int c = first; c <= last && c - first < 0x110000; ++c) of.putIfAbsent(glyph + c - first, c);
            }
        } else {
            int segments = (font.getShort(best + 6) & 0xffff) / 2;
            int ends = best + 14, starts = ends + 2 * segments + 2, deltas = starts + 2 * segments,
                offsets = deltas + 2 * segments;
            for (int i = 0; i < segments; ++i) {
                int end = font.getShort(ends + 2 * i) & 0xffff, start = font.getShort(starts + 2 * i) & 0xffff;
                int delta = font.getShort(deltas + 2 * i), offset = font.getShort(offsets + 2 * i) & 0xffff;
                for (int c = start; c <= end && c != 0xffff; ++c) {
                    int glyph;
                    if (offset == 0) {
                        glyph = (c + delta) & 0xffff;
                    } else {
                        glyph = font.getShort(offsets + 2 * i + offset + 2 * (c - start)) & 0xffff;
                        if (glyph != 0) glyph = (glyph + delta) & 0xffff;
                    }
                    if (glyph != 0) of.putIfAbsent(glyph, c);
                }
            }
        }
        return of;
    }

    /** A ToUnicode CMap: each used glyph's text, so that a reader can copy and search it */
    String toUnicode(BitSet used) {
        Map<Integer, Integer> of = codePoints();
        StringBuilder s = new StringBuilder("/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
            + "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            + "/CMapName /Adobe-Identity-UCS def\n"
            + "/CMapType 2 def\n1 begincodespacerange\n<0000> <ffff>\nendcodespacerange\n");
        StringBuilder chars = new StringBuilder();
        int n = 0;
        for (int g = used.nextSetBit(0); g >= 0; g = used.nextSetBit(g + 1)) {
            Integer c = of.get(g);
            if (c == null) continue;
            StringBuilder hex = new StringBuilder();
            for (char u : Character.toChars(c)) hex.append(String.format(Locale.ROOT, "%04x", (int) u));
            chars.append(String.format(Locale.ROOT, "<%04x> <%s>\n", g, hex));
            if (++n == 100) { // a block holds 100 at most
                s.append("100 beginbfchar\n").append(chars).append("endbfchar\n");
                chars.setLength(0);
                n = 0;
            }
        }
        if (n > 0) s.append(n).append(" beginbfchar\n").append(chars).append("endbfchar\n");
        return s.append("endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n").toString();
    }
}
