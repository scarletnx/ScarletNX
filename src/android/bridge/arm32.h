/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_ARM32_H
#define SNX_ANDROID_ARM32_H
#include "common/cpu/arm/arm.h"
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The 32-bit native bridge (docs/android/NATIVE.md, "32-bit libraries"): an APK's lib/armeabi-v7a or lib/armeabi
   libraries, ILP32 ARM code for bionic. Their pointers are 32 bits, so the guest has a window of 4 GiB of its own:
   guest address a is host address arm32_mem + a. The A32 and Thumb block compiler (common/cpu/arm/exec.h) runs the
   code. Imports are addresses at the top of the window, which the dispatch loop gives to host handlers, as the 64-bit
   bridge's exports are */

/* the guest's window: 4 GiB, its first 64 KiB not mapped */
extern uint8_t* arm32_mem;
static inline void* arm32_host(uint32_t address) { return address ? arm32_mem + address : NULL; }
static inline uint32_t arm32_guest(const void* host) {
    return host ? (uint32_t)((const uint8_t*)host - arm32_mem) : 0;
}

/* the window and the block compiler made, once. False with the reason in error */
bool arm32_init(char* error, size_t error_size);
/* whether arm32_init ran: the app's libraries are 32-bit ones */
bool arm32_active(void);

/* A host function that guest code calls: it reads its arguments from c (r0 to r3, then the stack) and writes its
   result into r0 (and r1); the call returns to lr. The same name gives the same address */
typedef void (*Arm32Handler)(SnxArmCPU* c, void* data);
uint32_t arm32_export(const char* name, Arm32Handler handler, void* data);
/* the calling thread's CPU state, made with its guest stack and TLS the first time */
SnxArmCPU* arm32_cpu(void);
/* the guest function fn (bit 0 set for Thumb) called on this thread with count argument words, laid out as AAPCS
   lays out its arguments (the caller aligns 64-bit ones). r0, and r1 into *high when it is not NULL */
uint32_t arm32_call(uint32_t fn, const uint32_t* words, unsigned count, uint32_t* high);
/* the calling thread's errno and TLS slots, in guest memory */
uint32_t arm32_errno_address(void);

/* a thread's state: its CPU made with a stack of stack_size bytes the first time (arm32_cpu's is 8 MiB), its pthread_t
   (its TLS block's address), its id (gettid's), its stack, and its state freed as the thread ends */
SnxArmCPU* arm32_cpu_with_stack(uint32_t stack_size);
uint32_t arm32_thread_self(void);
uint32_t arm32_thread_id(void);
void arm32_thread_stack(uint32_t* base, uint32_t* size);
void arm32_thread_end(void);
/* the signals sent to the calling thread and not run yet, a bit for each (arm32_thread.c): the dispatch loop runs
   them between blocks with arm32_signals_run, which sets up each handler's frame from c */
extern _Thread_local uint32_t arm32_pending_signals;
void arm32_signals_run(SnxArmCPU* c);
/* a system call that guest code makes (svc): number r7, arguments r0 to r5, the result or -errno (arm32_thread.c) */
uint32_t arm32_syscall(uint32_t number, const uint32_t args[6]);
/* the code at [lo, hi) changed: its blocks outside the libraries are compiled again when next reached */
void arm32_code_changed(uint32_t lo, uint32_t hi);
/* A32 code of the bridge's own, written into the window and compiled when reached: its address */
uint32_t arm32_guest_code(const uint32_t* words, unsigned count);

/* the guest's heap: bionic's malloc, in the window. 0 when it is full */
uint32_t arm32_malloc(uint32_t size);
uint32_t arm32_memalign(uint32_t align, uint32_t size);
uint32_t arm32_realloc(uint32_t address, uint32_t size);
void arm32_free(uint32_t address);
/* whole pages of the window, zeroed (mmap's): 0 when it is full */
uint32_t arm32_map(uint32_t size);
void arm32_unmap(uint32_t address, uint32_t size);
/* the pages at [address, address + size) the guest's, zeroed (mmap's MAP_FIXED): false outside the heap */
bool arm32_map_at(uint32_t address, uint32_t size);
/* the guest's copy of a host string (arm32_malloc's) */
uint32_t arm32_strdup(const char* text);

/* the loader (bionic's linker, for ELF32 ARM): a library loaded with its DT_NEEDED and constructors run */
typedef struct Arm32Library Arm32Library;
Arm32Library* arm32_load(const char* path, char* error, size_t error_size);
/* a symbol of a library, or of every loaded one in load order (library NULL); 0 when none has it */
uint32_t arm32_symbol(const Arm32Library* library, const char* name);
const Arm32Library* arm32_owner(uint32_t address);
const char* arm32_library_name(const Arm32Library* library);
/* __gnu_Unwind_Find_exidx: the .ARM.exidx of the library that holds pc, and its count of entries */
uint32_t arm32_exidx(uint32_t pc, int* count);
/* the library folder that DT_NEEDED and dlopen look in */
void arm32_set_library_dir(const char* dir);

/* a function or an object that the system's libraries export: a handler with its data, or an object's guest
   address in data (handler NULL). Each file's table ends with an empty name */
typedef struct {
    const char* name;
    Arm32Handler handler;
    void* data;
} Arm32Symbol;
extern const Arm32Symbol arm32_posix_symbols[];  /* arm32_posix.c: files, processes, time, strings, sockets, zlib */
extern const Arm32Symbol arm32_thread_symbols[]; /* arm32_thread.c: threads, waits, semaphores, signals, setjmp */
extern const Arm32Symbol arm32_shim_symbols[];   /* arm32_shim.c: EGL, GLES and ANativeWindow, the 64-bit bridge's */
/* the objects each file exports, by name: their guest address, 0 for none */
uint32_t arm32_posix_object(const char* name);
void arm32_posix_init(void);
void arm32_thread_init(void);
void arm32_shim_init(void);
/* a 64-bit bridge's handler called by its name (hle_find's) with x0 to x7 (and the stack after them): its x0, its
   errno made the guest's; *found false when the HLE lacks it */
uint64_t arm32_call64(const char* name, const uint64_t* x, unsigned count, bool* found);
/* the guest's errno from the host's errno, or from a Linux number */
void arm32_set_errno(int host_error);
void arm32_set_errno_linux(int linux_error);
/* the n'th word of a call's arguments (r0 to r3, then the stack) */
uint32_t arm32_word(const SnxArmCPU* c, unsigned n);
/* the host FILE of a guest FILE*, and a guest FILE* for a host one (fdopen's) */
void* arm32_host_file(uint32_t file);
uint32_t arm32_guest_file(void* file);
/* the host object of a guest JNI reference, and the calling thread's host JNIEnv (arm32_jni.c) */
jobject arm32_jni_ref(uint32_t handle);
JNIEnv* arm32_jni_env(void);
/* a thread that the guest made ends: its host JNIEnv detached when the bridge attached it */
void arm32_jni_thread_end(void);

/* The system's libraries (arm32_libc.c): the guest address of a symbol of bionic, libm, liblog, libdl or GLES 1.1,
   0 for one the HLE lacks */
uint32_t arm32_hle_symbol(const char* name);
bool arm32_hle_is_system_library(const char* name);
void arm32_hle_init(void);
/* an import the HLE lacks: an export that reports it when called and returns 0 */
uint32_t arm32_hle_missing(const char* name);

/* A call's arguments in AAPCS's order: words in r0 to r3, then on the stack, a 64-bit one at an even word. A va_list
   is a pointer to the words in memory */
typedef struct {
    const SnxArmCPU* c;
    uint32_t word; /* the next word, of the registers and the stack */
    bool va;
    uint32_t pointer; /* a va_list's next word */
} Arm32Args;
void arm32_args(Arm32Args* a, const SnxArmCPU* c, unsigned first_word);
void arm32_args_va(Arm32Args* a, uint32_t va_list);
uint32_t arm32_arg(Arm32Args* a);
uint64_t arm32_arg64(Arm32Args* a);
/* a printf format of the guest with its arguments, as vsnprintf: the length it needs */
int arm32_format(char* out, size_t size, const char* format, Arm32Args* a);

/* JNI (arm32_jni.c): the guest's JavaVM and a library loaded with its JNI_OnLoad (NULL), else the error */
char* arm32_jni_load(JNIEnv* env, const char* path, const char* library_dir);
/* a native method of the app called, by its key "Lpkg/Class;.name(signature)": the result as bits, the result's
   type in *result. *found false when no 32-bit library has it */
uint64_t arm32_jni_invoke(JNIEnv* env, const char* key, jobject self, jlongArray prims, jobjectArray refs,
                          char* result, bool* found);
/* NativeActivity's input queue (arm32_shim.c): a new AInputQueue's guest address, and the events sent to it. A motion's
   fields: device, source, action, flags, meta state, edge flags, button state; then count pointers' ids, tool types
   and 9 axes each (x, y, pressure, size, touch major and minor, tool major and minor, orientation). A key's: device,
   source, action, flags, key code, scan code, meta state, repeat count. Times in nanoseconds */
uint32_t arm32_input_queue_new(void);
void arm32_input_motion(uint32_t queue, const int32_t* fields, int64_t down, int64_t time, float xprec, float yprec,
                        unsigned count, const int32_t* ids, const int32_t* tools, const float* axes);
void arm32_input_key(uint32_t queue, const int32_t* fields, int64_t down, int64_t time);
/* the guest's JavaVM, for the HLE */
uint32_t arm32_jni_vm(void);
#endif
