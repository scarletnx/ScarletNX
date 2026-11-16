<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The memory model's litmus tests

These five files state five of the six tests of `tests/gnuabi/litmus.c` in the format of herd7, the memory model
simulator of Jade Alglave and Luc Maranget ([herdtools7](https://github.com/herd/herdtools7), documentation at
[diy.inria.fr](http://diy.inria.fr/doc/herd.html)). The C fixture is what runs on the GNU ABI runtime. A file here is
what derives the expectations, so they can be checked instead of taken on trust: the model is the AArch64 application
level memory model of the Arm ARM, whose authoritative form is `aarch64.cat` in the herd7 distribution.

```sh
herd7 -cat aarch64.cat mp+dmb.ish.litmus     # or Arm's copy at developer.arm.com/herd7
```

`herd7` answers `Never` for the condition of each file, which says the outcome in its `exists` line is one no Arm core
produces. That outcome is the one `tests/gnuabi_litmus.py` fails on: the fence, the acquire or the release the file
orders the accesses with is the one the translator has to keep.

| File | Test | The ordering it asks for |
| --- | --- | --- |
| `sb+dmb.ish.litmus` | store buffering | a `DMB ISH` between a store and the other thread's load, in both threads |
| `mp+dmb.ish.litmus` | message passing | a `DMB ISH` between the two stores, and one between the two loads |
| `mp+rel-acq.litmus` | message passing | `STLR` for the release and `LDAR` for the acquire |
| `mp+dmb.ishst+dmb.ishld.litmus` | message passing | `DMB ISHST` for the writer and `DMB ISHLD` for the reader, the split barrier each host lowers differently |
| `wrc+dmb.ish.litmus` | weak read coherence, three threads | a `DMB ISH` after each load that publishes, and multi-copy atomicity |

Two of them are host evidence beyond the ordering itself. `wrc+dmb.ish.litmus` separates Arm's model from PowerPC's,
which is not multi-copy atomic and allows its outcome, which is why `docs/layers/cpu/plan.md` ranks the memory order P1
on that host. `mp+dmb.ishst+dmb.ishld.litmus` is one fence on Arm, two on x86-64 and `lwsync` plus a load fence on
PowerPC64, so a lowering that is right on one host and wrong on another shows up there first.

`tests/gnuabi_litmus.py --replay FILE` checks a recorded run of the fixture without a compiler or a runner. The file
holds the lines the fixture printed, one `litmus <test> <outcome> <count>` per outcome, one `litmus <test> done
<attempts>`, and one `litmus <test> bad <n>` when a read was not 0 or 1, grouped in any order. The outcome counts and
the bad reads together must equal the attempts, or the record is one the fixture could not have printed.

The spin loop of `T_SPIN` in the C fixture has no file here: it has no forbidden outcome. What it checks is that a
plain load in a loop is still a load of memory after the translator and LLVM are done with it, so the loop ends.
