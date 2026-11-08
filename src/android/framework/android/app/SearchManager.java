// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.ComponentName;
import android.content.DialogInterface;
import android.os.Bundle;
import java.util.Collections;
import java.util.List;

/** The search service: no activity of the app has searchable metadata, and no global search exists. */
public class SearchManager implements DialogInterface.OnDismissListener, DialogInterface.OnCancelListener {
    public static final char MENU_KEY = 's';
    public static final int MENU_KEYCODE = 47, FLAG_QUERY_REFINEMENT = 1;
    public static final String QUERY = "query", USER_QUERY = "user_query", APP_DATA = "app_data", EXTRA_DATA_KEY = "intent_extra_data_key",
        ACTION_KEY = "action_key", ACTION_MSG = "action_msg", EXTRA_SELECT_QUERY = "select_query", EXTRA_NEW_SEARCH = "new_search",
        INTENT_ACTION_GLOBAL_SEARCH = "android.search.action.GLOBAL_SEARCH",
        SUGGEST_MIME_TYPE = "vnd.android.cursor.dir/vnd.android.search.suggest", SHORTCUT_MIME_TYPE = "vnd.android.cursor.item/vnd.android.search.suggest",
        SUGGEST_URI_PATH_QUERY = "search_suggest_query", SUGGEST_URI_PATH_SHORTCUT = "search_suggest_shortcut",
        SUGGEST_COLUMN_FORMAT = "suggest_format", SUGGEST_COLUMN_TEXT_1 = "suggest_text_1", SUGGEST_COLUMN_TEXT_2 = "suggest_text_2",
        SUGGEST_COLUMN_TEXT_2_URL = "suggest_text_2_url", SUGGEST_COLUMN_ICON_1 = "suggest_icon_1", SUGGEST_COLUMN_ICON_2 = "suggest_icon_2",
        SUGGEST_COLUMN_INTENT_ACTION = "suggest_intent_action", SUGGEST_COLUMN_INTENT_DATA = "suggest_intent_data",
        SUGGEST_COLUMN_INTENT_EXTRA_DATA = "suggest_intent_extra_data", SUGGEST_COLUMN_INTENT_DATA_ID = "suggest_intent_data_id",
        SUGGEST_COLUMN_QUERY = "suggest_intent_query", SUGGEST_COLUMN_SHORTCUT_ID = "suggest_shortcut_id",
        SUGGEST_COLUMN_SPINNER_WHILE_REFRESHING = "suggest_spinner_while_refreshing", SUGGEST_COLUMN_FLAGS = "suggest_flags",
        SUGGEST_NEVER_MAKE_SHORTCUT = "_-1", SUGGEST_PARAMETER_LIMIT = "limit";

    public interface OnDismissListener { void onDismiss(); }
    public interface OnCancelListener { void onCancel(); }

    private OnDismissListener onDismiss;
    private OnCancelListener onCancel;

    /** No search dialog exists: a search starts nothing, so it is dismissed at once. */
    public void startSearch(String initialQuery, boolean selectInitialQuery, ComponentName launchActivity, Bundle appSearchData, boolean globalSearch) {
        if (onDismiss != null) onDismiss.onDismiss();
    }
    public void triggerSearch(String query, ComponentName launchActivity, Bundle appSearchData) {}
    public void stopSearch() {}
    public void setOnDismissListener(OnDismissListener listener) { onDismiss = listener; }
    public void setOnCancelListener(OnCancelListener listener) { onCancel = listener; }
    @Deprecated @Override public void onCancel(DialogInterface dialog) { if (onCancel != null) onCancel.onCancel(); }
    @Deprecated @Override public void onDismiss(DialogInterface dialog) { if (onDismiss != null) onDismiss.onDismiss(); }
    public SearchableInfo getSearchableInfo(ComponentName componentName) { return null; }
    public List<SearchableInfo> getSearchablesInGlobalSearch() { return Collections.emptyList(); }
    public ComponentName getGlobalSearchActivity() { return null; }
    public android.content.Intent getAssistIntent(boolean inclContext) { return null; }
}
