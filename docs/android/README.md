<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Android apps

`scarletnx-android` runs Android apps on a desktop. (ScarletNX's own Android app, where the menu and the Switch
runtime run on a phone, is [ANDROID](../ANDROID.md).) The target is the API of Android 16 (API level 36).

## Index

* [Design](#design)
* [Interfaces](INTERFACES.md): each layer of Android an app reaches, and how far the runtime emulates it
* [HWUI](HWUI.md): the display lists and their Vulkan renderer
* [The DEX translator](#the-dex-translator)
* [Status](#status)

## Design

The layer is thin. An app's code runs on a JVM, and the Android system under it is high-level emulation:

- **Code.** An APK's DEX code is translated to JVM class files (`src/android/dex/`), and OpenJDK runs them through
  its invocation API. The JVM's JIT compiles them to host code. The translation of a whole app takes under a second,
  so classes are translated as they load and kept in a cache. `--translate=DIR` translates every class ahead of time.
- **The framework.** `android.*` is ScarletNX's own, a minimal subset that grows with the apps it runs. It is not
  AOSP's framework. Its native side is C, the HLE of the userland libraries: libhwui's display lists drawn with
  Vulkan, libandroidfw's resources, binder services in the process, SQLite.
- **Interfaces.** [INTERFACES](INTERFACES.md) lists each layer an app reaches, from DEX bytecode to the kernel,
  and what the runtime does with it: high-level emulation (HLE), a heavy abstraction onto one host facility, or a
  real implementation where an app subclasses or reads the code itself.
- **Native code.** An APK's native libraries are AArch64 code for bionic. They are to be recompiled as the GNU ABI
  runtime does ([GNUABI](../GNUABI.md)), with bionic, libandroid, EGL, GLES and Vulkan as hooks that the runtime answers,
  as the NVN hooks of the Switch runtime do. All drawing goes through Vulkan.

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
its frames into display lists, which the Vulkan renderer draws ([HWUI](HWUI.md)): its first screen shows as on a
phone, except for its bitmaps.
