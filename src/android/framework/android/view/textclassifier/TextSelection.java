// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.textclassifier;

/** A suggested selection of text: the selection that was asked for, as no classifier runs here. */
public final class TextSelection implements android.os.Parcelable {
    private final int start, end;
    private final TextClassification classification;
    private final String id;

    private TextSelection(int start, int end, TextClassification classification, String id) {
        this.start = start;
        this.end = end;
        this.classification = classification;
        this.id = id;
    }
    public int getSelectionStartIndex() { return start; }
    public int getSelectionEndIndex() { return end; }
    public int getEntityCount() { return 0; }
    public String getEntity(int index) { throw new IndexOutOfBoundsException("index " + index + " of 0 entities"); }
    public float getConfidenceScore(String entity) { return 0; }
    public String getId() { return id; }
    public TextClassification getTextClassification() { return classification; }
    public android.os.Bundle getExtras() { return android.os.Bundle.EMPTY; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeInt(start); dest.writeInt(end); dest.writeString(id); }
    @Override public String toString() { return "TextSelection{" + start + ", " + end + "}"; }

    public static final class Builder {
        private final int start, end;
        private TextClassification classification;
        private String id;

        public Builder(int startIndex, int endIndex) {
            if (startIndex < 0 || endIndex <= startIndex) throw new IllegalArgumentException("start must be >= 0 and end > start");
            start = startIndex;
            end = endIndex;
        }
        public Builder setEntityType(String type, float confidenceScore) { return this; }
        public Builder setId(String id) { this.id = id; return this; }
        public Builder setTextClassification(TextClassification textClassification) { classification = textClassification; return this; }
        public TextSelection build() { return new TextSelection(start, end, classification, id); }
    }

    /** A request to suggest a selection in a text. */
    public static final class Request implements android.os.Parcelable {
        private final CharSequence text;
        private final int start, end;
        private final android.os.LocaleList locales;
        private final boolean includeClassification;

        private Request(Builder b) { text = b.text; start = b.start; end = b.end; locales = b.locales; includeClassification = b.includeClassification; }
        public CharSequence getText() { return text; }
        public int getStartIndex() { return start; }
        public int getEndIndex() { return end; }
        public android.os.LocaleList getDefaultLocales() { return locales; }
        public boolean shouldIncludeTextClassification() { return includeClassification; }
        public android.os.Bundle getExtras() { return android.os.Bundle.EMPTY; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeCharSequence(text); dest.writeInt(start); dest.writeInt(end); }

        public static final class Builder {
            private final CharSequence text;
            private final int start, end;
            private android.os.LocaleList locales;
            private boolean includeClassification;

            public Builder(CharSequence text, int startIndex, int endIndex) {
                if (text == null) throw new NullPointerException("text");
                if (startIndex < 0 || endIndex > text.length() || endIndex <= startIndex)
                    throw new IllegalArgumentException("start must be >= 0, end <= text length, and end > start");
                this.text = text;
                start = startIndex;
                end = endIndex;
            }
            public Builder setDefaultLocales(android.os.LocaleList defaultLocales) { locales = defaultLocales; return this; }
            public Builder setIncludeTextClassification(boolean includeTextClassification) { includeClassification = includeTextClassification; return this; }
            public Builder setExtras(android.os.Bundle extras) { return this; }
            public Request build() { return new Request(this); }
        }
    }
}
