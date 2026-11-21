/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* SM53's system registers, the subgroup's votes and shuffles, and the barriers (sm53_thread.roots; their back end
   sm53_thread_spirv.h). Geometry invocation information follows Citron's emit_spirv_context_get_set.cpp:
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */

/* the program's stage (shader.h's ST_*) */
val Stage : unit -> int
/* the lane model: each invocation a warp of one lane (a fragment its quad's lane, else lane 0); an invocation of a
   tessellation control program is its lane; the fragment's lane in its quad is known */
val LaneModel : unit -> bool
val LaneIsInvocation : unit -> bool
val QuadLaneModel : unit -> bool
/* the lane: the invocation id, the lone lane, or the subgroup's */
val LaneId : unit -> bits(32)
val LoneLane : unit -> bits(32)
val SubgroupLane : unit -> bits(32)
/* a compute invocation's id in its workgroup (local) or its workgroup's, all three components (a vector) */
val ComputeIds : bool -> vec3
val VecAt : (vec3, int) -> bits(32)
/* the lane is a helper, or one a KIL inside the flow loop marked */
val LaneDead : unit -> bool
/* a tessellation or geometry program's invocation id, and a tessellation program's patch's vertices; the stage runs
   between the vertex and fragment stages, with an input primitive of InVertices vertices */
val InvocationId : unit -> bits(32)
val PatchVertices : unit -> bits(32)
val Between : unit -> bool
val InVertices : unit -> int
/* the clock (ReadClockKHR) of a scope: 3 the subgroup's, 1 the device's; its words */
val ReadClock : int -> clock2
val ClockAt : (clock2, int) -> bits(32)
/* base with n bits of insert at off (BitFieldInsert) */
val BitsInsert : (bits(32), bits(32), bits(32), bits(32)) -> bits(32)
/* the word of the subgroup's ballot of the predicate that holds the lane's 32 */
val BallotWord : (bits(32), bool) -> bits(32)
/* SHFL as a quad operation (the subgroup's own, when declared): 0 none, 1 a broadcast of a lane, 2 a swap, and its
   operand; the value of the quad's lane */
val QuadShfl : unit -> int
val QuadShflOperand : unit -> int
val SubgroupQuad : (bits(32), int, int) -> bits(32)
/* SHFL within the quad of a fragment's lone lane: it reads the pixel the lane would, through the quad's derivatives */
val QuadShflLanes : unit -> bool
val ShflQuadLanes : unit -> unit
val SubgroupShuffle : (bits(32), bits(32)) -> bits(32)
/* the barriers: a tessellation control program's patch, the workgroup's; memory's of a scope and semantics */
val PatchBarrier : unit -> unit
val ControlBarrier : unit -> unit
val MemoryBarrier : (int, int) -> unit

let ST_TCS : int = 1
let ST_GS : int = 3
let ST_FS : int = 4
let ST_CS : int = 5

/* S2R: the system register at 20 */
function execute_S2R() -> unit = {
    let sr = UInt(Field(20, 8));
    let stage = Stage();
    let value : bits(32) =
        /* the thread ids, SR_TID.X to .Z (0x21-0x23), and the block's, SR_CTAID.X to .Z (0x25-0x27) */
        if stage == ST_CS & ((sr >= 33 & sr <= 35) | (sr >= 37 & sr <= 39)) then {
            let local = sr <= 35;
            VecAt(ComputeIds(local), if local then sr - 33 else sr - 37)
        } else if sr == 0 | (sr >= 56 & sr <= 60) then {
            /* SR_LANEID, and the EQ, LT, LE, GT and GE lane masks of a 32-lane warp */
            let lane = LaneId() & 0x0000001f;
            let eq = 0x00000001 << lane;
            let lt = eq - 0x00000001;
            let le = lt | eq;
            match sr { 0 => lane, 56 => eq, 57 => lt, 58 => le, 59 => not_vec(le), _ => not_vec(lt) }
        } else if stage == ST_CS & sr == 32 then {
            /* SR_TID packed: x in bits 0-15, y in 16-25 and z in 26-31, the widths of the largest block (Eden gives y
               8 bits) */
            let tid = ComputeIds(true);
            let x = VecAt(tid, 0);
            let xy = BitsInsert(x, VecAt(tid, 1), 0x00000010, 0x0000000a);
            BitsInsert(xy, VecAt(tid, 2), 0x0000001a, 0x00000006)
        } else if stage == ST_FS & sr == 19 then (if LaneDead() then 0xffffffff else 0x00000000) /* SR_THREAD_KILL */
        else if sr == 17 then InvocationId() /* SR_INVOCATION_ID */
        else if sr >= 80 & sr <= 83 then ClockAt(ReadClock(if sr < 82 then 3 else 1), MOD(sr, 2))
        else if sr == 29 & Between() then {
            /* SR_INVOCATION_INFO: the vertices of the input primitive in bits 16-23, with the primitive's index in the
               batch (bits 0-7) 0, which makes the index the program builds for ISBERD the vertex's. Citron,
               acfb1dcc1ad9adf25b960c4f8f25c5bf78849406: geometry's input topology is known when translating. Its
               invocation info is a constant; tessellation remains dynamic */
            if stage == ST_GS then to_bits(32, InVertices() * 65536) else PatchVertices() << 16
        }
        /* Direct CBE writes are disabled: their enable and address registers (0x15..0x17) are zero */
        else if sr == 18 then 0x3f800000
        else if sr == 29 & stage != ST_FS then 0x00ff0000
        else 0x00000000;
    R_write(Dst(), value)
}

/* VOTE: the ballot of the predicate at 39 into d, and ALL (0), ANY (1) or EQ (2) of it (48-49) into the predicate at
   45. The lane model's warp is its lane alone: its ballot is its own bit */
function execute_VOTE() -> unit = {
    let value = PredRead(UInt(Field(39, 3)), UInt(Field(42, 1)));
    let one = Bits32(0x00000001); /* made here, as the translator made it */
    let lane_model = LaneModel();
    let lane = if lane_model then LoneLane() else SubgroupLane();
    let own = if not_bool(lane_model) then 0x00000000 else if QuadLaneModel() then one << lane else one;
    let mask = if lane_model then (if value then own else 0x00000000) else BallotWord(lane, value);
    let mode = UInt(Field(48, 2));
    if mode < 3 then {
        let any = mask != 0x00000000;
        let result = if mode == 1 then any else {
            let active = if lane_model then own else BallotWord(lane, true);
            let all = mask == active;
            if mode == 0 then all else all | not_bool(any)
        };
        PredWrite(UInt(Field(45, 3)), result)
    };
    R_write(Dst(), mask)
}

/* SHFL: PTX's shfl.sync, the lane at 20 (an immediate with bit 28) and c (the mask) at 34 (an immediate with bit 29) or
   the register at 39: c's bits 8-12 split the lanes into segments, its bits 0-4 clamp the lane. Mode (30-31) 0 IDX,
   1 UP, 2 DOWN, 3 BFLY. A quad's operation is the subgroup's own (QuadShfl), and a fragment's lone lane reads its
   quad through derivatives (ShflQuadLanes) */
function execute_SHFL() -> unit =
    if QuadShfl() != 0 then {
        let value = SubgroupQuad(R_read(RegA()), if QuadShfl() == 2 then 1 else 0, QuadShflOperand());
        PredWrite(UInt(Field(48, 3)), true);
        R_write(Dst(), value)
    } else if QuadShflLanes() then ShflQuadLanes() else {
        let index = if Field(28, 1) == 0b1 then ZeroExtend(Field(20, 5), 32) else R_read(RegB());
        let mask = if Field(29, 1) == 0b1 then ZeroExtend(Field(34, 13), 32) else R_read(UInt(Field(39, 8)));
        let lane_model = LaneModel();
        let lane_full = if lane_model then LoneLane() else SubgroupLane();
        let lane = lane_full & 0x0000001f;
        let seg = (mask >> 8) & 0x0000001f;
        let min_lane = lane & seg;
        let max_lane = min_lane | (mask & (not_vec(seg) & 0x0000001f));
        let mode = UInt(Field(30, 2));
        let source_lane : bits(32) = match mode {
            0 => min_lane | (index & not_vec(seg)),
            1 => lane - index,
            2 => lane + index,
            _ => EOR(lane, index)
        };
        let valid = if mode == 1 then not_bool(UInt(lane - min_lane) < UInt(index))
                    else not_bool(UInt(max_lane) < UInt(source_lane));
        let value = R_read(RegA());
        PredWrite(UInt(Field(48, 3)), valid);
        /* the lane model's lane alone: its own value, from its lane or one that runs nothing */
        if lane_model then R_write(Dst(), value) else {
            let host_lane = (lane_full & 0xffffffe0) | (source_lane & 0x0000001f);
            let shuffled = SubgroupShuffle(value, host_lane);
            R_write(Dst(), if valid then shuffled else value)
        }
    }

/* BAR: a tessellation control program's is its patch's barrier, any other the workgroup's */
function execute_BAR() -> unit = if Stage() == ST_TCS then PatchBarrier() else ControlBarrier()

/* CS2R: the clock (SR_CLOCKLO 0x50, of the subgroup's scope, else the device's) into a pair, SRZ zero */
function execute_CS2R() -> unit = {
    let sr = UInt(Field(20, 8));
    if sr == 255 then {
        R_write(Dst(), 0x00000000);
        if Dst() < 254 then R_write(Dst() + 1, 0x00000000)
    } else {
        let clock = ReadClock(if sr == 80 then 3 else 1);
        R_write(Dst(), ClockAt(clock, 0));
        if Dst() < 254 then R_write(Dst() + 1, ClockAt(clock, 1))
    }
}

/* MEMBAR: only compute has workgroup memory. Graphics memory barriers use Device scope even for MEMBAR.CTA; Vulkan
   does not permit Workgroup scope in those stages */
function execute_MEMBAR() -> unit = {
    let compute = Stage() == ST_CS;
    MemoryBarrier(if compute & Field(8, 2) == 0b00 then 2 else 1, if compute then 2376 else 2120) /* 0x948, 0x848 */
}
