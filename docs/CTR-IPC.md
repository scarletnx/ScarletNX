<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# CTR IPC and minimal startup services (3a/3b)

## Index

* [Wire and ownership](#wire-and-ownership)
* [Generator subset](#generator-subset)
* [Implemented services](#implemented-services)
* [Tests and reference provenance](#tests-and-reference-provenance)

This is **not** Switch HIPC/CMIF. It reuses the lexer/AST/output utilities of
`tools/swipc/swipc.c`, with a separate `--ctr` validator/emitter in `ctr.inc`.
The existing default emitter/output remains unchanged. See [NX IPC](IPC.md)
for that separate profile, and [3DS status](3DS.md) for outstanding work.

## Wire and ownership

- A command occupies 64 little-endian words at the active thread's TLS + `0x80`.
  Header bits 31:16 are the command ID, 11:6 normal-word count, 5:0 translated-word
  count; bits 15:12 must be zero. Header plus both regions must fit 64 words.
- Each native guest thread has its own zeroed, `0x200`-aligned TLS slot.
  Full RAM read/write permissions are checked **before** invoking handlers or
  allocating output handles. MMIO is never used for command-buffer transfers.
  Serialization makes the write preflight stable through response publication.
- Translation (Azahar's `hle_ipc.cpp`) takes any well-formed descriptor and
  classifies buffers before handles: copied (`0x00`) and moved (`0x10`) handles,
  up to 8 in all, the calling PID (`0x20`, the kernel writes the real PID over the
  client's word), static buffers (`2`) the service reads, PXI buffers (`6` the
  service reads, `4` it also writes, as libctru's `IPC_Desc_PXIBuffer`), and
  mapped buffers (`0xA` read, `0xC` write, `0xE` both). Every buffer is checked
  against the caller's memory for the access it grants before any handler runs,
  and handlers still check the exact form each command takes. A request with more
  than 8 handles stops the run, since the runtime does not translate it. Switch
  descriptors are never used.
- The response repeats the command ID; its first normal word is the service
  result. Transport SVC result and service result are distinct. Failed service
  calls publish a result-only response. Malformed requests return a transport
  error without changing the command buffer or invoking the handler. Moved handles
  leave the caller's table once a handler has taken the request, even when it
  answers with a transport error. Unknown commands answer success with one normal
  word, as Azahar's ReportUnimplementedFunction does and a Switch game's do, and
  each is logged the first time it is answered. With `--ctr_auto_stub_ipc=0` they
  stop the run, unless `--ipc-stubs` names their service (`all`, or `srv`, `fs`,
  `apt`, `ptm`, `hid`, `gsp`, `cfg`, `dsp`, `system`, `am`, where `fs` covers file
  sessions too).
- Sessions are real typed kernel objects. Duplicates share state and retain a
  reference; final close frees the object. Output handles are newly owned client
  references, published with the declared copy/move descriptor and relinquished
  on handler failure. Contiguous output handles of the same kind share one
  descriptor, including APT's two copied events and HID's six copied handles. HLE has no separate server address space/handle table yet.
- Bounds are 256 handles/256 object slots including roots; endpoint caps are
  64 `srv:` and 32 FS sessions. These are bounded implementation quotas, not
  complete hardware resource-limit/ExHeader capability enforcement. APT/PTM/HID
  session caps are 2 each. Threads and the other kernel objects follow the
  APPLICATION resource limit (32 threads including the initial one).
  Read-only FSFILE objects have a separate 64-object cap.

## Generator subset

Use `snx_swipc(target CTR ROOT ... FILES ...)` in CMake or:

```sh
swipc --ctr --root src/3ds --out GENERATED services/srv.swipc services/fs.swipc
```

The output directory must exist. Schemas live beside service C files. Include
`services/name.swipc.inc` only in the C file defining its static typed handlers;
other translation units include the generated `.h`.

- `u32`, `u64`, word-sized `bytes<N>`, one `pid` input, and copy/move handle
  outputs. Raw words follow **declaration order**, with no NX alignment sorting
  or 64-bit alignment padding. Named aliases resolve to the same concrete types.
- Handler signature: `uint32_t interface_Command(SnxCtrIpc*, arguments...)`.
  A PID parameter is `uint32_t`; scalar outputs are pointers, byte arrays use
  byte pointers, and the return value is the service result. Non-return outcomes
  are explicit context states, not successful responses.
- Command IDs are 16 bits. Request and result/output layouts must fit the full
  64-word envelope; unsupported wire types and `@stub`, `@unsorted`, `@fallback`
  are refused. Unspecified commands stop; there is no default successful stub.
- The host generator and dedicated hand-encoded generator fixture compile as
  strict C99. The CTR runtime target remains C11.

## Implemented services

- ConnectToPort (`0x2d`) recognizes the actual named `srv:` endpoint. Invalid
  pointers/names and connection/resource limits return errors, not fake handles.
- SendSyncRequest (`0x32`) invokes checked generated dispatch on a session.
  GetProcessId (`0x35`) identifies the real single application's process root;
  PID `0x10` is local to that independently owned board/process instance.
- `srv:` RegisterClient (`1`) associates the caller process with that session.
  GetServiceHandle (`5`) returns real FS, APT, PTM, HID, GSP, cfg and DSP sessions.
  An absent endpoint with the non-wait flag returns ServiceNotRegistered. A waiting
  request blocks its actual thread without a response or session, and the runtime
  prints the service's name, since no other process can register it. Peers can
  execute. Dynamic service installation is unsupported.
- `fs:USER` Initialize (`0x801`) and InitializeWithSdkVersion (`0x861`) record
  caller/SDK association. OpenArchive (`0x80c`) accepts an empty path and a
  checked size-one static descriptor. Archive 9 (SDMC) binds a read-write host
  folder for the title, where CreateFile, DeleteFile, CreateDirectory,
  DeleteDirectory, Rename and OpenDirectory work on UTF-16 paths whose static
  buffer descriptor (buffer 0, the path's size) is checked. Archive 3 binds the real process RomFS: 32
  live typed FS-private tokens, monotonically issued 64-bit IDs, never reused
  and never kernel handles. CloseArchive (`0x80e`) retires that binding;
  invalid/stale tokens return `0xc8804465`. Existing files outlive archive close.
  OpenArchive also opens another title's save data, as Azahar's OtherSaveData
  archives do: `0x567890b2` (General) and `0x567890b4` (Permitted), with a 12-byte
  binary path. Its media: SDMC is served, a game card answers GamecardNotInserted,
  others UnsupportedOpenFlags. Permitted's program ID is high `0x00040000` and low
  the unique ID shifted left by 8 in 32 bits; General's are the path's words as
  given. The save data is the SD card's `title/<high>/<low>/data/00000001` folder,
  and NotFormatted when that is missing. This title's own ID opens its own save
  data, as archive 4 does. GetFormatInfo reads `data/00000001.metadata` beside
  the folder.
  OpenFileDirectly (`0x803`) supports only archive 3 (SelfNCCH), empty archive
  path and the 12-byte all-zero binary RomFS selector, mode read-only and no
  attributes. It returns a typed, reference-counted file object, not a fabricated
  archive or mount. OpenFile (`0x802`) accepts a live archive token and the same
  read-only binary selector. Missing RomFS returns its genuine NotFound error.
  Token exhaustion and file/handle publication failures leave resources unchanged.
  ControlArchive (`0x80d`) GET_TIMESTAMP returns the backing host file's real
  modification time, COMMIT_SAVE_DATA on SDMC succeeds since writes have already
  reached the host, and other actions stop.
- FSFILE GetSize (`0x804`), Close (`0x808`) and Read (`0x802`) expose a CTR
  level-3 view reconstructed over extracted `romfs/`. `common/filesystem/`
  supplies the same indexed nodes, immutable shared mappings and fallback
  opens as NX; CTR metadata and wire framing are separate. No CIA/container is
  reopened. Reads are capped at 16 MiB/request, clamp at EOF, preflight the
  whole requested writable RAM span and stage all host IO before guest writes.
  Malformed descriptors, MMIO and permissions cannot publish partial reads;
  host IO failure stops without a reply or guest writes. Duplicates share close
  state. On host files (the SD card, save data, ExtSaveData and another title's
  save data) Write and SetSize reach the host file, and an ExtSaveData's files
  keep their size. Archives the runtime does not serve stop explicitly (parity
  roadmap F3).
- `APT:S`: GetLockHandle (`1`), Initialize (`2`), Enable (`3`), NotifyToWait
  (`0x43`), ReceiveParameter (`0xd`), GlanceParameter (`0xe`), AppletUtility
  (`0x4b`), IsNew3DS (`0x102`).
  One application slot (`0x300`, attributes 0), persistent recursive mutex,
  one-shot notification/parameter events and a real initial pending Wakeup.
  Unsupported applet attributes/identities/transitions stop explicitly. Duplicate
  initialization returns AlreadyExists and does not replace the slot.
- APT utility supports only operation 7 (release the initial startup transition,
  input `0x10`) and operation 4 (sleep-if-shell-closed, input 0, open shell only).
  A closed-shell sleep request is unsupported. The startup Wakeup is answered from
  Initialize on, by GlanceParameter and ReceiveParameter. ReceiveParameter returns
  an empty Wakeup with a **null object**, not a fabricated handle or payload. It
  consumes the parameter only after full command, descriptor and destination
  preflight. The checked TLS static table at +`0x180` must declare the expected
  buffer ID/capacity; MMIO is refused.
- `ptm:sysm`: model query (`0x40a`) and ConfigureNew3DSCPU (`0x818`) for the
  existing base configuration **0 only**. Boost/L2/other configurations stop.
- `hid:USER`: GetIPCHandles (`0xa`) returns actual system-pool shared memory
  and five one-shot events. MapMemoryBlock (`0x1f`) supports only the fixed HID
  read-only page at `0x10000000` (permissions 1/`0x10000000`). No periodic input
  updates/events or writable/general shared-memory mappings are implemented.

Serialized kernel support includes CreateThread (`8`), ExitThread (`9`),
SleepThread (`0xa`), Get/SetThreadPriority with effective (inherited)
priorities, ReleaseMutex (`0x14`), Create/Signal/ClearEvent (`0x17`/`0x18`/`0x19`),
timers (`0x1a`–`0x1d`), WaitSynchronization1/N (`0x24`/`0x25`) with finite
timeouts, mutex recursion, waiter references and wakeups by effective priority,
then by how long the thread waited. Pending thread CPU/TLS/wait state is preserved
across native dispatch. The dispatcher polls the scheduler every 4096 blocks, so a
thread a deadline wakes with a better priority preempts one that makes no SVC.
Multicore timing is not modeled and no host-thread safety or hardware-complete
scheduler claim is made. With no runnable peers and no deadline a real wait stays
BLOCKED, never a fabricated successful return.

GetSystemTick (`0x28`) uses a process-owned host monotonic origin and the
268111856 Hz base domain on both models. A read-only shared page at `0x1ff81000`
holds the host's local date and time (milliseconds since 1900) and its tick
reference, rewritten every hour into the entry its counter does not select, as
Azahar does. This is host-paced time, not cycle accuracy.

SuperTux **reaches and executes main**, opens its real reconstructed RomFS,
executes scalar VFP in `gettimeofday`, answers its archive timestamp request,
and goes on to draw its title screen and play its music.
See [the exact witness](3DS-SUPERTUX.md).

## Tests and reference provenance

`3ds_ipc` executes real sessions/handlers against checked memory on both models,
including forged PID replacement, PXI buffer directions, the handle limit, stub
lists and moved handles on success and on refusal, malformed counts/descriptors, read-only,
unmapped/misaligned/MMIO TLS, stale/duplicate handles, endpoint/handle exhaustion,
rollback and non-resumable waits. `ctr_swipc_dispatch` is a generator-only C99
fixture with deliberately test-local resource accounting; it checks declaration
order, 64-bit/byte/PID layout, reply packing and failed-output rollback.
`ctr_swipc_validation` rejects unsupported schemas, checks full-envelope bounds
and pins unchanged legacy default output. Synthetic CIA pipelines execute real
SVC/TLS request and blocked-wait paths through offline native AOT.

The optional supplied-title test preserves both earlier CPU reference boundaries
and a third before APT:S lookup handling, then executes the genuine main path. The post-HLE comparisons script selected HLE
and IPC effects: they validate CPU behavior, **not independently hardware-correct
kernel/service semantics**. The runtime executes actual C handlers, never that
script or a CPU interpreter. `3ds_services` tests both models, APT state/buffer
preflight, real resources, quotas/rollback, and PTM/HID/FS boundaries.
`3ds_threads` tests context/TLS preservation, waiter ownership, event/mutex/
arbiter wakeups, wait-all and no-peer blocking. A native synthetic CIA requires
its worker to write shared RAM and signal main before main can exit successfully.
A separate native CIA resolves and verifies a real extracted file through FSFILE.
`3ds_filesystem` independently decodes CTR metadata/UTF-16/hash chains, compares
contents, forces on-demand IO and checks failed-read atomicity, unavailable
resources, EOF/permissions/MMIO, handle/object caps and duplicate/close ownership.
The unchanged NX tree/service/raw-image fixtures also run in CTR-only builds.

Reference facts: Azahar `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e` (and
`987f18405073960622e1043bb44005989135c6e1` for F1's translation, stubs and GSP),
`core/hle/ipc.h`, `kernel/hle_ipc.cpp`, `kernel/svc.cpp`,
`service/sm/{srv.cpp,sm.h}`, `service/fs/fs_user.cpp`, `service/apt/`,
`service/hid/`, `kernel/{mutex,event,thread}.cpp` (GPL-2.0-or-later), and libctru's
APT startup protocol (see its own license). Service policy is a bounded HLE
personality, not an independently hardware-validated full implementation.
New C transport/services and generator extension are independent project code
under GPL-3.0-or-later, retaining the shared SwIPC tool's provenance. No Azahar
runtime/build dependency or title assets are included.
