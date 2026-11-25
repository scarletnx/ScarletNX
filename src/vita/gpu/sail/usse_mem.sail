/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The USSE's loads, texture reads and special instructions (usse_mem.roots; back end usse_mem_spirv.h): VLDST's
   loads from a uniform buffer read from memory and from the literal buffer, SMP's dependent texture reads, KILL
   and DEPTHF (Vita3K's data.cpp, texture.cpp and special.cpp). The back end gives what is the translator's: the
   uniform buffers' storage buffer and the literal buffer's words, the textures' samplers and formats, the gathers
   and the registers a texture read fills (store_input) */

/* the literal buffer: the register that addresses it (-1 for none), the bytes its reads start at, whether the program
   holds its words (a Private array), whether a word at a byte offset is in the program, and that word */
val LiteralRegister : unit -> int
val LiteralBase : unit -> int
val HasLiterals : unit -> bool
val LiteralFits : int -> bool
val LiteralData : int -> int
/* the uniform buffers' storage buffer is there */
val HasBuffer : unit -> bool
/* the reads that follow, from the literal array and the storage buffer: their pointer type first; the word at a word
   index (the literal array's clamped to it) */
val LiteralsBegin : unit -> unit
val BufferBegin : unit -> unit
val LiteralRead : bits(32) -> bits(32)
val BufferRead : bits(32) -> bits(32)
/* register n of a bank written with a word */
val WordStore : (int, int, bits(32)) -> unit
/* the dependent sampler an SA register holds: its texture's index plus 1, 0 for none; a texture's format, the type
   its components read as, and how many it has */
val Dependent : int -> int
val TextureFormat : int -> int
val TextureType : int -> int
val TextureComponents : int -> int
/* texture resource's sampled image (cube: a cube's); the gather of its texels' components at a coordinate into
   registers from dest of a bank, as type, and (uv) the bilinear weights after them; a vec4's n components into
   registers from dest of a bank, as type */
val Sampler : (int, int) -> sampler
val Gather : (sampler, vec2, int, int, int, int, int) -> unit
val StoreInput : (int, int, int, vec4, int) -> unit
/* the fragment discarded; the program's depth output, and its value */
val Kill : unit -> unit
val HasDepth : unit -> bool
val DepthWrite : bits(32) -> unit

/* VLDST: words loaded from a uniform buffer read from memory, at src0 + src1 (+ 4 without moe expansion) + src2 bytes,
   src0 a register the program's uniform buffer table filled; or from the literal buffer, at a register's offset
   from its array, else (constant offsets, one word) from the program's bytes */
function execute_VLDST() -> unit = {
    let op = UInt(Field(59, 2));
    if op == 2 then throw(Refuse("VLDST stores"));
    if op != 1 then throw(Refuse("VLDST operation %u", op));
    let data_type = UInt(Field(36, 2));
    if data_type > 2 then throw(Refuse("VLDST of data type %u", data_type));
    let size = if data_type == 0 then 4 else if data_type == 1 then 2 else 1;
    let bytes = size * (UInt(Field(44, 4)) + 1);
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), Bit(50), 0, 7);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 0, 7);
    Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 0, 7);
    SetType(SRC0, T_I32);
    SetType(SRC1, T_I32);
    SetType(SRC2, T_I32);
    let bank = if Secondary() then B_SA else if Bit(39) == 1 then B_PA else B_TEMP;
    if BankOf(SRC1) == B_IMM then SetNum(SRC1, NumOf(SRC1) * size);
    let dest = UInt(Field(21, 7));
    if BankOf(SRC0) == B_SA & NumOf(SRC0) == LiteralRegister() then {
        if UInt(Field(44, 4)) != 0 | BankOf(SRC1) != B_IMM | BankOf(SRC2) != B_IMM then {
            /* at a register's offset: from the array */
            if not_bool(HasLiterals()) then throw(Refuse("a literal buffer read past the program"));
            Load(A, SRC1, 1, 0);
            Load(B, SRC2, 1, 0);
            if Failed() then throw(Stop());
            let b1 = if BankOf(SRC1) != B_IMM then ISubT(At(A, 0), SConst(65536)) else At(A, 0);
            let offset = IAddT(SConst(LiteralBase()), IAddT(b1, At(B, 0)));
            let first = ShrT(AsU32(offset), UConst(2));
            LiteralsBegin();
            foreach (i from 0 to DIV(bytes, 4) - 1) WordStore(bank, dest + i, LiteralRead(IAddT(first, UConst(i))))
        } else {
            let offset = LiteralBase() + NumOf(SRC1) + NumOf(SRC2);
            if not_bool(LiteralFits(offset)) then throw(Refuse("a literal past the program"));
            WordStore(bank, dest, UConst(LiteralData(offset)))
        }
    } else {
        if not_bool(HasBuffer()) then throw(Refuse("a VLDST with no uniform buffer"));
        Load(A, SRC0, 1, 0);
        Load(B, SRC1, 1, 0);
        Load(C, SRC2, 1, 0);
        if Failed() then throw(Stop());
        let a1 = if BankOf(SRC1) != B_IMM then ISubT(At(B, 0), SConst(65536)) else At(B, 0);
        let a2 = if Bit(53) == 0 then IAddT(a1, SConst(4)) else a1;
        let address = AsU32(IAddT(At(A, 0), IAddT(a2, At(C, 0))));
        let index = ShrT(address, UConst(2));
        BufferBegin();
        foreach (i from 0 to DIV(bytes, 4) - 1) WordStore(bank, dest + i, BufferRead(IAddT(index, UConst(i))))
    }
}

/* SMP: the texture of the dependent sampler src1 names, read at src0 (one, two or three components: a cube's), its
   level of detail from src2 where lod_mode says (1 a bias in a fragment program, 2 the level, 3 the gradients),
   into registers from dest (PA with use_pa, else the temporaries); sb_mode 1 and 3 its texels' gather (3 with the
   bilinear weights) */
function execute_SMP() -> unit = {
    let dim = UInt(Field(42, 2)) + 1;
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), Bit(50), 1, 8);
    Src12(SRC1, UInt(Field(7, 7)), UInt(Field(30, 2)), Bit(49), 1, 8);
    let src0_type = UInt(Field(35, 2));
    let coord_type = if src0_type == 0 then T_F32 else if src0_type == 1 then T_F16 else T_C10;
    SetType(SRC0, coord_type);
    let dependent = if BankOf(SRC1) == B_SA then Dependent(NumOf(SRC1)) else 0;
    if dependent == 0 then throw(Refuse("an SMP whose sampler isn't a dependent one"));
    let sb_mode = UInt(Field(37, 2));
    if sb_mode == 2 then throw(Refuse("SMP queries"));
    if dim > 3 then throw(Refuse("SMP dimension %u", dim));
    let resource = dependent - 1;
    if resource >= 16 then throw(Refuse("SMP sampler %u", resource));
    let format = TextureFormat(resource);
    let fconv = UInt(Field(46, 2));
    let type = if fconv == 2 then T_F16 else if fconv == 3 then T_F32 else TextureType(format);
    Load(A, SRC0, if dim == 1 then 1 else if dim == 2 then 3 else 7, 0);
    if Failed() then throw(Stop());
    if KindOf(A) != K_FLOAT then ToFloat(A, coord_type, 1);
    let coord = if dim == 3 then V3Of(At(A, 0), At(A, 1), At(A, 2))
                else V2Of(At(A, 0), if dim == 1 then Zero() else At(A, 1));
    let sampler = Sampler(resource, if dim == 3 then 1 else 0);
    let bank = if Bit(39) == 1 then (if Secondary() then B_SA else B_PA) else B_TEMP;
    if sb_mode == 3 & dim == 3 then throw(Refuse("SMP cube gather with UV coefficients"));
    let dest = UInt(Field(21, 7));
    if sb_mode != 0 then {
        /* the four texels' components: each component of the four, then the next component's */
        Gather(sampler, coord, format, type, bank, dest, if sb_mode == 3 then 1 else 0)
    } else {
        Src12(SRC2, UInt(Field(0, 7)), UInt(Field(28, 2)), Bit(48), 1, 8);
        SetType(SRC2, coord_type);
        let lod_mode = UInt(Field(40, 2));
        let implicit = Fragment() & (lod_mode == 0 | lod_mode == 1);
        let sampled = if lod_mode == 3 then {
            /* the gradients */
            if dim == 3 then {
                Load(B, SRC2, 7, 0);
                Load(C, SRC2, 7, 1)
            } else {
                Load(B, SRC2, 3, 0);
                Load(C, SRC2, 12, 0)
            };
            if Failed() then throw(Stop());
            let gx = if dim == 3 then V3Of(At(B, 0), At(B, 1), At(B, 2)) else V2Of(At(B, 0), At(B, 1));
            let gy = if dim == 3 then V3Of(At(C, 0), At(C, 1), At(C, 2)) else V2Of(At(C, 0), At(C, 1));
            SampleGrad(sampler, coord, gx, gy)
        } else if lod_mode != 0 then {
            Load(B, SRC2, 1, 0);
            if Failed() then throw(Stop());
            let lod = At(B, 0);
            if implicit then SampleBias(sampler, coord, lod)
            else SampleLod(sampler, coord, if lod_mode == 1 then Zero() else lod)
        } else {
            if implicit then SampleImplicit(sampler, coord) else SampleLod(sampler, coord, Zero())
        };
        StoreInput(bank, dest, type, sampled, TextureComponents(format))
    }
}

/* KILL: the fragment is discarded (predicated: the instruction's if around it) */
function execute_KILL() -> unit = {
    if not_bool(Fragment()) then throw(Refuse("KILL in a vertex program"));
    Kill()
}

/* DEPTHF: src0 is the fragment's depth */
function execute_DEPTHF() -> unit = {
    Src0(SRC0, UInt(Field(14, 7)), Bit(34), Bit(51), 1, 7);
    SetType(SRC0, T_F32);
    if not_bool(HasDepth()) then throw(Refuse("DEPTHF in a program that doesn't say it writes depth"));
    Load(A, SRC0, 1, 0);
    if not_bool(Failed()) then DepthWrite(At(A, 0))
}
