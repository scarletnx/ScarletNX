// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
//
// The framework's behaviour as an app sees it on Android 16, where it needs none of the driver's natives: each check
// is a behaviour that Android 16 shows (the androidprobe's cases, run on an Android 16 emulator). Run as a source file
// over the jar:
//   java -cp scarletnx-android-framework.jar tests/android_framework.java
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;

final class AndroidFramework {
    private static int failures;

    private static void check(String what, Object want, Object got) {
        if (java.util.Objects.equals(want, got)) return;
        System.err.println("android_framework: " + what + ": want " + want + ", got " + got);
        ++failures;
    }

    private static String thrown(Throwable t) {
        return t == null ? "nothing" : t.getClass().getName() + ": " + t.getMessage();
    }

    /**
     * The last activity's finish (Main.exit, from a message on the main thread) ends the main loop with its status,
     * while the main looper itself still refuses to quit, as Android's does when an app asks it.
     */
    @SuppressWarnings("deprecation") // prepareMainLooper: the runtime prepares the main looper as Android did
    private static void mainLoopEnds() throws InterruptedException {
        AtomicReference<String> got = new AtomicReference<>("the loop did not end in 10 s");
        Thread main = new Thread(() -> {
            try {
                Looper.prepareMainLooper();
                new Handler(Looper.getMainLooper()).post(() -> snx.android.Main.exit(3));
                snx.android.Main.loop();
                String quit;
                try {
                    Looper.getMainLooper().quitSafely();
                    quit = "quit";
                } catch (IllegalStateException e) {
                    quit = e.getMessage();
                }
                got.set("status " + snx.android.Main.status() + ", main looper's quitSafely: " + quit);
            } catch (Throwable t) {
                got.set("threw " + thrown(t));
            }
        }, "android_framework main");
        main.setDaemon(true);
        main.start();
        main.join(10000);
        check("Main.exit(3) from the main thread",
              "status 3, main looper's quitSafely: Main thread not allowed to quit.", got.get());
    }

    /**
     * With no log.tag property, Android's isLoggable is true from INFO up. The driver's --log-level sets it as log.tag
     * does: a Log of a loader of its own, which reads the property as the driver gives it.
     */
    private static void logIsLoggable() throws Exception {
        check("Log.isLoggable of DEBUG and INFO", "false true",
              Log.isLoggable("android_framework", Log.DEBUG) + " " + Log.isLoggable("android_framework", Log.INFO));
        System.setProperty("snx.android.log_level", "D");
        URL jar = Path.of(System.getProperty("java.class.path")).toUri().toURL();
        try (URLClassLoader loader = new URLClassLoader(new URL[] {jar}, ClassLoader.getPlatformClassLoader())) {
            Class<?> log = Class.forName("android.util.Log", true, loader);
            check("Log.isLoggable of DEBUG with --log-level=D", true,
                  log.getMethod("isLoggable", String.class, int.class).invoke(null, "android_framework", Log.DEBUG));
        } finally {
            System.clearProperty("snx.android.log_level");
        }
    }

    /** What setDataSource of a path throws, or "returned", on a new player each */
    private static String source(String path) {
        MediaPlayer mp = new MediaPlayer();
        try {
            mp.setDataSource(path);
            return "returned";
        } catch (Exception e) {
            return e.getClass().getName();
        } finally {
            mp.release();
        }
    }

    /**
     * MediaPlayer opens a path, or a file: URI, before its decoder sees it: a missing file is FileNotFoundException,
     * which leaves the player idle. Another scheme goes to the decoder as it is.
     */
    private static void mediaPlayerSources() throws Exception {
        File dir = java.nio.file.Files.createTempDirectory("android_framework").toFile();
        File there = new File(dir, "there.ogg"), missing = new File(dir, "missing.ogg");
        java.nio.file.Files.write(there.toPath(), new byte[] {'O', 'g', 'g', 'S'});
        AtomicReference<String> got = new AtomicReference<>("the player's thread did not end in 10 s");
        Thread t = new Thread(() -> {
            Looper.prepare();
            String again;
            MediaPlayer mp = new MediaPlayer();
            try {
                mp.setDataSource(missing.getPath());
                again = "the missing file returned";
            } catch (Exception e) {
                try {
                    mp.setDataSource(there.getPath());
                    again = "then idle";
                } catch (Exception e2) {
                    again = "then " + thrown(e2);
                }
            } finally {
                mp.release();
            }
            got.set(source(missing.getPath()) + ", " + source("file://" + missing.getPath()) + ", "
                    + source(there.getPath()) + ", " + source("http://localhost:9/missing.ogg") + ", " + again);
        });
        t.setDaemon(true);
        t.start();
        t.join(10000);
        there.delete();
        dir.delete();
        check("MediaPlayer.setDataSource of a missing path, its file: URI, a file, an http: URI, and after the first",
              "java.io.FileNotFoundException, java.io.FileNotFoundException, returned, returned, then idle", got.get());
    }

    public static void main(String[] args) throws Exception {
        mainLoopEnds();
        logIsLoggable();
        mediaPlayerSources();
        if (failures != 0) System.exit(1);
        System.out.println("android_framework: ok");
    }
}
