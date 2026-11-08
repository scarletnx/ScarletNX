// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.app;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** A reply that a notification action asks for. The results travel in the intent's clip data, as on Android. */
public final class RemoteInput implements android.os.Parcelable {
    public static final String RESULTS_CLIP_LABEL = "android.remoteinput.results", EXTRA_RESULTS_DATA = "android.remoteinput.resultsData";
    public static final int SOURCE_FREE_FORM_INPUT = 0, SOURCE_CHOICE = 1;
    public static final int EDIT_CHOICES_BEFORE_SENDING_AUTO = 0, EDIT_CHOICES_BEFORE_SENDING_DISABLED = 1, EDIT_CHOICES_BEFORE_SENDING_ENABLED = 2;
    private static final String EXTRA_DATA_TYPE_RESULTS_DATA = "android.remoteinput.dataTypeResultsData", EXTRA_RESULTS_SOURCE = "android.remoteinput.resultsSource";
    private final String resultKey;
    private final CharSequence label;
    private final CharSequence[] choices;
    private final boolean allowFreeFormInput;
    private final int editChoicesBeforeSending;
    private final Bundle extras;
    private final Set<String> allowedDataTypes;

    private RemoteInput(Builder b) {
        resultKey = b.resultKey;
        label = b.label;
        choices = b.choices;
        allowFreeFormInput = b.allowFreeFormInput;
        editChoicesBeforeSending = b.editChoicesBeforeSending;
        extras = new Bundle(b.extras);
        allowedDataTypes = new HashSet<>(b.allowedDataTypes);
    }
    public String getResultKey() { return resultKey; }
    public CharSequence getLabel() { return label; }
    public CharSequence[] getChoices() { return choices; }
    public Set<String> getAllowedDataTypes() { return allowedDataTypes; }
    public boolean isDataOnly() { return !allowFreeFormInput && (choices == null || choices.length == 0) && !allowedDataTypes.isEmpty(); }
    public boolean getAllowFreeFormInput() { return allowFreeFormInput; }
    public int getEditChoicesBeforeSending() { return editChoicesBeforeSending; }
    public Bundle getExtras() { return extras; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }

    /** The intent inside the clip data that holds the results, or null. */
    private static Intent clipIntent(Intent intent) {
        ClipData clip = intent.getClipData();
        if (clip == null || clip.getItemCount() == 0) return null;
        ClipDescription d = clip.getDescription();
        if (d == null || !RESULTS_CLIP_LABEL.equals(String.valueOf(d.getLabel()))) return null;
        return clip.getItemAt(0).getIntent();
    }
    private static Intent ensureClipIntent(Intent intent) {
        Intent c = clipIntent(intent);
        if (c == null) {
            c = new Intent();
            intent.setClipData(ClipData.newIntent(RESULTS_CLIP_LABEL, c));
        }
        return c;
    }
    public static Bundle getResultsFromIntent(Intent intent) {
        Intent c = clipIntent(intent);
        return c != null ? c.getBundleExtra(EXTRA_RESULTS_DATA) : null;
    }
    public static Map<String, Uri> getDataResultsFromIntent(Intent intent, String remoteInputResultKey) {
        Intent c = clipIntent(intent);
        if (c == null || c.getExtras() == null) return null;
        HashMap<String, Uri> results = new HashMap<>();
        for (String key : c.getExtras().keySet()) {
            if (!key.startsWith(EXTRA_DATA_TYPE_RESULTS_DATA)) continue;
            Bundle b = c.getBundleExtra(key);
            String uri = b != null ? b.getString(remoteInputResultKey) : null;
            if (uri != null) results.put(key.substring(EXTRA_DATA_TYPE_RESULTS_DATA.length()), Uri.parse(uri));
        }
        return results.isEmpty() ? null : results;
    }
    public static void addResultsToIntent(RemoteInput[] remoteInputs, Intent intent, Bundle results) {
        Intent c = ensureClipIntent(intent);
        Bundle out = c.getBundleExtra(EXTRA_RESULTS_DATA);
        if (out == null) out = new Bundle();
        for (RemoteInput r : remoteInputs) {
            Object v = results.get(r.getResultKey());
            if (v instanceof CharSequence cs) out.putCharSequence(r.getResultKey(), cs);
        }
        c.putExtra(EXTRA_RESULTS_DATA, out);
    }
    public static void addDataResultToIntent(RemoteInput remoteInput, Intent intent, Map<String, Uri> results) {
        Intent c = ensureClipIntent(intent);
        for (Map.Entry<String, Uri> e : results.entrySet()) {
            String key = EXTRA_DATA_TYPE_RESULTS_DATA + e.getKey();
            Bundle b = c.getBundleExtra(key);
            if (b == null) b = new Bundle();
            b.putString(remoteInput.getResultKey(), e.getValue().toString());
            c.putExtra(key, b);
        }
    }
    public static void setResultsSource(Intent intent, int source) { ensureClipIntent(intent).putExtra(EXTRA_RESULTS_SOURCE, source); }
    public static int getResultsSource(Intent intent) {
        Intent c = clipIntent(intent);
        return c != null ? c.getIntExtra(EXTRA_RESULTS_SOURCE, SOURCE_FREE_FORM_INPUT) : SOURCE_FREE_FORM_INPUT;
    }

    public static final class Builder {
        private final String resultKey;
        private final Bundle extras = new Bundle();
        private final HashSet<String> allowedDataTypes = new HashSet<>();
        private CharSequence label;
        private CharSequence[] choices;
        private boolean allowFreeFormInput = true;
        private int editChoicesBeforeSending = EDIT_CHOICES_BEFORE_SENDING_AUTO;

        public Builder(String resultKey) {
            if (resultKey == null) throw new IllegalArgumentException("Result key can't be null");
            this.resultKey = resultKey;
        }
        public Builder setLabel(CharSequence label) { this.label = label; return this; }
        public Builder setChoices(CharSequence[] choices) { this.choices = choices != null ? choices.clone() : null; return this; }
        public Builder setAllowDataType(String mimeType, boolean doAllow) {
            if (doAllow) allowedDataTypes.add(mimeType);
            else allowedDataTypes.remove(mimeType);
            return this;
        }
        public Builder setAllowFreeFormInput(boolean allowFreeFormTextInput) { allowFreeFormInput = allowFreeFormTextInput; return this; }
        public Builder setEditChoicesBeforeSending(int editChoicesBeforeSending) { this.editChoicesBeforeSending = editChoicesBeforeSending; return this; }
        public Builder addExtras(Bundle extras) { if (extras != null) this.extras.putAll(extras); return this; }
        public Bundle getExtras() { return extras; }
        public RemoteInput build() { return new RemoteInput(this); }
    }
}
