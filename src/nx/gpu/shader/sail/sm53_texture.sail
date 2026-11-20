/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's texture and surface instructions (sm53_texture.roots; back end sm53_texture_spirv.h). Each is the
   translator's binding of the textures and images to the pipeline as much as an operation: the slot the prescan
   found for its handle (bound, bindless or an indexed table's element), the typed slots and their element swaps, the
   scaled render targets, the depth comparison by the slot's function, the min and max filters, the offsets' dispatch
   trees, the coordinates' registers by the texture's kind. That is emit_texture.c's (shader_tex_*), as SM86's
   texture() and surface() are sm86_translate.c's, which each instruction runs */

val TexTld : unit -> unit
val TexSurface : unit -> unit
val TexTlds : unit -> unit
val TexTexs : unit -> unit
val TexTex : unit -> unit
val TexTxd : unit -> unit
val TexTxq : unit -> unit
val TexTmml : unit -> unit
val TexTld4 : unit -> unit

/* TLD and TLD.B: a texel fetched at integer coordinates */
function execute_TLD() -> unit = TexTld()
/* SULD, SUST, SURED and SUATOM: a storage image's load, store, reduction and atomic */
function execute_SURFACE() -> unit = TexSurface()
/* TLDS and TEXS: the scalar forms' fetch and sample, their results into two register pairs */
function execute_TLDS() -> unit = TexTlds()
function execute_TEXS() -> unit = TexTexs()
/* TEX and TEX.B: a sample (its LOD, bias, offsets, depth comparison by their fields) */
function execute_TEX() -> unit = TexTex()
/* TXD and TXD.B: a sample with explicit gradients */
function execute_TXD() -> unit = TexTxd()
/* TXQ and TXQ.B: a texture's size, levels or kind */
function execute_TXQ() -> unit = TexTxq()
/* TMML and TMML.B: the levels a sample would take */
function execute_TMML() -> unit = TexTmml()
/* TLD4S, TLD4 and TLD4.B: a gather of four texels' component */
function execute_TLD4() -> unit = TexTld4()
