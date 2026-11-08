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
* [EGL and GLES](#egl-and-gles)
* [Hosts](#hosts)
* [32-bit libraries](#32-bit-libraries)
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
the functions it calls, breadth first inside the aligned 64 KiB block of the code window that holds it (a call out of
the block goes through the table), and fills the table. Compiled code is kept in a cache by a hash of the code, its
addresses and the entry, so the next run loads it. A unit depends on its entry alone, never on what the process
compiled before, which depends on which thread reached what first: with that, the same entry missed the cache in
every run. The A64 target is `SNX_RECOMPILER_A64_ANDROID` (Armv8.2 with LSE, RDM, FP16, DotProd and LRCPC, without
pointer authentication). `--jit-verbose` prints each unit and its time, and `--jit-opt=0` compiles at -O0, several times faster, to test the JIT itself (2 is the default; the cache keeps each level apart). A large engine reaches code its
earlier runs did not for several runs, each spent mostly in LLVM: one run of a Unity game compiled about 1100 new units
in 440 s while it loaded.

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

bionic's mutexes, condition variables, rwlocks, spinlocks, barriers, semaphores and thread attributes are the
bridge's own (`bionic_sync.c`, `hle_sync.c`), in bionic's LP64 layout inside the guest's storage, as bionic keeps them
on Android: no host object is behind them, so a static initializer, a copy or a struct the guest never zeroed means
what it means on a phone. Each waits on the kernel library's futex, so a guest signal runs its handler during the wait.
The host's own pthread types fit bionic's storage on glibc only: FreeBSD's and winpthreads' are pointers to heap
objects, and macOS's mutex and rwlock are larger than bionic's.

Signals, threads' futexes and every number of `syscall()` are the GNU ABI runtime's kernel library
(`src/gnuabi/kernel/`, [GNUABI.md](../GNUABI.md)) over the bridge's memory, whose guest addresses are the host's.
bionic's functions (`sigaction` in bionic's LP64 layout, the masks, `pthread_kill`, the waits) only convert to it.
The guest's signals are its own: the process is the JVM's too, and the guest never changes a host disposition or
mask. A handler runs in the frame Linux gives it (`common/runtime/linux_signal.c`) where every guest register is in
the thread's CPU structure: at `RtAPI.poll`, as an HLE call returns, or, for a thread that waits in a blocking HLE
call (`pthread_cond_wait`, `sem_wait`, a read) or runs no guest code, from the handler of a host real-time signal
that the sender gives it. A host fault in generated code still ends the app, as its registers are not exact there.
An undefined instruction is exact (the code stores every register before it calls `RtAPI.undef`), so it is the
guest's `SIGILL` with `ILL_ILLOPC` and the instruction's address, as Linux's `force_sig_fault` gives it: the handler
runs when the guest has one that the thread does not block, and the app ends otherwise. OpenSSL finds the CPU's
extensions this way: it runs an instruction of each one (SHA-512, SHA-3) under a handler that longjmps out.
A host fault in generated code (SIGSEGV, SIGBUS, SIGILL, SIGFPE, or an exception on Windows) prints the guest's
report before the JVM writes its own, which knows nothing of the guest (`fault.c`): the guest function or block whose
compiled code holds the faulting host instruction, with its library and offset, then the pc and registers the code
stored last and a backtrace by the guest's frame records. FreeBSD reports a general protection fault, such as a load
through a non-canonical address, as SIGBUS with BUS_OBJERR at the pc. Every other fault goes to the JVM's handler.
`--log-signals` logs each action, send and delivery, and `--sample` prints each guest thread's
pc, x30 and HLE call every 5 seconds, for a stall.

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
([HWUI](HWUI.md)) stops presenting. It draws its frames into a layer instead, which each of the guest's presents
blends over its image, as Android's compositor puts an app's windows over its `SurfaceView`
([HWUI](HWUI.md#over-an-apps-own-vulkan)). The guest's device gets the layer's import extension
(`VK_KHR_external_memory_fd`, or `VK_KHR_external_memory_win32` on Windows) and its swapchain images
`COLOR_ATTACHMENT` usage for that.

The device that the guest sees is a Mali-G710 (ARM's vendor and driver IDs), with the host's limits and features. An
engine can allow Vulkan only on the GPUs of its own list: one game's list holds recent Adreno, Xclipse and Mali GPUs
(Mali-G610 and newer), and on any other GPU the game takes GLES, which has no display here. A Mali-G710 is on such
lists.

## EGL and GLES

`libEGL.so` serves the guest's native code and Java's `EGL10` alike (`snx.android.opengl.Egl10`, the JSR 239 EGL
that `GLSurfaceView` uses): both reach the same objects in `hle_egl.c`, so a context and a surface that Java makes
current are current for the native GLES calls of the same thread, as on Android.

- **The configs** are GLES 1.1's, 2's and 3's (`EGL_OPENGL_ES_BIT`, `EGL_OPENGL_ES2_BIT`,
  `EGL_OPENGL_ES3_BIT_KHR`): RGBA 8888, RGB 888 and RGB 565, each with no depth buffer, a 16-bit one and a 24-bit one
  with 8 bits of stencil, for window surfaces and pbuffers, matched and sorted as `eglChooseConfig` does.
- **A context** of version 1 is GLES 1.1's (`hle_gles.c`). One of version 2 or 3 is GLES 3.1's (`gles2*.c`), as a
  phone's version 2 context is. Version 3.2 is refused with `EGL_BAD_MATCH`, so an engine asks for 3.1 next.
  `glGetString` names the version that is there (`OpenGL ES 3.1`, `OpenGL ES GLSL ES 3.10`), and the Mali GPU of
  `hle_gpu_name` as the renderer.
- **A context without a config** (`EGL_KHR_no_config_context`) of any version can be made current without a surface,
  and `glGetString` answers with the GPU that Vulkan reports. Engines name the GPU from such a throwaway context
  before they choose an API, and choose Vulkan on the GPUs of their own lists. A GLES 2 context without a surface
  makes a 1x1 pbuffer of its own only when a call first records commands, so a throwaway context makes no Vulkan
  device.
- **A window surface** is a swapchain on the host window (or on a headless surface). **A pbuffer** is an RGBA8 image
  of its own.

Both GLES libraries export most of their names twice (`glDrawArrays`, `glClear`): `hle.c` makes one export of each
such name, which calls the current context's.

`gles_vk.c` is the Vulkan app that both share. It makes the instance, surface, device, queue and swapchain, and
presents, through the pass-through's commands (`hle_vulkan_host.h`), so its presents get the renderer's layer and
`--shot`'s copies as a guest's own Vulkan presents do. The device is Vulkan 1.3 where the host has it, with dynamic
rendering, and `VK_EXT_depth_clip_control` and `VK_EXT_index_type_uint8` where the device has them. A frame is one
command buffer of a slot, recorded as the calls come, and two slots are in flight. Each submission has a serial: an
object that a frame may still use goes when its serial completed. A call that must see the GPU's results
(`glReadPixels`, `glFinish`, `glClientWaitSync`) submits what the frame recorded so far and waits for it, and the frame
goes on in the same command buffer. Between passes an acquired image is in `PRESENT_SRC_KHR`, so GLES 1.1's render
passes and GLES 2's dynamic rendering can share a frame.

**GLES 1.1** (`hle_gles.c`) is the fixed function. A draw goes into a render pass on the swapchain image. A texture
upload ends the pass, copies from a staging buffer and starts the pass again (loading what it drew), so each draw
reads the texture as it was at its call. The client arrays of a draw are converted into one vertex format (position,
texture coordinate and color as floats) in a buffer of the frame, as triangle, line or point lists. GL's window
origin is the bottom left: the viewport has a negative height, so GL's viewport, scissor and `glDrawTexOES`
coordinates are used as they are. A texture of 3-byte texels on a device that cannot sample that format is an RGBA8
image, which a compute shader (`gles_rgb.comp`) writes from the packed rows. It does 2D textures (the base level) in
GLES 1.1's formats, the texture environment of unit 0, the matrix stacks, `glDrawArrays`, `glDrawElements`,
`glDrawTexOES`, blending, the scissor and `glClear`. It does not do yet: depth and stencil tests, lighting, fog, the
alpha test, mipmaps, the second texture unit, and `glReadPixels`, which reads zeros.

**GLES 2, 3.0 and 3.1** (`gles2.c`, `gles2_object.c`, `gles2_program.c`, `gles2_draw.c`) are done as Mesa's zink does them:

- **Shaders** compile with glslc (`docs/GLSLC.md`). `glCompileShader` compiles for the errors and the info log.
  `glLinkProgram` compiles each stage again with a binding map: binding 0 of the program's set is the vertex
  shader's default uniform block, 1 the fragment shader's, then each sampler, image and block of one name at one
  binding for both stages. The link writes the vertex outputs' locations into the fragment shader's inputs of the
  same names, as OpenGL ES links the stages by name. The loose uniforms are the members of each stage's default
  block: the link reads their offsets, strides and types from the module, and flattens structs and arrays into GL's
  names and locations. `glUniform*` writes into each stage's copy, which a draw puts into the frame's arena when it
  changed. The vertex inputs take `glBindAttribLocation`'s locations, else glslc's.
- **Buffers** keep the guest's bytes in memory of their own, which `glMapBufferRange` gives the guest. A draw reads
  their Vulkan copy, written in place when no submission in flight reads it, else copied into the frame's arena.
  A buffer that the GPU writes (`glReadPixels` into a pack buffer) moves to host-cached memory, and is read back.
- **Textures** are one image each, with the whole chain of levels of the base level: 2D, cube maps, 3D and 2D arrays,
  GLES 2's unsized formats with luminance and alpha as swizzles, GLES 3's sized formats, depth and stencil,
  `glTexStorage*`, ETC2 and EAC and ASTC where the device has them, `glGenerateMipmap` by blits, the texture swizzle
  and the base and max levels. A texture rests in `SHADER_READ_ONLY_OPTIMAL`, so a draw samples it without a barrier,
  and an upload copies from the frame's staging buffer and returns the image to its rest. The unpack row length,
  skip and alignment are applied by copying whole rows. A 3-component format that the device does not have
  (`GL_RGB8`, `GL_RGB16F`, `GL_RGB32F` and their integer forms) is the 4-component one, which `gles_expand.comp`
  writes on the GPU: the CPU does not convert texels. Samplers are kept by their state, sampler objects included. A
  sampler whose texture is missing or incomplete reads a black texture.
- **Framebuffers**: a render target is dynamic rendering into its attachments' images, open from the first draw or
  clear into it until a copy, another target or the frame's end. The window's framebuffer is the swapchain image and
  a depth and stencil buffer of the config's sizes. Its viewport has a negative height, so GL's coordinates are kept.
  A framebuffer object's images are drawn upright instead, with the front face turned, so a texture holds its rows in
  GL's order. GL's clip depth of -1 to 1 is `VK_EXT_depth_clip_control`'s. The fragment shaders read `gl_FragCoord`
  and `gl_PointCoord` through glslc's `flip_y`, with the flip of the target in a push constant, so both are GL's in
  the window and in a framebuffer object. Renderbuffers are images in their
  attachment layout, multisampled when asked. `glBlitFramebuffer` and `glCopyTexSubImage2D` are blits, and resolves
  from a multisampled image. `glReadPixels` blits the rectangle into an RGBA8 or BGRA8 image upright, copies it into
  host-cached memory, and then copies the rows into the guest's memory.
- **Pipelines** are made from the state of a draw (the program, the target's formats, the vertex formats, the
  blending) at its first draw, and kept by a hash of that state. The rest is Vulkan 1.3's dynamic state: viewport,
  scissor, cull mode, front face, topology, depth and stencil tests, depth bias, line width, blend constants,
  primitive restart, rasterizer discard and the vertex strides.
- **Draws**: `glDrawArrays`, `glDrawElements`, `glDrawRangeElements` and their instanced forms, with vertex buffers
  or client arrays (the range a draw reads copied into the frame's arena), divisors of 0 and 1, the generic attributes'
  current values, and indices of 8, 16 and 32 bits. A line loop is a line strip with an index back to its start.
  Vertex arrays, occlusion queries (`GL_ANY_SAMPLES_PASSED`), fence syncs and uniform buffers are there.
- **GLES 3.1's calls**: compute shaders (`glDispatchCompute` and its
  indirect form), shader storage buffers, image units (`glBindImageTexture`: the texture then rests in `GENERAL`),
  `glMemoryBarrier`, indirect draws, the vertex attribute bindings (`glBindVertexBuffer`, `glVertexAttribFormat`),
  multisample textures, `glGetTexLevelParameter*`, the sample mask, the depth and stencil texture mode, and the
  program interface queries, and separate shader objects: a program pipeline draws through a hidden program that
  links its stages' shaders together, with the stages' uniforms copied into it at each draw. A dispatch has a
  barrier of every write on each side. GLES 3.1's atomic counters are not there (glslc has no `atomic_uint`), and
  their limits are 0.
- **GLES 3.2's calls that need no new shader stage**: the base vertex draws, `glCopyImageSubData`, the robustness
  calls (`glGetGraphicsResetStatus`, `glReadnPixels`, `glGetnUniform*`), and the debug calls, which do nothing. A
  context does not name itself 3.2: glslc has no geometry or tessellation stage.

What GLES 2 and 3 do not do yet: transform feedback, GLES 3.1's atomic counters and its buffer variables in the
interface queries, `glFramebufferParameteri`, GLES 3.2's geometry and tessellation, its indexed blending and color masks, texture buffers, sample shading and
border colors, a divisor past 1, `glClear` with a partial color mask (it clears every
channel), client depth and stencil texels for a 24-bit depth texture, ETC2 and ASTC on a device that has neither
(the decode on the GPU), `glReadPixels` of a format other than RGBA or BGRA bytes, program binaries, and a pbuffer bound as a texture.

`libz.so` is the build's own zlib (`hle_zlib.c`): z_stream has the guest's layout on an LP64 host, and a stream's
allocator, when the guest gives one, is called through the bridge. Windows' `uLong` is 32 bits, so Windows has no
zlib for the guest yet.

## Hosts

bionic's numbers and layouts are Linux's, so on Linux most of libc passes to the host as it is. The bridge also
builds for FreeBSD, macOS and Windows x64, where they differ:

- **The host's own calls** are `bridge/host.c`'s, one branch for each host: the code window's reservation at its
  fixed address (`MAP_FIXED_NOREPLACE` on Linux, `MAP_GUARD | MAP_FIXED | MAP_EXCL` on FreeBSD, a hint that must hold
  on macOS, `VirtualAlloc(MEM_RESERVE)` on Windows), the table and the stacks as lazy memory, a thread's id, `pread`,
  the kernel's checked copies of guest memory (`process_vm_readv` on Linux, `mach_vm_read_overwrite` on macOS, a pipe
  on FreeBSD, `ReadProcessMemory` on Windows) and an epoll set (epoll, kqueue, or WSAPoll over sockets).
- The app's own `/proc` files everywhere, as an Android device shows them (`hle_proc.c`): `cmdline` (the package),
  `comm`, `status` (no tracer), `stat`, `maps` (the guest's libraries) and the `exe` link (`app_process64`), of
  `/proc/self` and the app's pid. A Unity game's protection reads `cmdline`, `status` and `exe`, and when they fail
  (FreeBSD mounts no `/proc`) it goes on with a wrong key: its asset headers decrypted into garbage and it crashed.
  On Linux it read the JVM's own. Other `/proc` paths are the host's.
- bionic's `rand` and `random` everywhere: the BSD additive feedback generator that bionic's and glibc's `rand` are,
  in the bridge, as FreeBSD's and macOS's `rand` are other generators. A game that seeds it with a constant and makes a
  decryption's key from its numbers needs this sequence: with FreeBSD's, a Unity game decrypted its asset headers
  into garbage and crashed on a self-relative offset in one.
- Linux's `MADV_DONTNEED` everywhere: a private anonymous range reads as zero pages again after it. Garbage collectors
  hand such pages out again as zero memory: a Unity game's il2cpp collector did, and on FreeBSD, whose advice keeps
  the contents, a string builder's first chunk got a stale field for its null link and the game crashed in managed
  code. The HLE of `mmap`, `mprotect` and `munmap` records the guest's private anonymous mappings with their access,
  and `host_dontneed` maps fresh zero pages over the recorded parts (a new anonymous `MAP_FIXED` mapping, or a decommit
  and commit on Windows). Linux keeps its own advice.
- **errno**: on Linux the guest's errno is the host's. Elsewhere it is the guest's own, and each HLE call sets it
  from the host's in Linux's numbers (`gnu_host_errno`, `gnuabi/kernel/errno.c`). `strerror`, `strerror_r`, `perror`
  and `%m` give bionic's texts (`hle_error_text`): the host C library's are other words, in the user's language.
- **The locale**: bionic's C library has the C locale alone, its multibyte characters UTF-8 whatever the name.
  `setlocale`, `newlocale` and `uselocale` are bionic's (`hle_libc.c`): a name only sets `MB_CUR_MAX`, 4 or 1, a
  `locale_t` is bionic's struct of it, and the host's locale is never set. The host's is the user's, which the JVM
  sets for the process (a decimal comma, other names of days, other words), so a host call into guest code
  (`bridge_call`) runs its thread in the C locale until it returns: C.UTF-8 on Linux, whose multibyte functions
  serve the guest's. Windows' C runtime keeps the C locale, as no part of the process sets another.
- **Constants and layouts** convert by the GNU ABI's tables and the network stack's: `gnuabi/abi/linux_file.h`
  (struct stat, the open flags), `gnuabi/abi/linux_net.h` (families, flags, options, `sockaddr_in` and `sockaddr_in6`)
  and `common/network/net_host.h` (the stack's constants as the host's). The adapters in `hle_posix.c` also convert
  the mmap and msync flags, fcntl's commands and `struct flock`, the requests of ioctl, `struct dirent`, `struct
  statfs` and the poll events. `getaddrinfo`, `getnameinfo` and `gethostbyname` are the stack's on every host, whose
  flags and errors are bionic's. Offline (the Network setting), another machine's name is `EAI_NODATA`, as Android
  answers without a network. localhost is 127.0.0.1 alone, once, as Android's hosts file has it (`::1` is
  ip6-localhost there).
- **Threads**: FreeBSD's, macOS's and winpthreads' objects differ in size or in their static initializer, so a
  guest mutex, condition variable, rwlock and semaphore keep a host object's address at offset 8, made at the first
  use, and the attributes are bionic's own layout (`hle_thread.c`).
- **The carrier** that stops a parked wait for a guest signal is `SIGRTMIN + 4` on Linux and FreeBSD (`tgkill`,
  `thr_kill`) and `SIGUSR1` on macOS (`pthread_kill`), which HotSpot leaves alone on each (it takes `SIGSEGV`,
  `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGPIPE`, `SIGQUIT` and `SIGUSR2`). Windows has no signals: an APC, which runs only
  when the thread waits alertably. A thread in another wait takes its signal as its HLE call returns.
- **Windows x64** passes arguments by position, so `hle_call` takes a function's parameter classes: the four JNI
  functions that set a float or double field, and the five Vulkan commands with a float parameter (`vulkan_core.h`).
  `long` is 32 bits there (`strtol` is `strtoll`, `ftell` is `_ftelli64`) and `wchar_t` 16 (the wide functions are
  the bridge's, in UTF-8 as bionic's). The CRT's `free` takes only `malloc`'s blocks, so a block aligned past 16 bytes
  is kept in a table. `mmap` is `VirtualAlloc` and file views. `select`, `socketpair`, `readlink`, `fchmod` and
  `alarm` report ENOSYS or EOPNOTSUPP, and a socket is a Winsock `SOCKET`, which `read`, `write` and `close` tell
  from a CRT descriptor.

FreeBSD's and macOS's branches are written from those systems' manuals and have not been compiled. Windows' files
compile with `clang --target=x86_64-w64-mingw32 -fsyntax-only` and the mingw-w64 headers.

## 32-bit libraries

An APK with no `lib/arm64-v8a` runs its `lib/armeabi-v7a` or `lib/armeabi` libraries: the install takes the best ABI
the APK has, as a 64-bit phone does, into the app's `lib/arm`. These are ILP32 ARM code for bionic, so a guest pointer
is 32 bits and cannot be a host pointer. The 32-bit bridge (`arm32.c`, `arm32_libc.c`, `arm32_jni.c`) is a separate
one beside the 64-bit bridge:

- **Memory.** The guest has a window of 4 GiB of its own: guest address `a` is host address `arm32_mem + a`, its
  first 64 KiB not mapped. The libraries, the heap (bionic's `malloc` and `mmap`, the bridge's own allocator, with
  `MAP_FIXED` and `mremap`) and each thread's stack are in it. A file that the guest maps is mapped there. A direct
  `ByteBuffer` that the app's Java code allocates is in the window too: the translator sends `allocateDirect` to
  `snx.dalvik.Runtime`, so the native code reaches it, as an audio engine writes its samples into one.
- **Code.** The A32 and Thumb block compiler (`common/cpu/arm/exec.h`, the one the GNU ABI runtime's 32-bit programs
  use) compiles each block when it is first reached. A table for each library's code keeps each address's block, so a
  thread finds a compiled block without a lock. Code outside the libraries (a JIT's, as Mono's) compiles from where it
  is, kept by its page, and the `cacheflush` system call drops the blocks of the range it names.
- **Calls.** An import is an address at the top of the window, 8 bytes apart. The dispatch loop gives a call there to
  its host handler, which reads the arguments as AAPCS lays them out (r0 to r3, then the stack, a 64-bit one at an even
  word; a float or a double in core registers, as Android's softfp ABI passes them) and returns to `lr`. Linux's kernel
  helpers at `0xffff0fa0` to `0xffff0fe0` (`__kuser_cmpxchg` and the others, which ARMv5 code calls for its atomics) are
  the bridge's too, and an `svc` is a system call of the bridge's (`arm32_syscall`: `cacheflush`, `futex`, `gettid`,
  `tkill`, `clock_gettime`).
- **The loader** maps the `PT_LOAD` segments, applies `R_ARM_RELATIVE`, `ABS32`, `GLOB_DAT` and `JUMP_SLOT`, finds the
  symbols by the SysV or the GNU hash (an NDK r28 library for API level 26 has the GNU one alone), and runs `DT_INIT`
  and `DT_INIT_ARRAY`, then `JNI_OnLoad`. `__gnu_Unwind_Find_exidx` gives a library's `PT_ARM_EXIDX`, for C++
  exceptions. A `dlopen` handle names its library, and `dlsym` searches it first.
- **The system's libraries.** `arm32_libc.c` and `arm32_posix.c` have bionic's functions whose arguments have ILP32's
  layouts (`struct stat`, `dirent`, `tm`, `FILE`, `z_stream`, `addrinfo`, the `ctype` tables, the environment), the
  guest's `printf` and `scanf`, libm, liblog, libdl, zlib and SHA-1, and the `_FORTIFY_SOURCE` forms that the NDK
  turns on (`__memcpy_chk` and the other `__*_chk`, `__open_2`), with bionic's checks: one that fails ends the app with
  bionic's message and the sizes. The ones whose arguments are integers and bytes
  (files, descriptors, sockets, the process) are the 64-bit bridge's handlers, called through `arm32_shim.c` with each
  argument put in an AArch64 CPU's registers as AAPCS64 passes them, so `/proc`, errno and the Network setting are as
  for a 64-bit app; the ones the 64-bit bridge has no handler for (`openat`, `pwrite`, `pread64`, `pipe2`, `eventfd`
  and the others) are the host's in `arm32_posix.c`. EGL, GLES 1.1 and 2.0 and `ANativeWindow` are the 64-bit bridge's the same way. Their host objects
  are 32-bit handles, and the calls with pointers in memory (`glShaderSource`'s strings, `eglChooseConfig`'s configs)
  and a client array's pointer, which is an offset while a buffer is bound, are converted on their own.
- **Threads and signals** (`arm32_thread.c`). A guest thread is a host thread with its own CPU state; `pthread_t` is
  its TLS block's address. Mutexes, conditions, semaphores, onces and the C++ guards of static objects are host
  objects found by the guest's address. Every wait waits in slices of 10 ms and runs the signals sent to the thread
  between them, as Linux interrupts a wait: a garbage collector stops each thread with one. A handler runs between two
  blocks, or in a wait, in a Linux frame of the guest's stack (`siginfo_t` and a `ucontext_t` whose `uc_mcontext` has
  the registers, which the handler may change). `setjmp` and `longjmp` are guest code, as they move the guest's
  registers and stack; `sigsetjmp` keeps the mask.
- **JNI.** The guest's `JNIEnv` and `JavaVM` are tables of 32-bit import addresses in the window. A host reference or
  ID is 64 bits, so the guest gets a 32-bit handle for each: a local reference is an index into the thread's list,
  which the native method's return or `PopLocalFrame` shortens, a global or a weak one an index into a table, and a
  method or a field an index into the table of IDs, with the method's signature for the calls with variable arguments.
  A string's or an array's elements are a copy in the window, written back at its release. `Class.forName` becomes
  `snx.dalvik.Runtime.forName`, as for the 64-bit bridge.
- **NativeActivity.** Its `ANativeActivity_onCreate` (or the function of `android.app.func_name`) runs with an
  `ANativeActivity` in the window, and the callbacks it sets run at each step of the activity's life. The activity
  takes its window's input queue: the system's touches and keys go to a native `AInputQueue`, which the looper it is
  attached to wakes for, and which `AInputQueue_getEvent` and the `AInputEvent` getters read.

The 32-bit bridge's libc is the set that apps have needed so far, and an import it lacks says so once and returns 0.
It has no OpenSL ES or Vulkan yet, and a host fault in generated code (a JIT's null check) ends the app. Windows has
none of the POSIX layouts it takes bionic's from, so the build leaves the 32-bit bridge out there (`arm32_none.c`) and
a 32-bit library's load fails with its path. Two apps run:
an armeabi game engine of 6 MB (GLES 1.1 through `GLSurfaceView`), its menus, its game and its touches, and a Unity
4.7 game for armeabi-v7a (Mono's JIT, GLES 2 through native EGL, input through NativeActivity's queue), its menus and
a level.

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
  that `GetMethodID` recorded. `FindClass` falls back to the app's class loader within the app's native methods and
  the loads of its libraries, as Android's uses the loader of the class whose native method runs; on a thread with
  none (one the app attached) it has the system class loader alone, and an app's class is a ClassNotFoundException,
  as on ART. `GetVersion` is Android's JNI 1.6, not the host JDK's. The string functions' modified UTF-8 is ART's:
  a supplementary character is UTF-8's 4 bytes both ways, where HotSpot's CESU-8 has 6 (and its `NewStringUTF` ends
  the string at a 4-byte sequence). `RegisterNatives` records the guest's functions.
- The translator gives each `native` method a body that calls `NativeBridge.invoke*` with its key, the object and its
  parameters (primitives as bits in a `long[]`, references in an `Object[]`). The bridge finds the function by
  `RegisterNatives` or by its JNI name, and passes the parameters as AAPCS64 does.

All 24 libraries of the Unity game load and run their constructors. From Java, `System.loadLibrary` loads them and
runs their `JNI_OnLoad`: the game's crash reporter initializes its native side. An import without an HLE yet is an export that
says so once when called and returns 0.

The recompiler's `android` target (`a64.profile`) is a phone's Armv8.2 CPU without pointer authentication, and
`getauxval(AT_HWCAP)` reports the same features.

- libandroid: `ANativeWindow` is the `SurfaceView`'s surface, which is the host window at the view's size. The looper
  is an epoll set, the sensor manager has no sensors, and `AAssetManager` reads the assets of the APK and its splits.
  A stored asset opens as its APK's file descriptor with the asset's place in it.

- the Vulkan pass-through (`hle_vulkan.c`): each `vk*` command the guest names is the host loader's, called with the
  guest's arguments by `hle_call_wide` (up to 12 integers, the guest's stack slots past `x7`). The instance's
  `VK_KHR_android_surface` becomes SDL's surface extensions or `VK_EXT_headless_surface`, the surface is the SDL
  window's (ScarletNX's renderer gives the window up), the guest's allocation callbacks and debug messengers are left
  out, and the device reports itself a **Mali-G710** with ARM's vendor and driver IDs, its limits and features the
  host's. A present blends the windows of the app's Java side over the image when one shows (HWUI's layer). A surface's current extent is the native window's size, as an Android surface's always is (a Wayland or
  headless surface leaves it to the swapchain). A Unity game's engine reaches it, makes its swapchain on the window
  and presents its first frames (lavapipe here),
- EGL gives the GPU's identity alone: a display, contexts without a config or a surface, and `glGetString` with the
  device that Vulkan reports, which engines read before they choose an API. Nothing draws through GLES, on purpose.

`--sample` also prints the count of the guest's Vulkan presents, and asks each thread in guest code
for its pc at its next poll, so a loop's pc is its own and not that of its last call.

Next: OpenSL ES and the media codecs.
