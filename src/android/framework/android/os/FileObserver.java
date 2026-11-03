// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileSystems;
import java.nio.file.Path;
import java.nio.file.StandardWatchEventKinds;
import java.nio.file.WatchEvent;
import java.nio.file.WatchKey;
import java.nio.file.WatchService;

/**
 * Tells of changes to a file or the files of a folder, through the host's file watch service. The host tells of
 * creates, deletes and changes, so only CREATE, DELETE and MODIFY events come.
 */
public abstract class FileObserver {
    public static final int ACCESS = 0x1, MODIFY = 0x2, ATTRIB = 0x4, CLOSE_WRITE = 0x8, CLOSE_NOWRITE = 0x10,
        OPEN = 0x20, MOVED_FROM = 0x40, MOVED_TO = 0x80, CREATE = 0x100, DELETE = 0x200, DELETE_SELF = 0x400,
        MOVE_SELF = 0x800, ALL_EVENTS = 0xfff;

    private final File file;
    private final int mask;
    private WatchService watcher;

    @Deprecated public FileObserver(String path) { this(new File(path), ALL_EVENTS); }
    @Deprecated public FileObserver(String path, int mask) { this(new File(path), mask); }
    public FileObserver(File file) { this(file, ALL_EVENTS); }
    public FileObserver(File file, int mask) { this.file = file; this.mask = mask; }

    public abstract void onEvent(int event, String path);

    public synchronized void startWatching() {
        if (watcher != null) return;
        boolean dir = file.isDirectory();
        Path folder = (dir ? file : file.getAbsoluteFile().getParentFile()).toPath();
        try {
            WatchService w = FileSystems.getDefault().newWatchService();
            folder.register(w, StandardWatchEventKinds.ENTRY_CREATE, StandardWatchEventKinds.ENTRY_DELETE, StandardWatchEventKinds.ENTRY_MODIFY);
            watcher = w;
            Thread t = new Thread(() -> run(w, dir), "FileObserver");
            t.setDaemon(true);
            t.start();
        } catch (IOException e) {
            watcher = null;
        }
    }

    public synchronized void stopWatching() {
        if (watcher == null) return;
        try { watcher.close(); } catch (IOException e) {}
        watcher = null;
    }

    private void run(WatchService w, boolean dir) {
        try {
            while (true) {
                WatchKey key = w.take();
                for (WatchEvent<?> e : key.pollEvents()) {
                    if (!(e.context() instanceof Path p)) continue;
                    int event = e.kind() == StandardWatchEventKinds.ENTRY_CREATE ? CREATE
                        : e.kind() == StandardWatchEventKinds.ENTRY_DELETE ? DELETE : MODIFY;
                    String name = p.toString();
                    if (!dir) {
                        if (!name.equals(file.getName())) continue;
                        if (event == DELETE) event = DELETE_SELF;
                        name = null;
                    }
                    if ((event & mask) != 0) onEvent(event, name);
                }
                if (!key.reset()) break;
            }
        } catch (InterruptedException | java.nio.file.ClosedWatchServiceException e) {
        }
    }
}
