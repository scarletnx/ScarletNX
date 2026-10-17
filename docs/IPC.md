<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# IPC interfaces

## Index

* [The language](#the-language)
* [The layout](#the-layout)
* [The C side](#the-c-side)

This page describes **NX HIPC/CMIF**. The independent `swipc --ctr` profile and
3DS transport are documented in [CTR IPC](CTR-IPC.md); they do not use the NX
wire layout or the successful stand-in described below.

The services the runtime emulates answer the game through IPC (`hle/kernel/ipc.c`). Each service interface is
declared once, in a `.swipc` file beside the service's C file, and `tools/swipc/swipc.c` generates the code that turns
a request into a call of a typed handler and the handler's outputs into the reply. The services' C files hold only
the handlers. This replaced one `switch (c->cmd)` per interface that read every argument by hand at an offset
(`in32(c, 8)`) and wrote every output in order (`out64(c, x)`), which is where layout mistakes hid (#155). The one
object without an interface is sm's stand-in for a service the runtime doesn't have (`sm.c`), which answers every
command with success.

## The language

It is SwIPC's (github.com/reswitched/SwIPC, ISC), with the conventions below.

```
# comments start with # (// and /* */ work too)
type nn::account::Uid = bytes<0x10, 8>;             # bytes<size[, alignment]>
type nn::account::ProfileDigest = bytes<0x10>;
type nn::account::profile::ProfileBase = struct {   # natural alignment, like the SDK's struct
    nn::account::Uid uid;
    u64 last_edit_timestamp;
    bytes<0x20> nickname;
};

interface nn::account::IAccountServiceForApplication is acc:u0, acc:u1, acc:su {
    [0] GetUserCount() -> i32 count;
    [2] ListAllUsers(array<nn::account::Uid, 0xa> users);
    [5] GetProfile(nn::account::Uid uid) -> object<nn::account::profile::IProfile> profile;
    @stub [6] GetProfileDigest(nn::account::Uid uid) -> nn::account::ProfileDigest digest;
    [51] TrySelectUserWithoutInteraction(b8 is_network_service_account_required) -> nn::account::Uid uid;
}
```

- Names are the SDK's (Switchbrew, libnx, the SDK's symbols). `is` lists the service names the interface is served
  as (sm's names are 8 bytes at most, and each is served by one interface). Every argument and output has a name, but
  `pid` and `aruid` may go without. Argument, field and command names become C names, so C keywords, `::` and the
  generated names (a command `dispatch`, `new` or `register`, a field `padN_`, an argument `self`) are refused. A
  struct is written in a `type` definition.
- Types: `u8` to `u64`, `i8` to `i64` (or `s8` to `s64`), `b8` (or `bool`), `f32`, `f64`, `bytes<N[, A]>`, `T[N]`,
  `struct {...}`, and names given with `type`. A type is defined before it is used. Types more than one file uses
  are in `hle/services/types.swipc`, which comes first in every list of the runtime's .swipc files, and in every
  test's that uses its types.
- `pid` is the client's process id (ClientProcessId): an 8 byte slot in the raw data whose value the kernel gives (0
  when the client sent none). It is one slot: SwIPC's `(u64, pid)` is written `pid` here. `aruid` is
  ClientAppletResourceUserId: the same slot, whose raw value the handler gets. Both are inputs, at most one of the two
  per command, and neither is a field, an element or a type's name.
- Buffers are inputs whatever their direction: `buffer<T, attributes[, size]>` and `array<T, attributes>`. The
  attributes are the numeric BufferAttribute: In 1, Out 2, MapAlias 4, Pointer 8, FixedSize 0x10, AutoSelect 0x20,
  NonSecure 0x40, NonDevice 0x80. So 5 is an A buffer, 6 B, 9 X, 0xa C, 0x19 and 0x1a fixed-size X and C, 0x21 and
  0x22 AutoSelect, 0x46 a NonSecure B. A FixedSize buffer reaches its handler with the type's size (or the given
  size), or the client's if that is smaller, as sf gives the handler the type's size.
- `object<I>` is an interface object (I is declared in some .swipc file, with no commands if none is served),
  `handle<copy, kind>` and `handle<move, kind>` a handle. A move handle is only an output: the kernel refuses a request
  that carries one, so the generator refuses one among a command's inputs.
- `@stub` before a command: the generated code answers it with success and zeros, and there is no handler. A command
  that gives an object or a handle can't be a stub. `@unsorted` before a command keeps its raw arguments in
  declaration order (see the layout). `@fallback(function)` before an interface names the C function that answers the
  commands it doesn't declare, `int function(SvcObj* self, IpcCtx* c)`: `quiet_ok` (success, silent), `ipc_unknown`
  (sf's UnknownCommandId, what the console answers), or one of the service's own, static in its file (bsd answers
  EOPNOTSUPP). Without it `stub_ok` answers success and logs the command the first three times. `@version(...)` and
  `@undocumented` are accepted and ignored.

## The layout

The generated code lays a command out as the SDK's service framework does (Atmosphère's
`sf_impl_command_serialization.hpp` and libnx's `cmif.h` describe the same layout; Atmosphère is GPL-2.0-only and
only read):

- The raw arguments are stable-sorted by alignment, ascending, and each goes at the next multiple of its alignment.
  `(u32 a, u64 b, u8 c)` puts c at 0, a at 4 and b at 8. The outputs are placed the same way, and the reply's raw size
  is rounded up to 4. A few commands are laid out in declaration order instead, as the SDK's own client code shows
  (fsp-srv's CreateSaveDataFileSystem and CreateSaveDataFileSystemWithHashSalt): those are marked `@unsorted`. The
  game's SDK names each command's layout in its client templates (`InRawArgumentInfo<size, alignment, offset>`), which
  is where to check a declaration.
- Each buffer's attributes give its descriptor list: A, B, X or C, or A and X (B and C) for AutoSelect. Each list is
  numbered in declaration order, and an AutoSelect buffer takes the next index in both of its lists. The server uses
  its mapped half when that has an address. Which half a libnx client fills depends on the server's pointer buffer,
  which QueryPointerBufferSize answers: 0x8000, or what the service sets in its object's `ptrbuf` (nsd's is 0, as
  the console's is).
- Copy handles in come in declaration order. Out copy and move handles too, each kind numbered on its own, and out
  objects become move handles (sessions) ahead of the declared move handles on a plain session, and object ids after the
  data on a domain. An out object a handler leaves NULL is handle 0 or id 0 (`respond` in `ipc.c`).
- A failed request answers no data, handles or objects. The generated code puts a command's objects and handles in the
  reply whatever its result, and `ipc.c` closes and drops a failed request's (`drop_outputs`) once the dispatcher has
  returned, outside any lock a service's own dispatcher holds.
- A CMIF request is measured as sf measures it: its data region starts at the data words aligned to 16 and is 16
  bytes shorter than they are, and a domain request's data ends where its domain header says. A TIPC request's ends
  with its data words.
- The kernel refuses some requests before any service sees them, and `ipc.c` does as it does (Mesosphere): a header that
  describes more than the client's message buffer, its receive list included, or a receive list inside the raw data
  (InvalidCombination, which the console finds at the reply, after the descriptors); raw data that ends past the
  server's 0x100 bytes (MessageTooLarge); a move handle, which a client can't send a server (InvalidCombination); a
  buffer outside the address space (InvalidCurrentMemory). A SendSyncRequestWithUserBuffer's own buffer has to be
  page-aligned (InvalidAddress), whole pages (InvalidSize), and memory that allows IPC user buffers, read-write with no
  attribute, with one state and permission across the range (InvalidCurrentMemory): `ipc_user_buffer`.
- A request has to send what its command declares: the pid flag when it takes `pid` or `aruid`, as many A, B and X
  descriptors (an AutoSelect buffer counts in both of its lists, and X descriptors past them must be empty: the
  console takes the empty half of a libnx AutoSelect buffer beside the mapped one a command declares), no exchange (W)
  buffer, as many copy handles, and at least the command's raw data. Otherwise it is answered with sf's InvalidCmifRequest, RC(11, 420), before the handler
  runs, and logged as `[bad request]` (`ipc_counts` and `ipc_bad_request`), as Atmosphère's HipcCommandProcessor checks
  it. C descriptors aren't counted. A domain request with another number of objects is answered InvalidNumInObjects
  (`ipc_objects`), checked before the rest as sf's DomainServiceObjectProcessor does, and a command that takes objects
  answers NotSupported on a plain session, which can't send them. A TIPC request (sm, from newer SDKs) carries its pid
  in the special header only, so its raw data has no pid slot, and the generated code reads its arguments where TIPC
  puts them. A request whose data region doesn't hold its CmifInHeader (or domain header) is answered InvalidHeaderSize,
  and one without the SFCI magic or with a version above 1 InvalidInHeader. So a declaration that differs from what the
  game's SDK sends fails every such request. One difference from the console, on purpose: sf closes the session for
  these refusals (the client's SendSyncRequest gets SessionClosed), while the runtime answers the result in a reply and
  keeps the session. Its own declarations can be wrong where the console's aren't, and a game may survive an error
  result but not a dead session.
- The generator refuses a command whose request or reply would not fit the 0x100 byte message with its headers,
  descriptors, handles and domain object ids (0xd0 bytes of raw data out at most), and more than 15 move handles and
  objects out.

## The C side

For `hle/services/acc.swipc` the build writes `hle/services/acc.swipc.h` and `acc.swipc.inc` under
`build/<preset>/src/nx/snx_hle-swipc/`, from the list in `src/nx/CMakeLists.txt` (`snx_swipc` in
`cmake/swipc.cmake`). For each interface `I` (its name with `::` as `_`) the header has the types, with their padding
spelled out and their size and field offsets checked by the compiler, and what other files use:

```c
int nn_account_IAccountServiceForApplication_dispatch(SvcObj* self, IpcCtx* c);
#define nn_account_IAccountServiceForApplication_register(ctor) ... /* when it has service names */
```

The `.inc` is included by the C file that defines the handlers, and by no other. It declares the handlers static and
defines the dispatchers, so each handler is compiled with its only caller:

```c
static int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                               SvcObj** profile);
#define nn_account_IAccountServiceForApplication_new(data_size) svcobj_new("nn::account::IAccountServiceForApplication", ...)
int nn_account_IAccountServiceForApplication_dispatch(SvcObj* self, IpcCtx* c) { ... }
```

The service's C file defines one static handler per command that isn't a `@stub`, and its own `@fallback` function
if it has one. The compiler checks each definition against its prototype, and a missing handler is an error. A
handler gets the object, then the arguments in declaration order, then a pointer for each output, which starts
zeroed:

| Argument | Input | Output |
| --- | --- | --- |
| scalar | the value | `T*` |
| `pid`, `aruid` | the value (`u64`) | |
| struct, named bytes or array | `const T*` | `T*` |
| `bytes<N>`, `T[N]` written in place | `const T*` to the first element | `T*` |
| buffer | `IpcBuf` (address, size; empty when the client sent none) | |
| object | `SvcObj*`, NULL when the client sent none: a reference the handler takes its own of to keep it | `SvcObj**`, a new object |
| handle | the client's handle value, 0 when it sent none | `u32*`, a handle for the reply |

It returns the result: 0, or an `RC(module, description)`. `I_new` makes an object whose dispatcher is the generated
one. An object that needs a lock around every command, or a check before each one, has a dispatcher of its own that
calls `I_dispatch`: fs.c's `file_disp`, `dir_disp` and `fs_disp`, ldr.c's `ro_dispatch`. `I_register` registers a
constructor under the interface's service names, in `services.c` or in the service's own init (fs, nvdrv, hid, vi,
pl). Names that need constructors of their own register by hand: ldr:ro and ro:1 are one interface with two kinds of
session, and bsd:u, bsd:a and bsd:s share one interface with bsd:s the system's.

The generator is strict C90 with no dependencies, so a cross build also builds it for the machine doing the build
(`cmake/swipc.cmake`, with that machine's compiler, `SNX_HOST_CC`). Its output is C90 as well. The tests
(`ctest -R swipc` in `build/default`): `tests/swipc_dispatch.c` drives the dispatchers generated from
`tests/swipc/test.swipc` and `other.swipc` through the layout rules, against stand-ins for what `ipc.c` gives them.
The same output is compiled with `-std=c90 -pedantic-errors` against a C90 stand-in for `ipc.h`
(`tests/swipc/c90/`, on 64-bit hosts other than Windows). `tests/swipc/bad/` holds definitions the generator has to
refuse, each with its line and message. What `ipc.c` itself does with a reply (the order of out objects among the move
handles, domain ids, a NULL object) is not covered by these tests, nor by the services' own, which give the
dispatchers stand-ins too: only the homebrew smoke test (nx_runtime_homebrew, with SNX_TEST_VULKAN) and the games go
through it.
