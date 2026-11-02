<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Android interfaces

Each layer of Android that an app reaches, from its bytecode down to the kernel, and what `scarletnx-android` does
with it. The aim is a thin runtime: wherever an interface lets the runtime answer a whole subsystem at once, it does
so, and code runs as Android's only where the app itself depends on that code's behavior.

## Index

* [Strategies](#strategies)
* [The layers](#the-layers)
* [The framework, by package](#the-framework-by-package)
* [Native code](#native-code)
* [What the baseline app reaches](#what-the-baseline-app-reaches)

## Strategies

| Strategy | Meaning | Cost |
| --- | --- | --- |
| **Translate** | The app's own code, made into host code by a translator and a JIT | None at run time once cached |
| **Map** | An Android interface that a host interface already gives, nearly one to one (libcore's `java.*` onto OpenJDK) | A remap table for the differences |
| **HLE** | The runtime answers the interface itself, below the level Android implements it at. Nothing of Android's implementation runs | The interface's behavior, as apps see it |
| **Abstract** | A whole subsystem collapsed onto one host facility (every window an SDL window, every surface a Vulkan swapchain) | The subsystem's few entry points |
| **Implement** | Our own Java code, as Android's does it, because apps subclass it or depend on its exact logic (`View`, `TextView` layout) | Real code, kept minimal |
| **Off** | A service that reports itself absent or disabled, which apps must already handle (accessibility, autofill, telephony) | A stub |

An interface is HLE'd or abstracted unless the app can see the difference. Apps subclass `View` and `Activity`, so
their logic is implemented. Apps never see `system_server`, binder transactions, SurfaceFlinger or gralloc, so
those collapse. Where a native library does the work (SQLite, HarfBuzz, libdeflate), the Java side is a thin
wrapper over it through JNI, not a copy of the library in Java.

## The layers

| Layer | On Android | Here | Strategy | Status |
| --- | --- | --- | --- | --- |
| DEX bytecode | ART's interpreter, JIT and dex2oat | `src/android/dex/` to JVM classes, HotSpot's JIT | Translate | Done |
| ART internals apps touch (`dalvik.system.*`, hidden `VMRuntime` calls) | libart | Small Java shims | HLE | Planned, as apps reach them |
| libcore (`java.*`, `javax.*`) | ART's OpenJDK fork | OpenJDK's `java.base` | Map | Works; Android-only members go to a remap table in the translator when an app reaches one |
| Framework (`android.*`) | framework.jar | Our own subset, `src/android/framework/` | Implement where subclassed, else HLE or Off | In progress |
| Process start (zygote, `ActivityThread`, `LoadedApk`) | zygote and `ActivityThread.main` | `snx.android.Main` reads the manifest and starts the components in `handleBindApplication`'s order | Abstract | Done: the factory, the Application, the providers, the launcher activity |
| System services (`ActivityManager`, `PackageManager`, `WindowManager`, ...) in `system_server` | Binder IPC to another process | Plain objects in the process, called directly (`snx.android.app.SystemServices`) | HLE | The ones the baseline app asks for |
| Binder (`android.os.Binder`, `Parcel`) | `/dev/binder`, libbinder | Local binders only; `Parcel` a Java byte buffer for the apps that marshal through it | HLE | Planned |
| libandroid_runtime (the JNI of the framework) | C++ in the framework | Natives that the runtime registers (`android_jvm_register`) | HLE | XML parser, assets, text, SQLite done |
| libandroidfw (resources, binary XML, assets) | `AssetManager2`, `ResXMLParser` | `src/android/res/` (C): binary XML, resource table, configuration match | HLE | Done: binary XML, `resources.arsc`, themes, style resolution |
| libhwui (`Canvas`, `RenderNode`, `HardwareRenderer`, `Paint`, `Path`) | Skia over Vulkan or GLES, a RenderThread | Display lists that the Java side records in a byte format, drawn with Vulkan by ScarletNX's renderer ([HWUI](HWUI.md)) | HLE | Shapes, paths, strokes, text and bitmaps drawn; gradients, layers and image decoders next |
| minikin (text measure and shaping) | minikin and HarfBuzz | HarfBuzz behind `snx.android.hwui.Text` (`src/android/hwui/text.c`), which `Paint` and `Canvas` call | HLE | Shaping and metrics done |
| Fonts (`/system/fonts`, `fonts.xml`) | System font files | Noto Sans JP (the subset the Switch runtime already uses), which the build copies beside the program into `android-fonts/` | Abstract | Done, one family for every typeface |
| SurfaceFlinger, BufferQueue, gralloc, HWC | Composer process and buffer allocator | A window's surface is an SDL window's Vulkan swapchain, or an image of its own without a display | Abstract | Done |
| Input (`InputDispatcher`, `InputChannel`) | `system_server` and a socket per window | SDL events made into `MotionEvent` and `KeyEvent` on the window's thread: the left mouse button as a finger, a US keyboard | Abstract | First version |
| Native `Looper` (epoll) | libutils | The main `MessageQueue` waits in steps of 4 ms and reads SDL's event queue between them | HLE | Done |
| `Choreographer` vsync | `DisplayEventReceiver` from SurfaceFlinger | A 60 Hz timer; the swapchain's FIFO present holds the main thread to the display | Abstract | Done |
| SQLite (`android.database.sqlite`) | libsqlite and its JNI | Java classes over SQLite's amalgamation, a CPM package (`src/android/db/sqlite_jni.c`) | HLE | Done |
| `SharedPreferences`, files, databases folders | `/data/data/<package>` | A folder of the app in the data folder (`snx.android.data`, or `android/<package>` in ScarletNX's data folder), with Android's XML format for preferences | Abstract | Done |
| Settings provider (`Settings.Global`, `Secure`, `System`) | A content provider in `system_server` | A table of defaults | HLE | Planned |
| Accessibility, autofill, content capture, input methods | Services in `system_server` | Managers that report the service is off; text input from the keyboard as key events | Off | Planned |
| Connectivity, power, alarms, jobs (`JobScheduler`, `AlarmManager`) | `system_server` | Report a connected network and no power saving; jobs run on the main looper, a full battery on power is the sticky broadcast | HLE | Enough for WorkManager |
| Telephony, location, sensors, camera, Bluetooth | HALs | Report none present | Off | Not needed yet |
| Media codecs (`MediaCodec`, `ImageDecoder`) | codec2, libstagefright | FFmpeg and the image decoders the runtime has | HLE | Not needed yet |

## The framework, by package

Each package's plan, by how apps use it.

- **`android.app`.** `Activity`, `Application`, `Service`, `ContentProvider` (in `android.content`) and `Dialog`
  are implemented, because apps subclass them and depend on the order of their callbacks: the lifecycle methods,
  `ActivityLifecycleCallbacks` in their pre, normal and post forms, `ComponentCallbacks2`. `ActivityThread`,
  `LoadedApk`, `Instrumentation` and `ActivityManager`'s process records do not exist: `snx.android.Main` makes
  each component and calls its callbacks in Android's order. `AppComponentFactory` is honored, because AndroidX
  installs one.
- **`android.content`.** `Context` is implemented over a `ContextImpl` that maps the data folders onto the host.
  `PackageManager` answers from the manifest, which `snx.android.app.Package` reads as `PackageParser` does.
  `ContentResolver` reaches the app's own providers directly; no other process exists. `Intent`,
  `ComponentName` and `IntentFilter` are plain data. A broadcast goes to the receivers of the process.
- **`android.content.res`.** `Resources`, `Theme`, `TypedArray` and `AssetManager` are thin Java over the
  libandroidfw HLE. The framework's own resources (`android:` styles and attributes) are a small table that the
  runtime builds, with the values of Android 16's Material themes that apps read; there is no `framework-res.apk`.
- **`android.os`.** `Looper`, `Handler`, `Message` and `MessageQueue` are implemented (apps depend on their
  ordering), with the main queue's wait HLE'd onto SDL's event queue. `Bundle` and `Parcel` are Java. `Build`
  reports Android 16 (API 36). `SystemClock`, `Process` and `Trace` map onto the host or do nothing.
- **`android.view`.** `View`, `ViewGroup`, `MotionEvent`, `KeyEvent`, `ViewTreeObserver` and
  `ViewConfiguration` are implemented: apps (and Compose's `AndroidComposeView`) subclass them and depend on
  measure, layout, draw, attach and dispatch order. `ViewRootImpl`, `Window`, `WindowManager` and `Display` are
  abstracted: an activity's window is one SDL window, its decor a `FrameLayout` with the content view, its insets
  zero (no status or navigation bar), its display the window's size and the host's scale.
- **`android.graphics`.** Thin Java. `Canvas`, `RenderNode`, `Paint`, `Path`, `Shader` and `Typeface` hold
  native handles of the libhwui HLE; `Rect`, `RectF`, `Point`, `Color`, `Matrix` and `Insets` are plain Java.
  `Bitmap` pixels live in host memory for the CPU of the app and are uploaded to Vulkan when drawn.
- **`android.text`.** `Layout`, `StaticLayout`, `BoringLayout`, spans and `TextPaint` are implemented, with the
  measurement and line breaking natives HLE'd over HarfBuzz.
- **`android.database`.** `Cursor` and `SQLiteDatabase` are Java over SQLite's C library. An error becomes the
  exception that Android's JNI throws for its code.
- **`android.util`, `android.icu`.** Plain Java. `android.icu` maps onto `java.text` and `java.util` where an
  app reaches it.
- **`android.webkit`.** Off: no web engine. A `WebView` keeps its settings and clients, and a page that it loads
  starts and finishes blank, so an app's flow goes on. The cookie manager keeps cookies in memory.
- **Off.** `android.view.accessibility`, `android.view.autofill`, `android.view.contentcapture`,
  `android.view.translation`, `android.telephony`, `android.location`, `android.hardware`.

## Native code

An APK's `lib/arm64-v8a/*.so` are AArch64 code for bionic, recompiled as the GNU ABI runtime recompiles Linux
programs ([GNUABI](../GNUABI.md)). Their imports from Android's native libraries are hooks, as the Switch runtime's
NVN hooks are (`src/nx/gpu/nvn`):

| Library | Strategy |
| --- | --- |
| bionic (libc, libm, libdl) | Map the C library onto the host's where the ABI agrees; HLE `__system_property_get`, bionic's TLS slots and `dlopen` of the APK's own libraries. System calls that remain go to the GNU ABI layer |
| liblog | HLE onto the runtime's log |
| libandroid (`ANativeWindow`, `AAssetManager`, `ALooper`, `AConfiguration`, `AHardwareBuffer`, `ASurfaceTexture`) | HLE onto the same objects the Java side uses: a window is the SDL window's surface, an asset an APK member, a hardware buffer a Vulkan image |
| libvulkan | The host's Vulkan, with `VK_KHR_android_surface` and `VK_ANDROID_external_memory_android_hardware_buffer` answered by the hooks above |
| libEGL, libGLESv2, libGLESv3 | HLE onto Vulkan: all drawing in ScarletNX is Vulkan |
| libjnigraphics (`AndroidBitmap`) | HLE onto `android.graphics.Bitmap`'s pixels |
| libOpenSLES, libaaudio | Abstract onto SDL's audio |
| libmediandk | HLE onto FFmpeg |
| libbinder_ndk | Off: no other process to reach |

JNI between the JVM and the recompiled code goes through a bridge that converts the AArch64 calling convention, as
the Switch runtime's RtAPI does. The baseline app needs none of this: its only native library,
`libandroidx.graphics.path.so`, is AndroidX's path iterator for Android 13 and older, which Android 14 and later do
in the framework.

## What the baseline app reaches

The baseline app is a Kotlin and Jetpack Compose app with Room and WorkManager. Its DEX code references 387
framework classes, 1873 of their methods and 222 fields. 722 of the methods are in `android.view`, 366 in
`android.graphics`, 184 in `android.content`, 144 in `android.os`, 131 in `android.app` and 114 in `android.text`.
Many are behind checks of `Build.VERSION.SDK_INT` for older Android versions and never run on API 36, and the JVM
resolves a member only when code first runs it, so the framework grows from what a run reaches, not from the list.

A run now reaches the main loop: the Application, WorkManager and its database, the app's own worker, and
Compose's first frames, which the runtime records and counts when it runs without a window
(`SNX_ANDROID_HEADLESS=1`, `SNX_ANDROID_TRACE_FRAMES=1`).

The app subclasses 34 framework classes (among them `Activity`, `View`, `ViewGroup`, `ContentProvider`,
`BroadcastReceiver`, `Service`, `SQLiteOpenHelper`, `Canvas`, `RippleDrawable` and text spans) and implements 37
framework interfaces. Each of those must exist with the methods the app overrides before the app's class can load.
