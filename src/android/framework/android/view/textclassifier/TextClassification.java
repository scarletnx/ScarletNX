// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.textclassifier;

import java.util.Collections;
import java.util.List;

/** The kind of a piece of text. No classifier runs here, so a classification has no entities or actions. */
public final class TextClassification implements android.os.Parcelable {
    private final String text;
    private final String id;

    private TextClassification(String text, String id) { this.text = text; this.id = id; }
    public String getText() { return text; }
    public int getEntityCount() { return 0; }
    public String getEntity(int index) { throw new IndexOutOfBoundsException("index " + index + " of 0 entities"); }
    public float getConfidenceScore(String entity) { return 0; }
    public List<android.app.RemoteAction> getActions() { return Collections.emptyList(); }
    public String getId() { return id; }
    public android.os.Bundle getExtras() { return android.os.Bundle.EMPTY; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeString(text); dest.writeString(id); }
    @Override public String toString() { return "TextClassification{text=" + text + "}"; }

    public static final class Builder {
        private String text, id;

        public Builder() {}
        public Builder setText(String text) { this.text = text; return this; }
        public Builder setEntityType(String type, float confidenceScore) { return this; }
        public Builder setId(String id) { this.id = id; return this; }
        public TextClassification build() { return new TextClassification(text, id); }
    }
}
