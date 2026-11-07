// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
//
// The framework's behaviour as an app sees it on Android 16, where it needs none of the driver's natives: each check
// is a behaviour that Android 16 shows (the androidprobe's cases, run on an Android 16 emulator). Run as a source file
// over the jar:
//   java -cp scarletnx-android-framework.jar tests/android_framework.java
import android.os.Handler;
import android.os.Looper;
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

    public static void main(String[] args) throws Exception {
        mainLoopEnds();
        if (failures != 0) System.exit(1);
        System.out.println("android_framework: ok");
    }
}
