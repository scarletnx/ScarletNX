// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import org.w3c.dom.Element;
import org.w3c.dom.Node;

/**
 * A preferences file in Android's format (a map of typed values in XML, under shared_prefs), so that the files are the
 * same as on a device. The file is read once, and each commit or apply writes all of it, apply on one writer thread.
 */
final class SharedPreferencesImpl implements SharedPreferences {
    private static final Executor writer = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "SharedPreferences");
        t.setDaemon(true);
        return t;
    });
    private static final Object NULL = new Object();

    private final File file;
    private final HashMap<String, Object> map = new HashMap<>();
    private final WeakHashMap<OnSharedPreferenceChangeListener, Object> listeners = new WeakHashMap<>();
    private long generation, written;

    SharedPreferencesImpl(File file) {
        this.file = file;
        if (file.exists()) {
            try {
                read();
            } catch (Exception e) {
                android.util.Log.w("SharedPreferences", "cannot read " + file + ": " + e);
            }
        }
    }

    private void read() throws Exception {
        var factory = javax.xml.parsers.DocumentBuilderFactory.newInstance();
        factory.setExpandEntityReferences(false);
        Element root = factory.newDocumentBuilder().parse(file).getDocumentElement();
        for (Node n = root.getFirstChild(); n != null; n = n.getNextSibling()) {
            if (!(n instanceof Element e)) continue;
            String name = e.getAttribute("name"), value = e.getAttribute("value");
            switch (e.getTagName()) {
                case "string" -> map.put(name, e.getTextContent());
                case "int" -> map.put(name, Integer.parseInt(value));
                case "long" -> map.put(name, Long.parseLong(value));
                case "float" -> map.put(name, Float.parseFloat(value));
                case "boolean" -> map.put(name, Boolean.parseBoolean(value));
                case "set" -> {
                    HashSet<String> set = new HashSet<>();
                    for (Node s = e.getFirstChild(); s != null; s = s.getNextSibling())
                        if (s instanceof Element se && se.getTagName().equals("string")) set.add(se.getTextContent());
                    map.put(name, set);
                }
                default -> {}
            }
        }
    }

    private static void escape(Writer w, String s) throws java.io.IOException {
        for (int i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            switch (c) {
                case '&' -> w.write("&amp;");
                case '<' -> w.write("&lt;");
                case '>' -> w.write("&gt;");
                case '"' -> w.write("&quot;");
                default -> {
                    if (c < 0x20 && c != '\n' && c != '\t') w.write("&#" + (int) c + ";");
                    else w.write(c);
                }
            }
        }
    }

    /** The values as a file, written beside it and renamed over it. */
    private void write(Map<String, Object> values, long generation) {
        synchronized (file) {
            if (generation <= written) return;
            File tmp = new File(file.getPath() + ".tmp");
            try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
                w.write("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n");
                for (Map.Entry<String, Object> e : values.entrySet()) {
                    Object v = e.getValue();
                    String tag = v instanceof String ? "string" : v instanceof Integer ? "int" : v instanceof Long ? "long"
                        : v instanceof Float ? "float" : v instanceof Boolean ? "boolean" : "set";
                    w.write("    <" + tag + " name=\"");
                    escape(w, e.getKey());
                    if (v instanceof String s) {
                        w.write("\">");
                        escape(w, s);
                        w.write("</string>\n");
                    } else if (v instanceof Set<?> set) {
                        w.write("\">\n");
                        for (Object s : set) {
                            w.write("        <string>");
                            escape(w, String.valueOf(s));
                            w.write("</string>\n");
                        }
                        w.write("    </set>\n");
                    } else {
                        w.write("\" value=\"" + v + "\" />\n");
                    }
                }
                w.write("</map>\n");
            } catch (java.io.IOException e) {
                android.util.Log.w("SharedPreferences", "cannot write " + file + ": " + e);
                return;
            }
            if (tmp.renameTo(file)) written = generation;
        }
    }

    @Override public synchronized Map<String, ?> getAll() { return new HashMap<>(map); }
    @Override public synchronized String getString(String key, String defValue) { Object v = map.get(key); return v != null ? (String) v : defValue; }
    @SuppressWarnings("unchecked")
    @Override public synchronized Set<String> getStringSet(String key, Set<String> defValues) { Object v = map.get(key); return v != null ? (Set<String>) v : defValues; }
    @Override public synchronized int getInt(String key, int defValue) { Object v = map.get(key); return v != null ? (Integer) v : defValue; }
    @Override public synchronized long getLong(String key, long defValue) { Object v = map.get(key); return v != null ? (Long) v : defValue; }
    @Override public synchronized float getFloat(String key, float defValue) { Object v = map.get(key); return v != null ? (Float) v : defValue; }
    @Override public synchronized boolean getBoolean(String key, boolean defValue) { Object v = map.get(key); return v != null ? (Boolean) v : defValue; }
    @Override public synchronized boolean contains(String key) { return map.containsKey(key); }
    @Override public Editor edit() { return new EditorImpl(); }
    @Override public synchronized void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) { listeners.put(listener, NULL); }
    @Override public synchronized void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) { listeners.remove(listener); }

    private record Commit(HashMap<String, Object> values, long generation, ArrayList<String> keys,
                          ArrayList<OnSharedPreferenceChangeListener> listeners) {}

    private final class EditorImpl implements Editor {
        private final HashMap<String, Object> changes = new HashMap<>();
        private boolean clear;

        @Override public synchronized Editor putString(String key, String value) { changes.put(key, value != null ? value : NULL); return this; }
        @Override public synchronized Editor putStringSet(String key, Set<String> values) {
            changes.put(key, values != null ? Collections.unmodifiableSet(new HashSet<>(values)) : NULL);
            return this;
        }
        @Override public synchronized Editor putInt(String key, int value) { changes.put(key, value); return this; }
        @Override public synchronized Editor putLong(String key, long value) { changes.put(key, value); return this; }
        @Override public synchronized Editor putFloat(String key, float value) { changes.put(key, value); return this; }
        @Override public synchronized Editor putBoolean(String key, boolean value) { changes.put(key, value); return this; }
        @Override public synchronized Editor remove(String key) { changes.put(key, NULL); return this; }
        @Override public synchronized Editor clear() { clear = true; return this; }

        /** The changes in memory: a copy of the map to write, its generation, and the keys that changed. */
        private Commit commitToMemory() {
            ArrayList<String> keys = new ArrayList<>();
            ArrayList<OnSharedPreferenceChangeListener> notify;
            HashMap<String, Object> copy;
            long g;
            synchronized (SharedPreferencesImpl.this) {
                synchronized (this) {
                    if (clear && !map.isEmpty()) map.clear();
                    for (Map.Entry<String, Object> e : changes.entrySet()) {
                        Object v = e.getValue();
                        if (v == NULL ? map.remove(e.getKey()) != null : !v.equals(map.put(e.getKey(), v))) keys.add(e.getKey());
                    }
                    changes.clear();
                    clear = false;
                }
                copy = new HashMap<>(map);
                g = ++generation;
                notify = new ArrayList<>(listeners.keySet());
            }
            return new Commit(copy, g, keys, notify);
        }

        private void notifyListeners(Commit m) {
            ArrayList<String> keys = m.keys();
            ArrayList<OnSharedPreferenceChangeListener> notify = m.listeners();
            if (keys.isEmpty() || notify.isEmpty()) return;
            Runnable r = () -> { for (String k : keys) for (OnSharedPreferenceChangeListener l : notify) l.onSharedPreferenceChanged(SharedPreferencesImpl.this, k); };
            if (Looper.myLooper() == Looper.getMainLooper()) r.run();
            else new Handler(Looper.getMainLooper()).post(r);
        }

        @Override public boolean commit() {
            Commit m = commitToMemory();
            write(m.values(), m.generation());
            notifyListeners(m);
            return true;
        }
        @Override public void apply() {
            Commit m = commitToMemory();
            writer.execute(() -> write(m.values(), m.generation()));
            notifyListeners(m);
        }
    }
}
