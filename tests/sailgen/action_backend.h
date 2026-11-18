/* sailgen's tests (tests/CMakeLists.txt): a back end that defines the actions of refused_action.sail, sg_GPR_read and
   sg_Peek, whose val gives an integer, which an action may not. It is never compiled */
static inline SgD sg_GPR_read(SgCtx* g, long long n) { return sg_d_empty(64); }
static inline SgI sg_Peek(SgCtx* g, SgD address) { return sg_i_const(g, 0); }
