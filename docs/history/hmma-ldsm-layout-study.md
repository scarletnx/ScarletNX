<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The LDSM tile-address study

## Index

* [Why](#why)
* [The layouts](#the-layouts)
* [What ucas proved](#what-ucas-proved)
* [Measurements](#measurements)
* [Why games swizzle](#why-games-swizzle)
* [What this ruled out](#what-this-ruled-out)

*Snapshot: this page records a study and its measurements, as they were taken. The code has changed since, and the
page is not kept up to date. The experiment is kept as a `[DONOTMERGE]` draft PR, not as a change to the translator.*

## Why

An SM86 `LDSM.16` gathers its row addresses from the eight lanes that hold them: the general lowering shuffles those
addresses with `ds_bpermute` before each load. If the tile is row-major in lane order — lane `i` holds the address of
row `i` — every lane can derive its own element from one row base, with no address shuffles. The study asked whether
that saving is available to games, not only to synthetic benchmarks.

## The layouts

Write `l = 4g + t`, `g = l >> 2`, `t = l & 3`; a word holds two f16 halves `e`. A map is
`(lane, component) -> (row, col)`:

| layout | map |
| --- | --- |
| `LDSM.M88`, normal | `(l, m, e) -> (g, 2t+e)`, matrix `m` in lanes `8m..8m+7` |
| `LDSM.MT88`, transposed | `(l, m, e) -> (2t+e, g)` |
| `MOVM.MT88` | `(l, e) -> (2t+e, g)` |
| HMMA A, 16x8x16 | `(l, w, e) -> (g + 8*(w&1), 2t + 8*(w>>1) + e)` |
| HMMA B, 16x8x16 | `(l, h, e) -> (2t + 8*h + e, g)` |
| HMMA C/D, f32 | `(l, i, j) -> (g + 8*i, 2t+j)` |

D's owner of `(row, col)` is lane `4*(row % 8) + col/2`. This is why a normal LDSM feeds A and a transposed one feeds
B without a layout change. Hexcute (University of Toronto, 2025, https://hdl.handle.net/1807/150187) models
`ldmatrix`/`mma` as source and destination thread-value layouts and makes layout choice a constraint problem; the
full maps and their ucas prices are in the study PR's working note.

## What ucas proved

`lperm` on the swizzle `pi(lane) = (lane + 2*(lane & 7)) mod 32`:

* it is a permutation for even iteration counts, **not affine over F2**;
* its cheapest realisation is **one shuffle by the index** (cost 14), against a four-stage Benes network (46).

`banks` on the two shared-memory indices over 32 lanes, 4-byte elements:

| index | conflict degree |
| --- | ---: |
| `16*lane` (the row-major layout the direct path assumes) | 16 |
| `16*(lane + 2*(lane & 7))` (the game's swizzle) | 1 |

So the permutation is already realised at its cheapest by the shuffle the direct path would remove, and the swizzle
is what makes the game's LDS loads conflict-free.

## Measurements

Steam Deck, RADV VANGOGH, 16384 workgroups, ns per warp HMMA, alternating passes:

| form | general | tile-address proof |
| --- | ---: | ---: |
| gather | 3.915 | 3.385 |
| pairs | 3.549 | 2.673 |
| astat | 5.037 | 1.425 |

The direct path also removes 816 `ds_bpermute_b32` from the pairs shader's ISA. A 3060 Ti's pairs form goes from
about 2.6 to 2.1 ns. These are real numbers for a row-major tile; no game run reproduced them.

Whole-game checks with the experimental runtime built on the Deck:

| game | SM86 HMMA in the scene | proof triggers | result |
| --- | --- | --- | --- |
| Mario Kart World | none | no | path not exercised |
| Pokopia | none | no | path not exercised |
| Ocarina of Time remake | 2418 HMMA, 936 LDSM | no | 407 frames vs 405, no gain |

## Why games swizzle

The OoT remake's LDSM address chain is:

```
R116 = IMAD(R101, 16, RZ)        // the LDSM's address: 16*R101
R101 = IMAD(R101, 1, R48)        // loop-carried: R101 += R48
R48  = LOP3(R86, 7, RZ)          // R48 = R86 & 7
R86  = S2R                       // system register 0x00 = LANEID
```

so `addr(lane) = A + 16*(lane + k*(lane&7))`. The per-lane increment is lane-dependent: the row addresses held by
lanes are a swizzle of the tile's rows, changing each iteration. The swizzle is a bank-conflict fix (degree 16 to 1),
and the general path's address shuffles follow it. It is not an obstacle to remove — it is the reason the kernel is
fast. A runtime check of `addr(lane) == row0 + 16*lane` correctly rejects the tile and takes the general path.

## What this ruled out

The 4x result is available only to programs whose LDSM tile is row-major in lane order. No game we could run has one;
the one that uses SM86 HMMA swizzles its tile deliberately. A full permutation-inversion proof would not pay: ucas
prices the permutation at one shuffle, which is what the general path already does. The tile-address line is therefore
not a candidate for the runtime, and the experiment is kept only as a study.
