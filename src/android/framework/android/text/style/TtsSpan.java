// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

/** Speech markup for text. */
public class TtsSpan {
    public static final String TYPE_VERBATIM = "android.type.verbatim";
    private final String type;
    private final android.os.PersistableBundle args;

    public TtsSpan(String type, android.os.PersistableBundle args) { this.type = type; this.args = args; }
    public String getType() { return type; }
    public android.os.PersistableBundle getArgs() { return args; }

    public static class Builder<C extends Builder<?>> {
        private final String type;
        final android.os.PersistableBundle args = new android.os.PersistableBundle();

        public Builder(String type) { this.type = type; }
        @SuppressWarnings("unchecked")
        public C setStringArgument(String arg, String value) { args.putString(arg, value); return (C) this; }
        public TtsSpan build() { return new TtsSpan(type, args); }
    }

    public static class VerbatimBuilder extends Builder<VerbatimBuilder> {
        public VerbatimBuilder() { super(TYPE_VERBATIM); }
        public VerbatimBuilder(String verbatim) { super(TYPE_VERBATIM); setStringArgument("android.arg.verbatim", verbatim); }
    }
}
