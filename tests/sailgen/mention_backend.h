/* sailgen's tests (tests/CMakeLists.txt): a back end's text that defines sg_PSTATE_read and SG_PSTATE_N, and names
   sg_FPCR_read and SG_PSTATE_D only where nothing is defined: comments, a string, a declaration, a call and an
   initializer. Through it sailgen refuses FPCR and PSTATE.D as names the back end lacks. It is never compiled */
// sg_FPCR_read SG_PSTATE_D
enum { SG_PSTATE_N };
static const char* const sg_names = "sg_FPCR_read SG_PSTATE_D";
static inline SgD sg_FPCR_read(SgCtx* g);
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) { return sg_FPCR_read(g); }
static const int sg_fields[1] = {SG_PSTATE_D};
