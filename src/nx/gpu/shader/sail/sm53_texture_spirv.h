/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm53_texture.sail's back end (after sm53_spirv.h): emit_texture.c's lowerings of the texture and surface
   instructions */

[[maybe_unused]] static void sg_TexTld(SgG* g) { shader_tex_tld(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexSurface(SgG* g) { shader_tex_surface(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTlds(SgG* g) { shader_tex_tlds(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTexs(SgG* g) { shader_tex_texs(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTex(SgG* g) { shader_tex_tex(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTxd(SgG* g) { shader_tex_txd(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTxq(SgG* g) { shader_tex_txq(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTmml(SgG* g) { shader_tex_tmml(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
[[maybe_unused]] static void sg_TexTld4(SgG* g) { shader_tex_tld4(g->t, g->i, g->w, g->op, g->d, g->a, g->b); }
