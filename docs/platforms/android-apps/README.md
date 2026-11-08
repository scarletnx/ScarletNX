<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Android apps

`scarletnx-android` runs Android apps on a desktop. (ScarletNX's own Android app, where the menu and the Switch
runtime run on a phone, is [Android host](../../hosts/android-app.md).) The target is the API of Android 16 (API level 36).

## Index

* [Layers](#layers)
* [Design](#design)
* [Running an app](#running-an-app)
* [The DEX translator](#the-dex-translator)
* [Status](#status)

## Layers

This is the guest platform of Android apps. For the ScarletNX app that plays Switch games on a phone, see
[Android host](../../hosts/android-app.md).

| Layer | Code | Documents |
| --- | --- | --- |
| Loader | the install of an APK, an XAPK or an APKS (`src/android/runtime/install.c`), resources (`src/android/res/`) | [Running an app](#running-an-app) |
| CPU | DEX code translated to JVM classes (`src/android/dex/`), and native AArch64 or 32-bit libraries through the recompiler (`src/android/bridge/`) | [The DEX translator](#the-dex-translator), [The native bridge](native.md) |
| Kernel and HLE | the framework (`src/android/framework/`), binder services in the process, SQLite (`src/android/db/`) | [Interfaces](interfaces.md) |
| GPU | HWUI's display lists on Vulkan (`src/android/hwui/`), EGL, GLES and Vulkan for native code | [HWUI](hwui.md), [The native bridge](native.md) |
| Audio | `MediaPlayer` and `SoundPool` through FFmpeg and one SDL audio stream (`src/android/runtime/audio.c`) | [Interfaces](interfaces.md) |
| Input | the mouse as a finger and the keyboard as a hardware keyboard | [Running an app](#running-an-app) |
| Platform | the JVM, and the host layer that every driver shares (`src/common/platform/`) | [Running an app](#running-an-app) |

## Design

The layer is thin. An app's code runs on a JVM, and the Android system under it is high-level emulation:

- **Code.** An APK's DEX code is translated to JVM class files (`src/android/dex/`), and OpenJDK runs them through
  its invocation API. The JVM's JIT compiles them to host code. The translation of a whole app takes under a second,
  so classes are translated as they load and kept in a cache. `--translate=DIR` translates every class ahead of time.
- **The framework.** `android.*` is ScarletNX's own, a minimal subset that grows with the apps it runs. It is not
  AOSP's framework, though the files whose header names AOSP follow its source in parts ([Credits](../../CREDITS.md)).
  Its native side is C, the HLE of the userland libraries: libhwui's display lists drawn with Vulkan, libandroidfw's
  resources, binder services in the process, SQLite.
- **Interfaces.** [Interfaces](interfaces.md) lists each layer an app reaches, from DEX bytecode to the kernel,
  and what the runtime does with it: high-level emulation (HLE), a heavy abstraction onto one host facility, or a
  real implementation where an app subclasses or reads the code itself.
- **Native code.** An APK's native libraries are AArch64 code for bionic. They are to be recompiled as the GNU ABI
  runtime does ([GNU ABI](../gnuabi/README.md)), with bionic, libandroid, EGL, GLES and Vulkan as hooks that the runtime answers,
  as the NVN hooks of the Switch runtime do. All drawing goes through Vulkan.

An app runs with the rights of the user who starts it, as a native program does: nothing confines it. Its Java code
runs on the JVM as any Java program's, its native code in the same process (a guest pointer is a host pointer,
[Native bridge](native.md)), and `open`, `java.io` and `getenv` reach the user's files and environment as they are. Run an app
only where you would run a program from the same source.

## Running an app

Build it with LLVM 22 (`-DSNX_LLVM_CONFIG=` its `llvm-config`, [Build](../../dev/build.md)). The JIT compiles an app's
native code with the LLVM the build links, and LLVM 19, FreeBSD's `llvm` package, crashes on some of an engine's code.
The configure step warns when the Android driver is built with an older one.

`build/scarletnx-android APP.apk` runs an app in a window, with the left mouse button as a finger and the keyboard
as a hardware keyboard. `APP` can also be a bundle of a base APK and its splits (an XAPK or an APKS), which is
installed first, as Android's package manager installs one: its APKs into `android/app/<package>/` of the data
folder (`base.apk`, `split_<name>.apk`), with the native libraries of `arm64-v8a` in its `lib/arm64/`, or those of
`armeabi-v7a` or `armeabi` in its `lib/arm/` when the app has no 64-bit ones ([Native bridge](native.md#32-bit-libraries)).
The app's data folder has a `lib` link to them, as Android's has. A stamp keeps
the bundle's path and size, so the next run starts at once. The app's own data is in `android/data/<package>/`. The build puts the framework's jar and the font beside the program. `-J OPTION` gives the JVM
an option, as these system properties:

| Property | Meaning |
| --- | --- |
| `snx.android.width`, `snx.android.height`, `snx.android.dpi` | The display: 600 by 1066 pixels at 240 dpi when not given |
| `snx.android.night` | `true` for the dark theme |
| `snx.android.data` | The app's data folder: `android/data/<package>` in ScarletNX's data folder when not given |

These options of `scarletnx-android` run an app without a user, as the Switch runtime's `--script` and `--shot` do.
`--headless` (`-H`) and `--shot` are [common options](../../user/usage.md#common-options), which take a value as
`--shot PATH` or `--shot=PATH`:

| Option | Meaning |
| --- | --- |
| `-H`, `--headless` | No window: the renderer draws into an image of its own (`--trace-frames` logs each frame) |
| `--shot FILE.ppm` | Save frames as binary PPM. `%ld` in the name is the frame's number. An app that draws with Vulkan has its own presents saved: the image of each listed `vkQueuePresentKHR`, read back after the copy that waits for the present's semaphores |
| `--shot-frame=60,120` | The frames (or the Vulkan presents, the first 1) to save, 60 when not given |
| `--taps="60:300,672 90:..."` | A finger down at frame 60 at (300, 672) in pixels, and up 3 frames later |
| `--type='120:2+3\n\|300:x'` | Text typed from frame 120 on, as key presses of a US keyboard (`\n` is Enter). An older version named it `--keys`, which the other drivers use for a key file: `--keys` now stops with a message that names `--type` |
| `--key-frames=4` | The frames between scripted keys. An app gets a frame or more to take each key, as from a person |

These find what an app does when it stops or runs slowly:

| Option | Meaning |
| --- | --- |
| `--log-level=L` | The least `android.util.Log` level printed: `V`, `D` (the default), `I`, `W`, `E` or `A`. It is also the least level `Log.isLoggable` is true for, as Android's `log.tag` property sets it; without it, `I`, as on Android |
| `--sample` | Every 5 seconds, each guest thread's pc, x30 and HLE call, and the count of Vulkan presents |
| `--log-signals` | Each guest signal action, send and delivery |
| `--jit-verbose`, `--jit-opt=0` | Each unit the JIT compiles with its time, and its code at -O0, which compiles several times faster to test the JIT itself (2 is the default) |
| `--jit-keep-ir=DIR` | The LLVM IR of each unit the JIT compiles, saved in DIR, so a unit that LLVM fails on can be compiled again by itself. `--jit-verbose` also says each unit's entry before it compiles, so a crash in LLVM names it |
| `--linker-verbose` | Each native library and its load address |
| `--framework=JAR`, `--java-home=DIR` | The framework's jar and the JDK, in place of the ones beside the program and that the build found |

Without a display, the renderer draws into an image of its own, so the shots work on a machine with only a
software Vulkan driver (lavapipe).

`--translate=DIR APP.apk` writes every class of the app as a JVM class file into `DIR`, which `javap` reads.

## The DEX translator

`dex_translate_class` (`src/android/dex/translate.h`) makes a version 52 class file, with stack map frames, from a
DEX class. Dalvik registers have no types, and JVM locals do. The translator gives each register up to five JVM
locals of fixed types, one for each way the code reads it: int, float, long, double and `java/lang/Object`. Each
local gets a value at the method's entry and keeps its type, so every branch target has the same frame. A value that
the code needs as a narrower class gets a `checkcast` where it is used.

A dataflow pass over the registers decides what the bytecode alone does not tell:

- whether `if-eqz` and `if-eq` compare numbers or references,
- which array type an `aget` or `aput` reads (`int[]` or `float[]`, `long[]` or `double[]`),
- which register holds the object of a `new-instance` before its `<init>`. The translator puts the JVM's `new` and
  `dup` at the `<init>` call, so no uninitialized object goes through a branch,
- when a constructor's `this` is initialized.

ART allows some things that the JVM verifier does not. The translator rewrites them:

- R8 inlines a trivial constructor. A constructor can then call an ancestor's `<init>` that is not its direct
  superclass's, and a `new-instance` can be followed by an ancestor's `<init>`. `dex_program_open` reads every
  method first. Each class from the instance's class up to that ancestor then gets a synthetic constructor that
  calls its superclass's. Its descriptor has two more parameters (`Lsnx/dalvik/Init;` and the ancestor's type), so
  it is never one of the class's own constructors.
- R8 rebinds a member reference to the class that declares it. The JVM allows a protected member of a class in
  another package only on the current class or its subclasses. The receiver is cast to its own static type when the
  dataflow knows it and it is a class of the app (not an interface). ART verified that this type is assignable to
  the declaring class, also when the chain between them goes through framework classes (an app's `ViewGroup` that
  calls a `View` method).
- `invoke-super` of a method of an interface that is not a direct superinterface becomes an `invokespecial` of the
  superclass's method. JVM resolution from the superclass finds the default method that it inherits.
- `fill-array-data` becomes a call to `snx.dalvik.Runtime.fillArrayData` with the payload in string constants.

DEX annotations become JVM attributes, so that reflection finds them: the runtime-visible ones become
`RuntimeVisibleAnnotations` and `RuntimeVisibleParameterAnnotations`, `dalvik.annotation.Signature` becomes
`Signature`, and `AnnotationDefault` becomes each element's `AnnotationDefault` (`src/android/dex/annotations.c`).
The other system annotations (inner classes, enclosing methods, `throws`) are dropped.

A method that the translator cannot translate gets a body that throws `VerifyError` with the reason. It does not
make the whole class fail.

## Status

The translator translates each class of the baseline app (3725 classes, about 19400 methods) in under a second. Each
class whose dependencies outside the app exist passes HotSpot's verifier. The app runs to its main loop and draws
its frames into display lists, which the Vulkan renderer draws ([HWUI](hwui.md)). The baseline app works: a tap
opens a new file, and typed lines show their results, with the file in its Room database.
