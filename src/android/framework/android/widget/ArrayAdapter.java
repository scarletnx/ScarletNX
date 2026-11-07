// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.Resources;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** An adapter over a list of objects, each shown as the text of its toString() in a TextView. */
public class ArrayAdapter<T> extends BaseAdapter implements Filterable, ThemedSpinnerAdapter {
    private final Object lock = new Object();
    private final Context context;
    private final int resource;
    private int dropDownResource;
    private final int fieldId;
    private List<T> objects;
    private ArrayList<T> originalValues;
    private boolean notifyOnChange = true;
    private Resources.Theme dropDownTheme;
    private ArrayFilter filter;

    public ArrayAdapter(Context context, int resource) { this(context, resource, 0, new ArrayList<T>()); }
    public ArrayAdapter(Context context, int resource, int textViewResourceId) { this(context, resource, textViewResourceId, new ArrayList<T>()); }
    public ArrayAdapter(Context context, int resource, T[] objects) { this(context, resource, 0, Arrays.asList(objects)); }
    public ArrayAdapter(Context context, int resource, int textViewResourceId, T[] objects) { this(context, resource, textViewResourceId, Arrays.asList(objects)); }
    public ArrayAdapter(Context context, int resource, List<T> objects) { this(context, resource, 0, objects); }
    public ArrayAdapter(Context context, int resource, int textViewResourceId, List<T> objects) {
        this.context = context;
        this.resource = dropDownResource = resource;
        this.fieldId = textViewResourceId;
        this.objects = objects;
    }

    /** The list that the adapter changes: the filtered copy when a filter ran, else the list it got. */
    private List<T> target() { return originalValues != null ? originalValues : objects; }
    public void add(T object) { synchronized (lock) { target().add(object); } changed(); }
    public void addAll(Collection<? extends T> collection) { synchronized (lock) { target().addAll(collection); } changed(); }
    @SuppressWarnings("unchecked") public void addAll(T... items) { synchronized (lock) { Collections.addAll(target(), items); } changed(); }
    public void insert(T object, int index) { synchronized (lock) { target().add(index, object); } changed(); }
    public void remove(T object) { synchronized (lock) { target().remove(object); } changed(); }
    public void clear() { synchronized (lock) { target().clear(); } changed(); }
    public void sort(Comparator<? super T> comparator) { synchronized (lock) { Collections.sort(target(), comparator); } changed(); }
    private void changed() { if (notifyOnChange) notifyDataSetChanged(); }
    @Override public void notifyDataSetChanged() { super.notifyDataSetChanged(); notifyOnChange = true; }
    public void setNotifyOnChange(boolean notifyOnChange) { this.notifyOnChange = notifyOnChange; }
    public Context getContext() { return context; }
    @Override public int getCount() { return objects.size(); }
    @Override public T getItem(int position) { return objects.get(position); }
    public int getPosition(T item) { return objects.indexOf(item); }
    @Override public long getItemId(int position) { return position; }

    @Override public View getView(int position, View convertView, ViewGroup parent) { return createView(LayoutInflater.from(context), position, convertView, parent, resource); }
    private View createView(LayoutInflater inflater, int position, View convertView, ViewGroup parent, int res) {
        View view = convertView != null ? convertView : inflater.inflate(res, parent, false);
        TextView text;
        try {
            text = fieldId == 0 ? (TextView) view : (TextView) view.findViewById(fieldId);
        } catch (ClassCastException e) {
            throw new IllegalStateException("ArrayAdapter requires the resource ID to be a TextView", e);
        }
        if (text == null) throw new RuntimeException("Failed to find view with ID " + fieldId + " in item layout");
        T item = getItem(position);
        text.setText(item instanceof CharSequence cs ? cs : String.valueOf(item));
        return view;
    }

    public void setDropDownViewResource(int resource) { dropDownResource = resource; }
    @Override public void setDropDownViewTheme(Resources.Theme theme) { dropDownTheme = theme; }
    @Override public Resources.Theme getDropDownViewTheme() { return dropDownTheme; }
    @Override public View getDropDownView(int position, View convertView, ViewGroup parent) {
        Context c = dropDownTheme != null ? new android.view.ContextThemeWrapper(context, dropDownTheme) : context;
        return createView(LayoutInflater.from(c), position, convertView, parent, dropDownResource);
    }

    public static ArrayAdapter<CharSequence> createFromResource(Context context, int textArrayResId, int textViewResId) {
        return new ArrayAdapter<>(context, textViewResId, 0, new ArrayList<>(Arrays.asList(context.getResources().getTextArray(textArrayResId))));
    }

    @Override public Filter getFilter() { if (filter == null) filter = new ArrayFilter(); return filter; }
    @Override public CharSequence[] getAutofillOptions() {
        CharSequence[] explicit = super.getAutofillOptions();
        if (explicit != null) return explicit;
        if (objects.isEmpty()) return null;
        CharSequence[] options = new CharSequence[objects.size()];
        for (int i = 0; i < options.length; ++i) options[i] = String.valueOf(objects.get(i));
        return options;
    }

    /** Keeps the items whose text, or a word of it, starts with the prefix, without regard to case. */
    private final class ArrayFilter extends Filter {
        @Override protected FilterResults performFiltering(CharSequence prefix) {
            FilterResults results = new FilterResults();
            ArrayList<T> values;
            synchronized (lock) {
                if (originalValues == null) originalValues = new ArrayList<>(objects);
                values = new ArrayList<>(originalValues);
            }
            if (prefix == null || prefix.length() == 0) { results.values = values; results.count = values.size(); return results; }
            String p = prefix.toString().toLowerCase(Locale.ROOT);
            ArrayList<T> kept = new ArrayList<>();
            for (T v : values) {
                String text = String.valueOf(v).toLowerCase(Locale.ROOT);
                if (text.startsWith(p)) { kept.add(v); continue; }
                for (String word : text.split(" ")) if (word.startsWith(p)) { kept.add(v); break; }
            }
            results.values = kept;
            results.count = kept.size();
            return results;
        }
        @SuppressWarnings("unchecked")
        @Override protected void publishResults(CharSequence constraint, FilterResults results) {
            objects = (List<T>) results.values;
            if (results.count > 0) notifyDataSetChanged(); else notifyDataSetInvalidated();
        }
    }
}
