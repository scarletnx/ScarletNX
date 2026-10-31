# hactool, as scarletnx --extract runs it

[hactool](https://github.com/SciresM/hactool) by SciresM, from commit `1d64a83450e025622f3468c28fc4164dad2c5ef6`
(1.4.0), under its ISC license ([LICENSE](LICENSE)). ConvertUTF (Unicode license) comes with it, its notice at the top
of its files; its cJSON is the build's (`cmake/dependencies.cmake`). `storage.c` and `storage.h` port LibHac's storages
(MIT, Copyright (c) 2018 Alex Barney, as Ryubing maintains it), their notice at the top of `storage.c`. ScarletNX's
changes are GPL-3.0-or-later.

It is a library inside the driver (`cmake/hactool.cmake`), not a program: `main.c`, the bundled mbedtls and its LZ4 are
left out; its LZ4 calls go to the build's (`snx_lz4`). The driver calls it through `snx_hac.h` (`snx_hac.c`), and
`snx_io.h`, included ahead of every file, keeps what it prints from the console (it can hold keys), turns its `exit`
into one the driver cleans up after, and counts what it writes for the progress row. The files keep hactool's own style
(`.clang-format` here formats nothing).

What ScarletNX changed, each change marked `ScarletNX` where it is not a file of its own:

- **Decrypted dumps.** DXCI gamecards and DNCA content archives keep the containers' layouts with their sections in the
  clear (encryption None); a decrypted patch keeps its indirect (BKTR) tables. Header and table layouts follow
  hactool's `nca.h`, `xci.h` and `bktr.h`.
- **OpenSSL.** `aes.c`, `sha.c` and `rsa.c` wrap the build's OpenSSL instead of the bundled mbedtls 2.x, keeping
  their names and behavior. Two behaviors are corrections: a CBC decrypt writes its last block, and PSS and PKCS#1
  v1.5 signatures go through OpenSSL's verifier. Beta NCA0's OAEP keeps hactool's decoding, since only its private
  exponent is known.
- **LibHac's storages** (`storage.c`): sparse sections, compressed sections (LZ4 or zeros), bucket trees of any size
  (L2 nodes, gaps and integrity data after the tables), AesCtrEx runs left unencrypted, updates without AesCtrEx
  entries, and gamecard dumps that start with the card's 0x1000-byte key area. `nca.c` reads every section through a
  chain of them, as LibHac's `Nca` builds it, in place of its own CTR, XTS and BKTR seeking. Encryption types 5 and 6
  and the SHA3 hash types, which LibHac names but does not read, are refused with a message.
- **NCAs read in place.** `nca_ctx_t.base_offset` is where an NCA starts in its file, so one inside an XCI's secure
  partition or an NSP is read there instead of from a copy unpacked beside it.
- **Read in place, mapped.** `nca_ctx_t.map` (and each section's) is the container mapped into memory, which the
  sections' storage chains read from (`storage_memory_new`) instead of the file, so the runtime can read from several
  threads with no file position between them. `snx_hac_fs_open` keeps a program NCA's ExeFS and RomFS open, an
  update's over its base, for the runtime and the recompiler to read a dump they did not extract.
- **Big-endian hosts.** The formats are little-endian, and a big-endian host reads them through `snx_le32` and its kin
  (`types.h` includes `common/utilities/endian.h`). The NCA header (with its FS headers), PFS0 and HFS0 tables, RomFS
  headers, NSO headers and a save's header are put in the host's order once read (`nca_header_swap` and the `_swap` and
  `_from_le` helpers), and the RSA signatures and saved files see the bytes as stored. The XCI, NPDM, KIP, package and
  NAX0 headers stay as stored and their words are converted where they are read.
- **A file's own buffer.** `nca_save_section_file` saves a file through a buffer of its size, up to hactool's 4 MB,
  instead of 4 MB filled for every file: a RomFS is thousands of small ones, and where the heap hands a block that big
  out of fresh memory each time, as Windows' does, each fill faulted in 1024 pages.
- **Keys by name.** `extkeys_set_key` sets one prod.keys key, which `extkeys_initialize_settings` calls for each line
  of a file and the driver for each line it kept.
- **Comments.** Five comments in `aes.c`, `nca.h` and `packages.c` say "the console" or "content archive" instead of the
  console maker's name, which `tests/tracked_material.py` keeps out of the tracked files.
