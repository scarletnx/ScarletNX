<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Application memory accounting

## Index

* [Implemented behavior](#implemented-behavior)
* [Remaining HLE differences](#remaining-hle-differences)
* [References](#references)
* [Validation](#validation)

This change targets the heap rejection seen during Super Mario Odyssey startup. A
3200 MiB request was refused because the runtime charged a fixed 256 MiB in
addition to the requested heap against a 3285 MiB application budget. The game
subsequently initialized an allocator with a bad buffer. Increasing `memory_gb`
avoided that path, but did not correct the accounting.

A 32-bit title then hit a second startup failure. It read the total from GetInfo
6 (3285 MiB), subtracted a small reserve, and asked SetHeapSize for 3260 MiB.
That is larger than a 32-bit process's heap region, so the kernel refused it and
the SDK's heap manager stayed uninitialized. Mesosphere caps GetInfo's total at
the process's heap region (`m_max_process_memory`), not at the application pool:
2 GiB for a 32-bit process without an alias region, 1 GiB with one. GetInfo 6 and
21 now do the same, and the title asks for 2024 MiB, which fits.

The memory behavior below was checked against Atmosphere's Mesosphere sources at
commit `c7e6c35e900b52c364d557bdbfe554c857158d97`. This is an independently written
implementation in ScarletNX, not a port of Atmosphere's GPL-2.0-only code.

## Implemented behavior

- Charge the page-rounded loaded images, including data and BSS, and the main
  thread's page-rounded stack instead of a fixed 256 MiB. ScarletNX's unmapped
  guard pages are not physical allocations. Images are charged by the loader and
  the main stack is charged before the guest main thread starts.
- For a single application with a private NPDM system resource, charge that
  reservation once. GetInfo 6/7 include it and 21/22 exclude it. This reservation
  is not the same quantity as the slab usage returned by GetInfo 17.
- Use application pool sizes of 3285, 4916 and 6964 MiB for the 4, 6 and 8 GiB
  layouts. The latter two are 417 MiB smaller than the previous extended settings.
- With `memory_fit` at `0` (Auto, the default) or `1` (On), the pool the game is told is at most what
  the host can still give the process as the game starts, plus what the process
  holds, less what the rest takes (the runtime then and 256 MiB for the graphics,
  700 MiB at least), never under 1 GiB (`mem_fit_budget`). Every GetInfo above and
  the heap's limit follow it. A host that can give the whole pool changes nothing;
  `memory_fit=2` (Off) tells the pool whatever the host can give.
- Validate heap requests in 2 MiB steps and reject sizes at or above 8 GiB with
  InvalidSize. Keep the heap-region/process maximum and optional `heap_max_mb`
  checks separate from resource-limit failures.
- Cap GetInfo 6/21 at the process's heap region, as Mesosphere's
  `m_max_process_memory` does: a 32-bit process sees 2 GiB without an alias
  region and 1 GiB with one, however large the application pool is. A 64-bit
  process sees its layout's heap size, or the pool when that is smaller. GetInfo
  7/22 still report what is actually used.
- Serialize heap and physical mapping transactions with the same memory lock.
  Account successful changes only. Charge heap growth, not the existing heap
  again. Failed growth leaves the old size and mapping intact. Shrinking rejects
  locked or otherwise incompatible pages and releases the removed allocation.
- Require a nonzero private system resource for MapPhysicalMemory and
  UnmapPhysicalMemory. Preserve their argument-validation order and overflow error.
- Charge only newly backed alias pages against the same budget as the heap. A
  repeated or overlapping physical map does not double-charge existing pages.
  When a host commit fails, roll back this call's preceding commits and preserve
  all pre-existing mappings and accounting.
- Reject incompatible physical unmaps before making changes. Release only the
  mapped bytes, tolerate holes, and prevent the counter from underflowing.

These are the single-application cases relevant to this crash. This is **not** a
claim that every memory SVC now reproduces the kernel.

## Remaining HLE differences

- There is no secure-system-resource slab allocator or page-table-page usage
  model. GetInfo 17's pre-existing estimate remains unchanged. It must not be
  described as accurate Atmosphere slab usage.
- Multiple processes sharing resource limits, default application system resources
  on newer firmware, dynamic resource limits, application/applet pool sharing and
  reservation wait timeouts are not modeled. GetInfo totals therefore describe
  this runtime's one-application layout, not all of Atmosphere's configurations.
- GetInfo's existing handle/subtype validation and unrelated memory SVC behavior
  are not changed. MapMemory still uses copied alias views rather than physical
  page aliases. This patch does not make arbitrary cross-SVC misuse of those views
  kernel-accurate.
- Host VM commitment and metadata allocation are not the console's physical page
  allocator. Host out-of-memory behavior can differ from console behavior.
- The public generated-code ABI is unchanged. Rebuild/redeploy the runtime.
  Recompiling the game module is not required by this patch.

## References

All paths below are under `libraries/libmesosphere` in the referenced Atmosphere
commit:

- `source/kern_k_process.cpp`: `GetUsedUserPhysicalMemorySize`,
  `GetTotalUserPhysicalMemorySize`, their non-system variants, and `Run`.
- `include/mesosphere/kern_k_page_table_base.hpp`: `GetNormalMemorySize`.
- `source/kern_k_page_table_base.cpp`: `SetHeapSize`, `MapPhysicalMemory` and
  `UnmapPhysicalMemory`.
- `source/svc/kern_svc_physical_memory.cpp`: request validation and the private
  system-resource prerequisite.
- `source/kern_k_system_resource.cpp` and
  `source/board/nintendo/nx/kern_k_system_control.cpp`: private application
  system-resource reservations and the application-pool sizes.

## Validation

`tests/memory_accounting.c` links the actual `mem.c` and `svc_mem.c`. Its POSIX VM
backend reserves the guest address space sparsely, so the large-heap tests do not
need gigabytes of resident RAM. It injects commit failures and stubs unrelated
thread, handle, clock and GPU services. No game binaries or extracted data are
needed.

The suite checks the 3200 MiB startup request with synthetic 55 MiB images and a
1 MiB main stack, boundary sizes, GetInfo memory values, private system-resource
charges, locked-page shrink rejection, repeated and overlapping physical maps,
partial-failure rollback, incompatible unmaps, and 50 two-thread races for the
last 2 MiB of the shared heap/physical budget. It also switches to both 32-bit
layouts and checks that GetInfo 6/21 name the heap region (2 GiB without an alias
region, 1 GiB with one), that the application pool's total is still refused, and
that a request just inside the region succeeds.

Run it through the normal CMake/CTest configuration on a Unix host, or without the
runtime's third-party dependencies from the repository root:

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror -Iruntime \
  tests/memory_accounting.c src/nx/memory/mem.c src/nx/hle/svc/svc_mem.c \
  -pthread -o /tmp/snx-test-memory
/tmp/snx-test-memory
```

This standalone test was run with GCC 14.2, including ASan/UBSan. The project's
production build still requires Clang. The 32-bit fix was also checked with a
full runtime build and a headless run of the affected title at `memory_gb=0`: the
heap request changed from a refused 3260 MiB to a successful 2024 MiB, and the run
continued past the old crash for a minute of rendering with no failure logged.
Game verification should still include repeated startup at `memory_gb=0` or `4`,
followed by normal gameplay. Passing this suite alone does not prove that every
downstream guest allocator path is fixed.
