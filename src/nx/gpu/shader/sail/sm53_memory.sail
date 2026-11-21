/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's constant buffer, local, shared and global memory instructions (sm53_memory.roots; back end
   sm53_memory_spirv.h). A word's place is a pointer (an opaque value) into the memory to_spirv declared for it: the
   shared array (or its buffer, where the program's shared memory spills), the local array, a global buffer's slot.
   The read-modify-write loops that a 32-bit device makes of a byte's store and of the atomics SPIR-V has not
   (a CAS loop each) are the back end's */

/* the constant buffer word of bank at a run-time byte address, and of a bank picked at run time from first on */
val CbufReadBank : (int, bits(32), bits(32)) -> bits(32)
/* the textures the translation found: how many, texture k's index taken from this LDC (its bl2_off), the stride's
   shift of its element; the index stored */
val TextureCount : unit -> int
val TexIndexedBy : int -> bool
val TexIndexShift : int -> int
val TexIndexStore : (int, bits(32)) -> unit
/* the program's words of shared and local memory */
val SharedWords : unit -> int
val LocalWords : unit -> int
/* a shared word's pointer (at its index in the program's own, shader_shared_index's), a local word's, a global
   buffer slot's word's (its index past the buffer's base made the slot's, shader_gbuf_word_index) */
val SharedPointer : bits(32) -> wordptr
val LocalPointer : bits(32) -> wordptr
val GbufPointer : (int, bits(32)) -> wordptr
val Load : wordptr -> bits(32)
val Store : (wordptr, bits(32)) -> unit
/* atomic loads (a storage buffer's, Device scope; shared memory's) */
val AtomicLoad : wordptr -> bits(32)
val AtomicLoadShared : wordptr -> bits(32)
/* n bits of register r at off into the word, a compare-and-swap loop till it lands (a byte's or a halfword's store
   where another lane may store the word's other bytes); the register is read in the loop */
val CasInsert : (wordptr, bits(32), bits(32), int) -> unit
val CasInsertShared : (wordptr, bits(32), bits(32), int) -> unit
/* the atomics: SPIR-V's of an opcode, its compare-and-exchange (the new, the comparator), and the ones it has not, a
   CAS loop of the wrap step (INC, DEC: shader_wrap_step) or of a float add, each giving the old value */
val AtomicOp : (int, wordptr, bits(32)) -> bits(32)
val AtomicOpShared : (int, wordptr, bits(32)) -> bits(32)
val AtomicCas : (wordptr, bits(32), bits(32)) -> bits(32)
val AtomicCasShared : (wordptr, bits(32), bits(32)) -> bits(32)
val AtomicLoopShared : (wordptr, int, int) -> bits(32) /* its value the register read in the loop */
val AtomicLoop : (wordptr, bits(32), int, int) -> bits(32)
/* SPIR-V's opcode of an ATOMS (0xfffffffe EXCH of a pair, 0xffffffff none: the wrap loop) and of a RED or ATOM
   (0xffffffff none) */
val SharedAtomicOpcode : unit -> int
val GlobalAtomicOpcode : int -> int
/* the index of the word an ATOMS addresses (the register at 8 plus its immediate, shader_shared_atomic_index) */
val SharedAtomicIndex : unit -> bits(32)
/* the global buffer slot the prescan found for this instruction (gb_at: 0 none, 255 an attribute's export, 254 a
   pointer only known at run time, else the slot + 1), the constant buffer word holding that slot's base (bank,
   offset), an access by a run-time pointer (shader_global_dynamic) */
val GbAt : unit -> int
val GbufBank : int -> int
val GbufOffset : int -> int
val GlobalDynamic : unit -> unit
/* an atomic's pointer into a global buffer slot (shader_global_atomic_pointer), its byte offset AtomicOffset's */
val GlobalAtomicPointer : int -> wordptr
val AtomicOffset : unit -> bits(32)
/* bytes written at a global slot's byte offset, for the buffer's write-back (log_written) */
val LogWritten : (int, bits(32), int) -> unit

/* a byte (size 0, 1 signed) or halfword (2, 3 signed) of a word, at its address's place in the word */
function SubwordOffset(address : bits(32), size : int) -> bits(32) =
    (address & (if size < 2 then 0x00000003 else 0x00000002)) << 3
function SubwordExtract(word : bits(32), bit_offset : bits(32), width : bits(32), size : int) -> bits(32) =
    if MOD(size, 2) == 1 then Bits32(BitsS(word, bit_offset, width)) else BitsU(word, bit_offset, width)
/* a byte's or a halfword's width, made where the translator made it: before the word's load */
function SubwordWidth(size : int) -> bits(32) = Bits32(if size < 2 then 0x00000008 else 0x00000010)
/* the words of a size at 48-50: 5 two, 6 and 7 four */
function SizeWords(size : int) -> int = if size == 5 then 2 else if size >= 6 then 4 else 1

/* LDC: the constant buffer word at the register at 8 plus the signed offset at 20, of bank 36 (bit 44: the register's
   high half added to the bank, its low half the address), a byte or halfword of it (48-50) or a pair. It loads the
   handle of a texture that is indexed: the element it picks, the address register over the stride, stored */
function execute_LDC() -> unit = {
    let off = Bits32(SignExtend(Field(20, 16), 32)); /* made before the register's read, as the translator made it */
    let a = R_read(RegA());
    let bank = UInt(Field(36, 5));
    let dynamic_bank = Field(44, 1) == 0b1;
    let picked = if dynamic_bank then to_bits(32, bank) + (a >> 16) else 0x00000000;
    let source = if dynamic_bank then a & 0x0000ffff else a;
    let address = source + off;
    let word = if dynamic_bank then CbufReadBank(bank, picked, address) else CbufReadDynamic(bank, address);
    let size = UInt(Field(48, 3));
    foreach (k from 0 to TextureCount() - 1) {
        if TexIndexedBy(k) then TexIndexStore(k, R_read(RegA()) >> TexIndexShift(k))
    };
    if size < 4 then {
        let byte_offset = address & (if size < 2 then 0x00000003 else 0x00000002);
        let bit_offset = byte_offset << 3;
        R_write(Dst(), SubwordExtract(word, bit_offset, SubwordWidth(size), size))
    } else R_write(Dst(), word);
    if size == 5 then {
        let next = address + 0x00000004;
        R_write(Dst() + 1, if dynamic_bank then CbufReadBank(bank, picked, next) else CbufReadDynamic(bank, next))
    }
}

/* ATOMS: an atomic of shared memory, the operation SharedAtomicOpcode's: EXCH of a pair (two words exchanged, not one
   atomic), SPIR-V's own, or the wrap step's CAS loop (52-55: 3 INC, else DEC) */
function execute_ATOMS() -> unit = {
    let index = SharedAtomicIndex();
    let pointer = SharedPointer(index);
    let operation = SharedAtomicOpcode();
    if operation == 4294967294 then {
        let next_index = SharedIndex(index + 0x00000001, SharedWords());
        let high = SharedPointer(next_index);
        let old_low = Load(pointer);
        let old_high = Load(high);
        Store(pointer, R_read(RegB()));
        Store(high, R_read(RegB() + 1));
        R_write(Dst(), old_low);
        R_write(Dst() + 1, old_high)
    } else if operation != 4294967295 then R_write(Dst(), AtomicOpShared(operation, pointer, R_read(RegB())))
    else R_write(Dst(), AtomicLoopShared(pointer, RegB(), if Field(52, 4) != 0x3 then 1 else 0))
}

/* LDS and STS: shared memory at the register at 8 plus the signed offset at 20 (RZ: the offset alone), words of the
   size at 48-50 or a byte or halfword of the first, which a store puts in with a CAS loop (another lane may store the
   word's other bytes) */
function execute_LDS() -> unit = {
    let store = Form() == 1;
    let immediate = Bits32(SignExtend(Field(20, 24), 32)); /* made before the register's read */
    let address = if RegA() == RZ then immediate else R_read(RegA()) + immediate;
    let first = address >> 2;
    let size = UInt(Field(48, 3));
    foreach (k from 0 to SizeWords(size) - 1) {
        let index0 = if k != 0 then first + to_bits(32, k) else first;
        let pointer = SharedPointer(SharedIndex(index0, SharedWords()));
        if size < 4 then {
            let bit_offset = SubwordOffset(address, size);
            let width = SubwordWidth(size);
            if store then CasInsertShared(pointer, bit_offset, width, Dst())
            else R_write(Dst(), SubwordExtract(AtomicLoadShared(pointer), bit_offset, width, size))
        } else if store then Store(pointer, R_read(Dst() + k))
        else R_write(Dst() + k, Load(pointer))
    }
}

/* LDL and STL: local memory, addressed as LDS is, its words wrapped to the array's; a byte's store a load and a
   store (the invocation's own memory) */
function execute_LDL() -> unit = {
    let store = Form() == 1;
    let immediate = Bits32(SignExtend(Field(20, 24), 32)); /* made before the register's read */
    let address = if RegA() == RZ then immediate else R_read(RegA()) + immediate;
    let first = address >> 2;
    let size = UInt(Field(48, 3));
    foreach (k from 0 to SizeWords(size) - 1) {
        let index0 = if k != 0 then first + to_bits(32, k) else first;
        let pointer = LocalPointer(index0 & to_bits(32, LocalWords() - 1));
        if size < 4 then {
            let bit_offset = SubwordOffset(address, size);
            let width = SubwordWidth(size);
            let old = Load(pointer);
            if store then Store(pointer, BitsInsert(old, R_read(Dst()), bit_offset, width))
            else R_write(Dst(), SubwordExtract(old, bit_offset, width, size))
        } else if store then Store(pointer, R_read(Dst() + k))
        else R_write(Dst() + k, Load(pointer))
    }
}

/* LDG and STG: a global buffer slot's words at the register at 8 plus the signed offset at 20, less the slot's base
   (its constant buffer word). An STG the prescan found to be the attribute export (SR_DIRECTCBEWRITEENABLED is zero:
   the shader's ordinary path) does nothing; a pointer only known at run time is shader_global_dynamic's */
function execute_LDG() -> unit = {
    let store = Form() == 1;
    let at = GbAt();
    if store & at == 255 then () else if at == 254 then GlobalDynamic() else {
        let slot = at - 1;
        let base = CbufReadDynamic(GbufBank(slot), to_bits(32, GbufOffset(slot)));
        let immediate = Bits32(SignExtend(Field(20, 24), 32)); /* made before the register's read */
        let address0 = R_read(RegA()) + immediate;
        let address = address0 - base;
        let index = address >> 2;
        let size = UInt(Field(48, 3));
        let words = SizeWords(size);
        if store then LogWritten(slot, address, if size < 2 then 1 else if size < 4 then 2 else words * 4);
        foreach (k from 0 to words - 1) {
            let word0 = if k != 0 then index + to_bits(32, k) else index;
            let ptr = GbufPointer(slot, word0);
            if size < 4 then {
                let byte_offset = address & (if size < 2 then 0x00000003 else 0x00000002);
                let bit_offset = byte_offset << 3;
                let width = SubwordWidth(size);
                if store then CasInsert(ptr, bit_offset, width, Dst())
                else R_write(Dst(), SubwordExtract(AtomicLoad(ptr), bit_offset, width, size))
            } else if store then Store(ptr, R_read(Dst() + k))
            else R_write(Dst() + k, Load(ptr))
        }
    }
}

/* ATOM.CAS: compare (the register at 20) and swap in the next register's value, of a global slot's word */
function execute_ATOM_CAS() -> unit = {
    let slot = GbAt() - 1;
    let pointer = GlobalAtomicPointer(slot);
    LogWritten(slot, AtomicOffset(), 4);
    let next = R_read(RegB() + 1);
    R_write(Dst(), AtomicCas(pointer, next, R_read(RegB())))
}
function execute_ATOMS_CAS() -> unit = {
    let pointer = SharedPointer(SharedAtomicIndex());
    let next = R_read(RegB() + 1);
    R_write(Dst(), AtomicCasShared(pointer, next, R_read(RegB())))
}

/* RED and ATOM on a global slot's word, each one 32 bits (shader_global_atomic_opcode): SPIR-V's atomic, or a CAS loop
   of a float add (size 3) or the wrap step (operation 3 INC, else DEC). ATOM returns the old value; RED's data is d */
function execute_RED() -> unit = {
    let red = Form() == 0;
    let slot = GbAt() - 1;
    let pointer = GlobalAtomicPointer(slot);
    LogWritten(slot, AtomicOffset(), 4);
    let value = R_read(if red then Dst() else RegB());
    let opcode = GlobalAtomicOpcode(if red then 1 else 0);
    let prior = if opcode != 4294967295 then AtomicOp(opcode, pointer, value)
                else AtomicLoop(pointer, value, if red then UInt(Field(20, 3)) else UInt(Field(49, 3)),
                                if red then UInt(Field(23, 3)) else UInt(Field(52, 4)));
    if not_bool(red) then R_write(Dst(), prior)
}
