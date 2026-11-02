<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The native bridge

An APK's `lib/arm64-v8a/*.so` are AArch64 shared libraries for bionic, Android's C library. The native bridge loads
them into the JVM's own process, runs their code through the recompiler, and answers their imports with host
functions. Nothing of Android's userland runs: bionic, libandroid, liblog, libdl and the graphics libraries are
high-level emulation, as [INTERFACES](INTERFACES.md) plans them. The design follows from the libraries of a large
Unity game: a 362 MB engine library with 265 MB of code, 20 smaller ones, and 624 imports from the system.

## Index

* [Memory: a guest pointer is a host pointer](#memory-a-guest-pointer-is-a-host-pointer)
* [Loading](#loading)
* [Code](#code)
* [Calls between the guest and the host](#calls-between-the-guest-and-the-host)
* [Threads](#threads)
* [JNI](#jni)
* [Vulkan: a pass-through](#vulkan-a-pass-through)
* [EGL and GLES: pending on purpose](#egl-and-gles-pending-on-purpose)
* [Status](#status)

## Memory: a guest pointer is a host pointer

The generated code reads guest address `a` at `cpu.mem + a`, with all 64 bits of `a` (`ir_state.c`). The bridge sets
`cpu.mem` to 0. A guest pointer is then a host pointer: the guest's heap is the host's `malloc`, a string the guest
passes is read where it is, and a structure the host fills (a `stat`, a `VkPhysicalDeviceProperties`) is the guest's
at once. This is what makes the HLE thin and the Vulkan calls direct.

The layouts agree where both sides are LP64 and little-endian, as AArch64 Android and x86-64 or AArch64 Linux are,
with the same alignment rules for C types. A big-endian host (PowerPC64) needs each structure that crosses the boundary
swapped field by field, in the adapters, and a type whose layout differs (bionic's `struct stat`, `pthread_mutex_t`,
`jmp_buf`, `va_list`) is converted on every host.

## Loading

`System.loadLibrary` goes to the bridge (the translator's remap table sends it there). The bridge loads a library as
bionic's linker does:

- Its `PT_LOAD` segments are mapped into the **code window**, a reservation of the host's address space that holds
  every guest library. The dispatch table covers the window, one slot for each 4 bytes.
- Its `DT_NEEDED` libraries load first: the app's own from its library folder, the system's as HLE libraries
  (their symbols are host functions).
- Relocations: `R_AARCH64_RELATIVE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT` and `IRELATIVE`. Lookups go through the GNU and
  SysV hash tables, in the order of the global group, then the library's own `DT_NEEDED` tree.
- `DT_INIT` and `DT_INIT_ARRAY` run, then `JNI_OnLoad`.

The app's libraries have no `PT_TLS`, so static TLS is not needed yet. `dlopen` and `dlsym` of guest code are the
same loader.

## Code

Code compiles when it first runs, as the GNU ABI runtime's JIT does it ([GNUABI](../GNUABI.md), "How code runs"): a
miss in the dispatch table calls `snx_recompile_from` (the recompiler library, `snx_recompiler.h`) for the function and
the functions it calls, and fills the table. Compiled code is kept in a cache by a hash of the code, so the next run
loads it. `--translate` compiles every library ahead of time into the same cache. The A64 target is
`SNX_RECOMPILER_A64_MAX`: phones are Armv8.2 or later.

## Calls between the guest and the host

- **Guest to host.** Each import gets an address in the code window's last pages, and its dispatch slot holds a host
  adapter (a `GuestFn`). The adapter reads the arguments from the AArch64 registers (`x0` to `x7`, `v0` to `v7`, then
  the stack), calls the host function, writes `x0` or `v0`, and returns to `x30`. A function pointer that the host
  gives the guest (`dlsym`, `vkGetInstanceProcAddr`, a JNI function table) is such an address.
- **Variable arguments.** AAPCS64 passes them as named ones are, in registers and then on the stack. A `printf` of the
  guest is formatted by the bridge's own formatter from the guest's registers. A `va_list` that the guest passes
  (`vsnprintf`, `__android_log_vprint`) is AAPCS64's structure, read the same way.
- **Host to guest.** A guest function that the host calls (a thread's start, `JNI_OnLoad`, a native method, a `qsort`
  comparator, a Vulkan debug callback) runs on the calling thread's CPU state: its arguments in the registers, `x30` at a
  stop address, and the dispatch loop until the function returns there. A host function pointer that the guest needs
  for a guest function is a small host trampoline that does this.

## Threads

A guest thread is a host thread. `pthread_create` starts a host thread whose CPU state runs the start routine. Each
thread that runs guest code has its own CPU state, guest stack and bionic TLS block: `TPIDR_EL0` points to the TLS
slots, where bionic's code reads the stack guard (slot 5) and the thread's own data. A thread of the JVM gets them the
first time it calls a native method.

Signals are bionic's and the kernel's on Android. Here `sigaction` of the guest records the handler. A host fault in
guest code (the GC's write barriers, a null check that the engine leaves to the hardware) enters the guest handler
with a frame the guest can return from.

## JNI

`JNIEnv` and `JavaVM` given to the guest are tables in host memory whose entries are adapter addresses. Each adapter
calls the host JVM's own JNI with the thread's host `JNIEnv`: references (`jobject`, `jclass`) and IDs pass through
unchanged, since a guest pointer is a host pointer. The `Call*Method` functions with variable arguments build a
`jvalue` array from the guest's registers and call the `A` form.

A `native` method of the app's classes is translated into a body that calls the bridge with its arguments. The bridge
finds the guest function by `RegisterNatives` (which the guest called) or by its JNI name (`Java_pkg_Class_method`)
in the loaded libraries, and runs it on the thread's CPU state. No host trampoline is generated at run time.

## Vulkan: a pass-through

The guest calls Vulkan directly. `libvulkan.so` is an HLE library whose functions are generated from the Vulkan
registry (`vk.xml`): each one is an adapter that calls the host's Vulkan (volk) with the guest's arguments as they
are. Structures, `pNext` chains, handles and mapped memory pass through, since a guest pointer is a host pointer and
the layouts agree. The adapters change only:

- function pointers: `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` give the adapters' addresses, and a callback
  that the guest gives (`VkAllocationCallbacks`, `VkDebugUtilsMessengerCreateInfoEXT`) is wrapped in a host trampoline,
- Android's own: `VK_KHR_android_surface` makes a surface of the SDL window on the guest's instance, and
  `VK_ANDROID_external_memory_android_hardware_buffer` maps to host memory. The instance extensions that the guest
  sees are the Android ones it expects,
- on a big-endian host, every structure, swapped field by field (the registry gives each member's type).

The game's window is the SDL window: while the guest owns a swapchain on it, ScarletNX's own renderer
([HWUI](HWUI.md)) stops presenting.

The game chooses Vulkan only on GPUs of its list (`assets/vulkan_gpu_list_config.txt`, mobile GPU names). The host's
GPU name is not one, so the device name that the guest reads may need to be one of the list. The choice of how is
left until the engine runs.

## EGL and GLES: pending on purpose

The engine library needs `libEGL.so` and `libGLESv2.so` (`DT_NEEDED`), so both exist, but their functions report no
display: `eglGetDisplay` gives `EGL_NO_DISPLAY`, `eglInitialize` fails with `EGL_NOT_INITIALIZED`, and a GLES call does
nothing. An engine that tries GLES then takes Vulkan. GLES over Vulkan is a later step, and is not started.

## Status

`--native-test LIB [SYMBOL...]` loads a library through the bridge and calls its functions. Done:

- the loader, with `DT_ANDROID_RELA` (packed) and `DT_RELR` relocations, `dlopen`, `dlsym` and
  `dl_iterate_phdr`,
- the JIT, whose cache makes a second load of a 362 MB engine library and its 864 constructors take 1.8 s (170 s
  the first time). The code window is at a fixed address (256 GiB), since the cache is keyed by the code's addresses,
- each thread's CPU state, guest stack and bionic TLS slots, `pthread_create`, keys with destructors,
- libc's string, memory, stdio, file, socket, time and math functions, most of them the host's own, with the
  conversions of `struct stat`, the open flags, `sysconf`, `epoll_event`, `addrinfo`, `sigset_t` and the system call
  numbers, and the guest's `printf` and `scanf`.

- `setjmp` and `longjmp` as guest code in the code window (`hle_code.c`): they move the guest's own registers and
  stack, which a host function can't,
- the imports that are objects: `__sF` and `stdin` to `stderr`, `_ctype_`, `environ`, getopt's and the time zone's,
  OpenSL ES's interface IDs (its engine reports that OpenSL ES is not there, for now) and libmediandk's format keys.

- JNI: the guest's `JNIEnv` and `JavaVM` are tables of exports over the host JVM's functions, called through one
  adapter (`hle_call`) that passes `x0` to `x5` and `v0` to `v7` as the host's integer and floating registers, which
  AAPCS64 and the System V ABI fill in the same order. The calls with variable arguments read them by the signature
  that `GetMethodID` recorded. `FindClass` falls back to the app's class loader, as Android's uses the loader of the
  class that loaded the library. `RegisterNatives` records the guest's functions.
- The translator gives each `native` method a body that calls `NativeBridge.invoke*` with its key, the object and its
  parameters (primitives as bits in a `long[]`, references in an `Object[]`). The bridge finds the function by
  `RegisterNatives` or by its JNI name, and passes the parameters as AAPCS64 does.

All 24 libraries of the Unity game load and run their constructors. From Java, `System.loadLibrary` loads them and
runs their `JNI_OnLoad`: the game's crash reporter initializes its native side. An import without an HLE yet is an export that
says so once when called and returns 0.

The recompiler's `android` target (`a64.profile`) is a phone's Armv8.2 CPU without pointer authentication, and
`getauxval(AT_HWCAP)` reports the same features.

Next: signals, libandroid's window, looper, sensor and asset functions,
OpenSL ES and the media codecs, the EGL and GLES libraries that report no display, and the Vulkan pass-through.
