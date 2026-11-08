// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.graphics.drawable.Icon;

/** A person in a conversation notification or a shortcut. */
public final class Person implements android.os.Parcelable {
    private final CharSequence name;
    private final Icon icon;
    private final String uri, key;
    private final boolean bot, important;

    private Person(Builder b) {
        name = b.name;
        icon = b.icon;
        uri = b.uri;
        key = b.key;
        bot = b.bot;
        important = b.important;
    }
    public Builder toBuilder() {
        return new Builder().setName(name).setIcon(icon).setUri(uri).setKey(key).setBot(bot).setImportant(important);
    }
    public CharSequence getName() { return name; }
    public Icon getIcon() { return icon; }
    public String getUri() { return uri; }
    public String getKey() { return key; }
    public boolean isBot() { return bot; }
    public boolean isImportant() { return important; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static class Builder {
        private CharSequence name;
        private Icon icon;
        private String uri, key;
        private boolean bot, important;

        public Builder() {}
        public Builder setName(CharSequence name) { this.name = name; return this; }
        public Builder setIcon(Icon icon) { this.icon = icon; return this; }
        public Builder setUri(String uri) { this.uri = uri; return this; }
        public Builder setKey(String key) { this.key = key; return this; }
        public Builder setImportant(boolean important) { this.important = important; return this; }
        public Builder setBot(boolean bot) { this.bot = bot; return this; }
        public Person build() { return new Person(this); }
    }
}
