/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's attributes and varyings (sm53_io.roots; back end sm53_io_spirv.h): ALD and AST by attribute address, OUT's
   vertices and primitives on their streams, AL2P, ISBERD, IPA and PIXLD. The attribute addresses are Maxwell's: 0x00-0x1f
   a patch's tessellation levels, 0x20-0x7f its vectors (a patch's) or the position (0x70) and the fixed ones (0x60 the
   primitive's number, 0x64 the layer, 0x68 the viewport, 0x6c the point size), 0x80-0x27f the generics, 0x2c0-0x2df
   the clip distances, 0x2e0 and 0x2e4 the point sprite's coordinates, 0x2f0-0x2f4 the tessellation coordinate,
   0x2f8 the instance, 0x2fc the vertex, 0x3a0 the viewport mask, 0x3fc the front face. Each one's variable is the
   translator's (its interface variables, which the back end reads and writes through) */

/* the stage's place: between the vertex and fragment stages (an input primitive of InVertices vertices, a control
   program's of OutVertices) */
val Between : unit -> bool
val InVertices : unit -> int
val OutVertices : unit -> int
/* the index of a vertex of words vertices that the program read in a register, in the shared array's range */
val SharedIndex : (bits(32), int) -> bits(32)
/* the variables the translator declared: generic input v (32 the position after the vertex stage), output attribute
   (by generic), the position, the point size (an output's or an input's), the clip distances (the planes used), the
   layer and viewport, a fragment's FragCoord */
val HasInput : int -> bool
val HasOutputAttr : int -> bool
val HasPosition : unit -> bool
val HasPointSize : bool -> bool
val HasClip : bool -> bool
val ClipUsed : int -> bool
val HasLayerOutput : unit -> bool
val HasLayerInput : unit -> bool
val HasViewportOutput : unit -> bool
val HasFragcoord : unit -> bool
/* an output attribute private to a control program's invocation (the Private storage class, 6, else Output, 3) */
val AttrPrivate : int -> bool
/* a word of input v (its component's bits) of a vertex (has_vertex) and component, as u32 */
val InputBits : (int, int, bits(32), bits(32)) -> bits(32)
/* a float of: a patch's vector p (a control program's own, its storage Output, else Input), a tessellation level
   (inner, k), the tessellation coordinate's component, the point size of a vertex (an output's or an input's), a clip
   distance of a vertex (an output's or input's), an output attribute's or the position's component of a vertex (an
   output), an input's or the position's (input) */
val PatchLoad : (int, int, bits(32)) -> bits(32)
val TessLevelLoad : (int, int, int) -> bits(32)
val TessCoord : int -> bits(32)
val PointSizeLoad : (bool, bits(32)) -> bits(32)
val ClipLoad : (bool, bits(32), int) -> bits(32)
val OutputLoad : (int, bits(32), bits(32)) -> bits(32)
val InputLoad : (int, bits(32), bits(32)) -> bits(32)
/* the primitive's number (a capture draw's original primitive's); the instance less the base instance, the vertex's
   index; the component of an input attribute as its own type (a typed register write) */
val PrimitiveRead : unit -> bits(32)
val InstanceId : unit -> bits(32)
val VertexId : unit -> bits(32)
val InputComponentWrite : (int, int, int) -> unit
/* the stores: a patch's vector or tessellation level, a vertex's point size, clip distance, output attribute or
   position component (as a control program stores its vertex), the vertex stages' own outputs */
/* (each takes the register it stores, read as the store makes it) */
val PatchStore : (int, int, bits(32)) -> unit
val TessLevelStore : (int, int, int) -> unit
val PointSizeStore : (int, bits(32)) -> unit
val ClipStore : (int, bits(32), int) -> unit
val OutputStore : (int, int, bits(32), bits(32)) -> unit
val PointSizeOut : int -> unit
val ViewportOut : int -> unit
val LayerOut : int -> unit
val ClipOut : (int, int) -> unit
val VectorOut : (int, int, int) -> unit
/* a control program's invocation id, its output vertex */
val InvocationIdInput : unit -> bits(32)
/* a geometry program's streams: the streams it emits on, its vertex emitted on stream k (the stream's capture of
   transform feedback for k other than 0: XfbCapture), its primitive ended; the vertex stage's end of a vertex (stream
   0: transform feedback, the position's fix, the primitive's number); the default point size after an emit */
val StreamMask : unit -> int
val XfbOn : unit -> bool
val PositionFix : unit -> bool
val PointSizeUsed : unit -> bool
val StreamSelect : (bits(32), int) -> unit
val StreamEnd : unit -> unit
val VertexEnd : unit -> unit
val XfbCapture : int -> unit
val EmitVertex : int -> unit
val EndPrimitive : int -> unit
val DefaultPointSize : unit -> unit
/* a fragment's inputs: the SPH's imap has generic v, its interpolation (1 constant, 2 perspective, 3 linear), the
   address is the position over its w (SH_VARYING_), the input's three vertices are read (pv_mask), and their
   value interpolated by the vertices' weights; FragCoord's component; the front face; the layer; the sprite's
   coordinate (T flipped with the pipeline's origin); the scale of a resolution; y with the pipeline's origin */
val SphImapAny : int -> bool
val IpaInterpolation : int -> int
val IpaPosition : int -> bool
val IpaOverW : unit -> bool
val PvMask : int -> bool
val FragNdc : int -> bits(32)
val ProvokingValue : (int, int) -> bits(32)
val FragCoordComponent : int -> bits(32)
val FrontFace : unit -> bool
val LayerInput : unit -> bits(32)
val PointCoordComponent : int -> bits(32)
val SpriteFlip : unit -> bits(32)
val NeedsScale : unit -> bool
val ResolutionScale : unit -> bits(32)
val FragYOrigin : bits(32) -> bits(32)
val InputComponent : (int, int) -> bits(32)
val InputScalar : (int, bits(32)) -> bits(32)
/* a float product the device must not fuse, and .SAT (the translator's float_saturate) */
val FMulNoContract : (bits(32), bits(32)) -> bits(32)
val FloatSaturate : (bits(32), int) -> bits(32)
/* a fragment's sample id, and the coverage word */
val SampleId : unit -> bits(32)
val CoverageWord : unit -> bits(32)

let ST_TCS : int = 1
let ST_FS : int = 4

/* ALD: width words (47-48) from the attribute address at 20 (plus the register at 8 when it is one), into d on. A vertex
   stage's own inputs by address; a stage between the vertex and fragment stages reads its input primitive's vertex
   (the register at 39), a control program a patch's (bit 31) or its own output vertices (bit 32) */
function execute_ALD() -> unit = {
    let width = UInt(Field(47, 2)) + 1;
    let between = Between();
    if RegA() != RZ then {
        /* indexed: each word at the register plus addr (and 4 for the next) is a declared input's component, the
           input picked by the address's vector (0x80 the first generic one, 0x70 the position after the vertex
           stage) and the component by its word. An address of no declared input reads 0, as in Eden */
        let vertex = if between then SharedIndex(R_read(UInt(Field(39, 8))), InVertices()) else 0x00000000;
        let base = R_read(RegA()) + ZeroExtend(Field(20, 10), 32);
        foreach (k from 0 to width - 1) {
            let offset = if k != 0 then base + to_bits(32, k * 4) else base;
            let vector = offset >> 4;
            let component = (offset >> 2) & 0x00000003;
            var value : bits(32) = 0x00000000;
            foreach (v from 0 to 32) {
                if (v < 32 & HasInput(v)) | (v == 32 & between & HasInput(32)) then {
                    let bits = InputBits(v, if between then 1 else 0, vertex, component);
                    let at = vector == to_bits(32, if v < 32 then 8 + v else 7);
                    value = if at then bits else value
                }
            };
            R_write(Dst() + k, value)
        }
    } else if between | Field(31, 1) == 0b1 then {
        let addr = UInt(Field(20, 10));
        let patch = Field(31, 1) == 0b1;
        let output = Stage() == ST_TCS & Field(32, 1) == 0b1 & not_bool(patch);
        let vertex = if patch then 0x00000000
                     else SharedIndex(R_read(UInt(Field(39, 8))), if output then OutVertices() else InVertices());
        foreach (k from 0 to width - 1) {
            let location = addr + k * 4;
            let component = to_bits(32, MOD(DIV(location, 4), 4));
            if patch then {
                /* the control program reads back what it wrote */
                if location >= 32 then R_writeF(Dst() + k, PatchLoad(if Stage() == ST_TCS then 3 else 1, DIV(location - 32, 16), component))
                else if location < 24 then R_writeF(Dst() + k, TessLevelLoad(if Stage() == ST_TCS then 3 else 1, if location < 16 then 0 else 1, DIV(MOD(location, 16), 4)))
                else R_write(Dst() + k, 0x00000000)
            } else if location == 752 | location == 756 then R_writeF(Dst() + k, TessCoord(DIV(location - 752, 4)))
            else if location == 96 then R_write(Dst() + k, PrimitiveRead())
            else if location == 108 then {
                if HasPointSize(output) then R_writeF(Dst() + k, PointSizeLoad(output, vertex)) else R_write(Dst() + k, 0x00000000)
            } else if location >= 704 & location < 736 then {
                if HasClip(output) then R_writeF(Dst() + k, ClipLoad(output, vertex, DIV(location - 704, 4)))
                else R_write(Dst() + k, 0x00000000)
            } else if output then R_writeF(Dst() + k, OutputLoad(location, vertex, component))
            else R_writeF(Dst() + k, InputLoad(location, vertex, component))
        }
    } else {
        let addr = UInt(Field(20, 10));
        foreach (k from 0 to width - 1) {
            let location = addr + k * 4;
            if location == 760 then R_write(Dst() + k, InstanceId())
            else if location == 764 then R_write(Dst() + k, VertexId())
            else InputComponentWrite(Dst() + k, DIV(location - 128, 16), MOD(DIV(location, 4), 4))
        }
    }
}

/* AST: width words (47-48) of d on to the attribute address at 20. A control program stores its invocation's output
   vertex, and the patch's levels and vectors (bit 31); the vertex stages store their own outputs */
function execute_AST() -> unit = {
    let addr = UInt(Field(20, 10));
    let width = UInt(Field(47, 2)) + 1;
    let patch = Field(31, 1) == 0b1;
    if Stage() == ST_TCS | patch then {
        let vertex = if patch then 0x00000000 else InvocationIdInput();
        foreach (k from 0 to width - 1) {
            let location = addr + k * 4;
            /* A later evaluation stage makes the control program's layer and viewport outputs inert */
            if patch | (location != 100 & location != 104) then {
                let component = to_bits(32, MOD(DIV(location, 4), 4));
                if not_bool(patch) & location == 108 & HasPointSize(true) then PointSizeStore(Dst() + k, vertex)
                else if not_bool(patch) & location >= 704 & location < 736 then {
                    if HasClip(true) & ClipUsed(DIV(location - 704, 4)) then ClipStore(Dst() + k, vertex, DIV(location - 704, 4))
                } else if not_bool(patch) then OutputStore(Dst() + k, location, vertex, component)
                else if location >= 32 then PatchStore(Dst() + k, DIV(location - 32, 16), component)
                else if location < 24 then TessLevelStore(Dst() + k, if location < 16 then 0 else 1, DIV(MOD(location, 16), 4))
            }
        }
    } else foreach (k from 0 to width - 1) {
        let location = addr + k * 4;
        if location == 108 then { if HasPointSize(true) then PointSizeOut(Dst() + k) }
        else if location == 104 then ViewportOut(Dst() + k)
        else if location == 100 then { if HasLayerOutput() then LayerOut(Dst() + k) } /* none: the device can't take this stage's layer */
        /* the viewport mask, 0 or viewport 0 alone (the prescan's check): no broadcast, the primitive keeps the
           viewport its index selects, as Eden and Ryujinx draw it on hosts without NV_viewport_array2 */
        else if location == 928 then ()
        else if location >= 704 then { if ClipUsed(DIV(location - 704, 4)) then ClipOut(DIV(location - 704, 4), Dst() + k) }
        else VectorOut(location, MOD(DIV(location, 4), 4), Dst() + k)
    }
}

/* OUT: a geometry program's vertex emitted (bit 39) and its primitive cut (bit 40) on the stream that the register
   at 20, the constant buffer word or the immediate names (its low two bits). Each stream the program uses is a
   selection of its own; stream 0's vertex is the vertex stages' end (transform feedback, the position's fix, the
   primitive's number), another's its transform feedback's capture. d becomes the next output's handle, 0 */
function execute_OUT() -> unit = {
    let stream0 = match Form() { 2 => ZeroExtend(Field(20, 2), 32), 1 => CbufWord(), _ => R_read(RegB()) };
    let stream = Bits32(stream0) & 0x00000003; /* a word at run time, an immediate's too */
    foreach (target from 0 to 3) {
        if target == 0 | MOD(DIV(StreamMask(), 2 ^ target), 2) == 1 then {
            StreamSelect(stream, target);
            if Field(39, 1) == 0b1 then {
                if target == 0 then VertexEnd() else XfbCapture(target);
                EmitVertex(target);
                if HasPointSize(true) & not_bool(PointSizeUsed()) then DefaultPointSize()
            };
            if Field(40, 1) == 0b1 then EndPrimitive(target);
            StreamEnd()
        }
    };
    R_write(Dst(), 0x00000000)
}

/* AL2P: the attribute's address, the register plus the offset at 20 */
function execute_AL2P() -> unit = {
    let offset = Bits32(ZeroExtend(Field(20, 11), 32)); /* made before the register's read */
    R_write(Dst(), R_read(RegA()) + offset)
}
/* ISBERD: the index is the vertex's as it is */
function execute_ISBERD() -> unit = R_write(Dst(), R_read(RegA()))

/* IPA: a fragment's input at the attribute address at 30 (or the register's, bit 38), interpolated in its mode, then
   IPA.MUL's product with the register at 20 (bits 54-55 1) and .SAT (bit 51) */
function execute_IPA() -> unit = {
    /* a perspective input by address that IPA.MUL multiplies by the reciprocal of a[0x7c] (IpaOverW) is attr itself:
       neither multiplication is made, which takes their roundings, and the infinity of attr times FragCoord.w past
       the float range, out of the value */
    let over : bool = if Field(38, 1) == 0b1 then false else {
        let addr = UInt(Field(30, 8)) * 4;
        let generic = addr >= 128 & addr != 1020 & addr != 736 & addr != 740;
        if generic & not_bool(IpaPosition(addr)) then IpaInterpolation(DIV(addr - 128, 16)) == 2 & IpaOverW() else false
    };
    let v : bits(32) = if Field(38, 1) == 0b1 then {
        /* indexed: the generic input at the index register's address, picked among those the header gives a mode by
           its vector (one nothing feeds reads 0, 0, 0, 1) and interpolated in its own mode */
        let offset = R_read(RegA());
        let vector = offset >> 4;
        let component = (offset >> 2) & 0x00000003;
        let last = component == 0x00000003;
        let wcoord = if HasFragcoord() then FragCoordComponent(3) else 0x00000000;
        var value : bits(32) = FConst(0x00000000);
        foreach (v from 0 to 31) {
            if SphImapAny(v) then {
                let bits = if HasInput(v) then {
                    let x = InputScalar(v, component);
                    if IpaInterpolation(v) == 2 & HasFragcoord() then FMulNoContract(x, wcoord) else x
                } else (if last then FConst(0x3f800000) else FConst(0x00000000)); /* no output feeds it (in_unfed_mask) */
                value = if vector == to_bits(32, 8 + v) then bits else value
            }
        };
        value
    } else {
        let addr = UInt(Field(30, 8)) * 4;
        let component = MOD(DIV(addr, 4), 4);
        /* a generic input's mode: the addresses before them in the chain below (the layer, the face, the primitive's
           number, the sprite's coordinates) take none */
        let generic = addr >= 128 & addr != 1020 & addr != 736 & addr != 740;
        let interpolation = if generic & not_bool(IpaPosition(addr)) then IpaInterpolation(DIV(addr - 128, 16)) else 0;
        let value : bits(32) = if addr == 100 then Float32(if HasLayerInput() then Bits32(LayerInput()) else 0x00000000)
            else if addr == 1020 then Float32(if FrontFace() then 0xffffffff else 0x00000000)
            else if addr == 96 then Float32(PrimitiveRead())
            else if addr == 736 | addr == 740 then {
                let x = PointCoordComponent(component);
                /* T flips with the sprite's origin, the pipeline's (SH_SPEC_SPRITE_FLIP) */
                if addr == 740 then {
                    let flip = SpriteFlip();
                    let flipped = FAdd(FConst(0x3f800000), FNeg(x));
                    if flip != 0x00000000 then flipped else x
                } else x
            } else if addr >= 128 then {
                let input = DIV(addr - 128, 16);
                if IpaPosition(addr) then (if component == 3 then FConst(0x3f800000) else FragNdc(component))
                /* the position over its w (SH_VARYING_): the fragment's own x and y, and 1 */
                else if input < 32 & PvMask(input) then ProvokingValue(input, component)
                else if HasInput(input) then InputComponent(input, component)
                else FConst(if component == 3 then 0x3f800000 else 0x00000000) /* no output feeds it (in_unfed_mask) */
            } else FConst(0x00000000);
        if addr >= 112 & addr < 128 then {
            let x = FragCoordComponent(component);
            /* a[0x7c] is the interpolated 1/w, FragCoord.w as it is: shaders take its reciprocal for IPA.MUL */
            let scaled = if addr != 124 & NeedsScale() & component < 2 then FMulNoContract(x, ResolutionScale()) else x;
            if addr == 116 then FragYOrigin(scaled) else scaled
        } else if interpolation == 2 & not_bool(over) then
            /* The hardware interpolates attr/w for a perspective input, and the shader multiplies by w (IPA.MUL with
               the reciprocal of a[0x7c]). Vulkan's input is attr itself: times the interpolated 1/w, FragCoord.w, it
               is what the hardware gives */
            FMulNoContract(value, FragCoordComponent(3))
        else value
    };
    let m = if Field(54, 2) == 0b01 & not_bool(over) then FMulNoContract(v, R_readF(RegB())) else v;
    R_writeF(Dst(), FloatSaturate(m, UInt(Field(51, 1))))
}

/* PIXLD: the sample's id (31-33 5) or the coverage */
function execute_PIXLD() -> unit = R_write(Dst(), if Field(31, 3) == 0b101 then SampleId() else CoverageWord())
