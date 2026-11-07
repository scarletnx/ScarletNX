// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
//
// The framework's behaviour as an app sees it on Android 16, where it needs none of the driver's natives: each check
// is a behaviour that Android 16 shows (the androidprobe's cases, run on an Android 16 emulator). Run as a source file
// over the jar:
//   java -cp scarletnx-android-framework.jar tests/android_framework.java
import android.icu.text.PluralRules;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.File;
import java.net.InetAddress;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Locale;
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

    /**
     * Java's name resolver answers the names of Android's /etc/hosts as a device does, whatever the host's own say:
     * localhost is 127.0.0.1 alone, offline and online. Offline is this JVM, whose resolver the jar's
     * META-INF/services names; online, another JVM with the driver's snx.android.network=1.
     */
    private static void hostsResolve() throws Exception {
        check("localhost offline", "[localhost/127.0.0.1]", Arrays.toString(InetAddress.getAllByName("localhost")));
        Path dir = java.nio.file.Files.createTempDirectory("android_framework");
        Path online = dir.resolve("Online.java");
        java.nio.file.Files.writeString(online, "class Online { public static void main(String[] a) throws Exception {"
            + " System.out.print(java.util.Arrays.toString(java.net.InetAddress.getAllByName(\"localhost\"))); } }");
        Process p = new ProcessBuilder(Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                       "-Dsnx.android.network=1", "-cp", System.getProperty("java.class.path"),
                                       online.toString()).redirectErrorStream(true).start();
        String got = new String(p.getInputStream().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).trim();
        if (!p.waitFor(60, java.util.concurrent.TimeUnit.SECONDS)) {
            p.destroyForcibly();
            got = "the online JVM did not end in 60 s: " + got;
        }
        java.nio.file.Files.delete(online);
        java.nio.file.Files.delete(dir);
        check("localhost online", "[localhost/127.0.0.1]", got);
    }

    /** Each number's plural category in a language, as Android 16's ICU selects it */
    private static String categories(String language, double... numbers) {
        PluralRules rules = PluralRules.forLocale(Locale.forLanguageTag(language));
        StringBuilder got = new StringBuilder();
        for (double n : numbers) got.append(got.length() > 0 ? " " : "").append(rules.select(n));
        return got.toString();
    }

    /**
     * Plurals: the category of a quantity by the locale's rules (English 0 is other, Portuguese 0 is one), and the
     * entry of a plural's bag for it, keyed as aapt2 writes the quantities: one 0x01000006, other 0x01000004.
     */
    private static void plurals() throws Exception {
        check("English 0 1 2 1.5", "other one other other", categories("en-US", 0, 1, 2, 1.5));
        check("Portuguese 0 1 2 1000000", "one one other many", categories("pt-BR", 0, 1, 2, 1000000));
        check("European Portuguese 0 1", "other one", categories("pt-PT", 0, 1));
        check("French 0 1.5 2", "one one other", categories("fr", 0, 1.5, 2));
        check("Russian 1 2 5 11 21 22 1.5", "one few many many one few other",
              categories("ru", 1, 2, 5, 11, 21, 22, 1.5));
        check("Polish 1 22 25 112", "one few many many", categories("pl", 1, 22, 25, 112));
        check("Arabic 0 1 2 3 11 100", "zero one two few many other", categories("ar", 0, 1, 2, 3, 11, 100));
        check("Welsh 0 1 2 3 6 7", "zero one two few many other", categories("cy", 0, 1, 2, 3, 6, 7));
        check("Japanese 1", "other", categories("ja", 1));
        // ICU's own ways: a region it has no rules for takes its language's (Angola's Portuguese is Portugal's in
        // CLDR), and a double's fraction digits are its first 6, truncated (2.01 is 2.009999, f = 10000, not 1)
        check("Angolan Portuguese 0, Croatian 2.01 and 21", "one other one",
              categories("pt-AO", 0) + " " + categories("hr", 2.01, 21));
        java.lang.reflect.Method entry =
            android.content.res.Resources.class.getDeclaredMethod("quantityEntry", int[].class, String.class);
        entry.setAccessible(true);
        // the bag of <item quantity="one"> then <item quantity="other">: key, type, data, cookie and 4 more ints each
        int[] bag = {0x01000006, 3, 0, 0, 0, 0, 0, 0, 0x01000004, 3, 1, 0, 0, 0, 0, 0};
        int[] few = {0x01000008, 3, 0, 0, 0, 0, 0, 0};
        check("the bag entry of one, other, and few (none: other's), and of one where only few is",
              "0 8 8 -1", entry.invoke(null, bag, "one") + " " + entry.invoke(null, bag, "other") + " "
                  + entry.invoke(null, bag, "few") + " " + entry.invoke(null, few, "one"));
    }

    public static void main(String[] args) throws Exception {
        mainLoopEnds();
        logIsLoggable();
        mediaPlayerSources();
        hostsResolve();
        plurals();
        if (failures != 0) System.exit(1);
        System.out.println("android_framework: ok");
    }
}
