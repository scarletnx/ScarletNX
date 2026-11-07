// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentName;

/** An activity's searchable configuration. SearchManager returns none, so each value is the attribute's default. */
public final class SearchableInfo implements android.os.Parcelable {
    private final ComponentName searchActivity;

    private SearchableInfo(ComponentName searchActivity) { this.searchActivity = searchActivity; }
    public ComponentName getSearchActivity() { return searchActivity; }
    public String getSuggestAuthority() { return null; }
    public String getSuggestPath() { return null; }
    public String getSuggestSelection() { return null; }
    public String getSuggestIntentAction() { return null; }
    public String getSuggestIntentData() { return null; }
    public int getSuggestThreshold() { return 0; }
    public int getHintId() { return 0; }
    public int getInputType() { return 1; } // InputType.TYPE_CLASS_TEXT
    public int getImeOptions() { return 3; } // EditorInfo.IME_ACTION_SEARCH
    public boolean getVoiceSearchEnabled() { return false; }
    public boolean getVoiceSearchLaunchWebSearch() { return false; }
    public boolean getVoiceSearchLaunchRecognizer() { return false; }
    public int getVoiceLanguageModeId() { return 0; }
    public int getVoicePromptTextId() { return 0; }
    public int getVoiceLanguageId() { return 0; }
    public int getVoiceMaxResults() { return 0; }
    public boolean shouldRewriteQueryFromData() { return false; }
    public boolean shouldRewriteQueryFromText() { return false; }
    public boolean shouldIncludeInGlobalSearch() { return false; }
    public boolean queryAfterZeroResults() { return false; }
    public boolean autoUrlDetect() { return false; }
    public int getSettingsDescriptionId() { return 0; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
