// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.textclassifier;

import java.util.Collection;
import java.util.Collections;

/** The links that a classifier finds in a text. No classifier runs here, so a text has no links. */
public final class TextLinks implements android.os.Parcelable {
    public static final int STATUS_LINKS_APPLIED = 0, STATUS_NO_LINKS_FOUND = 1, STATUS_NO_LINKS_APPLIED = 2,
        STATUS_DIFFERENT_TEXT = 3, STATUS_UNSUPPORTED_CHARACTER = 4;
    public static final int APPLY_STRATEGY_IGNORE = 0, APPLY_STRATEGY_REPLACE = 1;
    private final String text;

    private TextLinks(String text) { this.text = text; }
    public CharSequence getText() { return text; }
    public Collection<TextLink> getLinks() { return Collections.emptyList(); }
    public android.os.Bundle getExtras() { return android.os.Bundle.EMPTY; }
    public int apply(android.text.Spannable text, int applyStrategy, java.util.function.Function<TextLink, Object> spanFactory) {
        return this.text.equals(text.toString()) ? STATUS_NO_LINKS_FOUND : STATUS_DIFFERENT_TEXT;
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeString(text); }

    /** A link: a range of the text and its entity types. */
    public static final class TextLink implements android.os.Parcelable {
        private final int start, end;

        TextLink(int start, int end) { this.start = start; this.end = end; }
        public int getStart() { return start; }
        public int getEnd() { return end; }
        public int getEntityCount() { return 0; }
        public String getEntity(int index) { throw new IndexOutOfBoundsException("index " + index + " of 0 entities"); }
        public float getConfidenceScore(String entityType) { return 0; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeInt(start); dest.writeInt(end); }
    }

    public static final class Builder {
        private final String text;

        public Builder(String fullText) { text = fullText; }
        public Builder clearTextLinks() { return this; }
        public TextLinks build() { return new TextLinks(text); }
    }
}
