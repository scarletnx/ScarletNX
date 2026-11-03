// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

import android.net.Uri;
import java.util.ArrayList;

/** A clip of the clipboard or a drag: items of text, URIs or intents. */
public class ClipData implements android.os.Parcelable {
    private final ClipDescription description;
    private final ArrayList<Item> items = new ArrayList<>();

    public static class Item {
        private final CharSequence text;
        private final String html;
        private final Intent intent;
        private final Uri uri;

        public Item(CharSequence text) { this(text, null, null, null); }
        public Item(CharSequence text, String htmlText) { this(text, htmlText, null, null); }
        public Item(Intent intent) { this(null, null, intent, null); }
        public Item(Uri uri) { this(null, null, null, uri); }
        public Item(CharSequence text, Intent intent, Uri uri) { this(text, null, intent, uri); }
        public Item(CharSequence text, String htmlText, Intent intent, Uri uri) { this.text = text; html = htmlText; this.intent = intent; this.uri = uri; }
        public CharSequence getText() { return text; }
        public String getHtmlText() { return html; }
        public Intent getIntent() { return intent; }
        public Uri getUri() { return uri; }
        public android.view.textclassifier.TextLinks getTextLinks() { return null; }
        public CharSequence coerceToText(Context context) { return text != null ? text : uri != null ? uri.toString() : intent != null ? intent.toUri(0) : ""; }
        public CharSequence coerceToStyledText(Context context) { return coerceToText(context); }
        public String coerceToHtmlText(Context context) { return html != null ? html : String.valueOf(coerceToText(context)); }
    }

    public ClipData(CharSequence label, String[] mimeTypes, Item item) { this(new ClipDescription(label, mimeTypes), item); }
    public ClipData(ClipDescription description, Item item) { this.description = description; items.add(item); }
    public ClipData(ClipData other) { description = other.description; items.addAll(other.items); }
    public static ClipData newPlainText(CharSequence label, CharSequence text) {
        return new ClipData(label, new String[] {ClipDescription.MIMETYPE_TEXT_PLAIN}, new Item(text));
    }
    public static ClipData newHtmlText(CharSequence label, CharSequence text, String htmlText) {
        return new ClipData(label, new String[] {ClipDescription.MIMETYPE_TEXT_HTML}, new Item(text, htmlText));
    }
    public static ClipData newIntent(CharSequence label, Intent intent) {
        return new ClipData(label, new String[] {ClipDescription.MIMETYPE_TEXT_INTENT}, new Item(intent));
    }
    public static ClipData newRawUri(CharSequence label, Uri uri) {
        return new ClipData(label, new String[] {ClipDescription.MIMETYPE_TEXT_URILIST}, new Item(uri));
    }
    public static ClipData newUri(ContentResolver resolver, CharSequence label, Uri uri) { return newRawUri(label, uri); }
    public ClipDescription getDescription() { return description; }
    public void addItem(Item item) { items.add(item); }
    public void addItem(ContentResolver resolver, Item item) { items.add(item); }
    public int getItemCount() { return items.size(); }
    public Item getItemAt(int index) { return items.get(index); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }
}
