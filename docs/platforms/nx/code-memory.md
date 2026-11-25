<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Code memory of the process

Three SVCs let a program publish its own memory as code. A JIT writes instructions into ordinary heap memory,
makes them executable through an alias, runs them, and unmaps the alias before it writes the next block.

| SVC | What it does |
| --- | --- |
| `svcMapProcessCodeMemory` (0x77) | Maps the process's own normal memory as an AliasCode view |
| `svcSetProcessMemoryPermission` (0x73) | Sets none, read, read-write or read-execute on code |
| `svcUnmapProcessCodeMemory` (0x78) | Unmaps the view and gives the source its read-write access back |

The behavior follows Atmosphère's `KPageTableBase::MapCodeMemory`, `SetProcessMemoryPermission` and
`UnmapCodeMemory` in `libraries/libmesosphere/source/kern_k_page_table_base.cpp`, which is the reference for the
console's kernel. It was read for its behavior, and no code was ported (Atmosphère is GPL-2.0-only). The code is in
`src/nx/hle/svc/svc_mem.c`.

## The mapping

`svcMapProcessCodeMemory(handle, dst, src, size)` takes a source that is normal memory, read-write, with no
attribute. On the console the source becomes kernel-read and unmapped to the program, and takes the Locked
attribute. The same pages appear at the destination as AliasCode with no permission. The program then calls
`svcSetProcessMemoryPermission(dst, size, ReadExecute)` and runs the code.

The source is not readable by the program while the view exists. It keeps its pages, and the program writes them
again after the unmap. `svcUnmapProcessCodeMemory` unmaps the view, restores the source to read-write and clears the
Locked attribute. To change the code, the program maps, runs, unmaps, writes, and maps again.

The console maps exactly the range it is given. It needs no free page around the view. `ldr:ro`'s code view
(`mem_map_code`) does require a free guard page on each side, because a module is placed where its AOT code expects
it. The three SVCs use the same alias pool as the other views, so a view and its source are one set of pages, and the
view costs no memory. On a host without the pool (Windows) the view is a copy, and the unmap writes it back into the
source.

## The permissions

`svcSetProcessMemoryPermission` takes a range whose blocks carry the `FlagCode` state flag and no attribute. It
accepts None, Read, ReadWrite and ReadExecute. Execute alone and Write alone are InvalidNewMemoryPermission, as on the
console. A read-write code block becomes CodeData (Code) or AliasCodeData (AliasCode). Those states do not carry
`FlagCode`, so the call cannot set execute on them again. The program unmaps and maps again instead.

Setting a read-execute permission tells the runtime's JIT that the range changed, so the runtime compiles the code
again. The same happens at the unmap, for the view and its source.

## Ranges and attributes

`svcUnmapProcessCodeMemory` takes a range, and the range can be part of a view. The console unmaps what it is
given, so a program can unmap the parts of a view in any order. The runtime keeps each view's parts, and the
source's lock stays only where a part is left.

A view whose attribute changes keeps its source's lock. `svcSetMemoryAttribute` can set Uncached on code data, and
the console's page table still holds the source Locked. The lock goes when the view does, not when its attribute
changes. An unmap takes only a view whose attribute is nothing but PermissionLocked, as the console does, so the
program clears Uncached first.

## Invalid requests

The calls return the console's results. A wrong alignment is InvalidAddress. A zero or partial page count is
InvalidSize. A range that wraps is InvalidCurrentMemory. A handle that is not a process is InvalidHandle. A destination
outside the address space, or over the heap, the alias region or the kernel's pages, is InvalidCurrentMemory, the SVC's
own check before the page table's. A source that is not normal read-write memory, a destination that is not free, and a
range that is not code are InvalidCurrentMemory too. A wrong permission is InvalidNewMemoryPermission. An unmap whose
source is not the view's own source, for the range given, is InvalidMemoryRegion, as the console's page group comparison
gives.

## Limits

The runtime keeps the live views in one table, `KMAPVIEW_MAX` entries shared with `svcMapMemory`. Past that number
`svcMapProcessCodeMemory` answers OutOfResource instead of making a view it could not give back. No game is known to
keep more than a few views at once.

The runtime serves one process, so the handle names that process. SVCs 0x73, 0x77 and 0x78 need no game recompile.
They are runtime code only.

## Validation

`tests/memory_alias.c` links the real `mem.c` and `svc_mem.c` and runs `test_process_code` on a host with the alias
pool and `test_process_code_copies` without it. The checks cover the console's argument results, the states and
permissions, a write through a read-write view that the source keeps, the unmap of a wrong source, the unmap, write,
map again sequence, the JIT's invalidation, and a view between two mapped pages. `tests/svcgen_layout.cmake` holds
the SVCs' registers in both ABIs.
