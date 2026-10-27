/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the MCP server's files share (mcp.c, mcp_edenprobe.c, mcp_gdb.c): a growing text, the edenprobe tools and the GDB
   tools */
#ifndef SNX_MCP_INT_H
#define SNX_MCP_INT_H
#include "common/utilities/common.h" /* SnxU64 */
#include <stddef.h>
#include <yyjson.h>

struct cJSON;

/* text grown as it is written, p NULL until then; lines counts what a tool chose to count */
typedef struct {
    char* p;
    size_t n, cap;
    int lines;
} Text;
void mcp_put_n(Text* t, const char* p, size_t n);
void mcp_put(Text* t, const char* p);
void mcp_put_num(Text* t, unsigned long v);
void mcp_put_hex(Text* t, SnxU64 v); /* lower case, no prefix */

/* The edenprobe tools (mcp_edenprobe.c): each returns its answer, malloc'd, or NULL with why in err */

/* A service case's mechanical parts written into the edenprobe checkout, every file or none: the case's enum entry
   and row, its judge and the judge's reference check (both to be filled in), the device side asking nothing yet, and
   its suite's row in ui.c when the suite is new. The answer lists what was written and what is left to write */
char* mcp_edenprobe_scaffold(const char* checkout, const char* suite, const char* id, const char* name,
                             const char* tier, long issue, Text* err);
/* make -f host.mk host-run in the checkout: the judges and their reference checks, on the host compiler. The
   answer: the lines about suite, the failures and the end */
char* mcp_edenprobe_check(const char* checkout, const char* suite, Text* err);
/* The NRO recompiled by self (this program) into cache (once while the NRO is unchanged) and run headless with the
   mode words until its summary, relaunched after a crash: the report's lines for filter ("suite" or "suite: name"),
   and its summary */
char* mcp_edenprobe_run(const char* self, const char* cache, const char* nro, const char* mode, const char* filter,
                        Text* err);
/* The case's lines in ScarletNX's report and the reference's (another emulator's, a console's), and the edenprobe pull
   request's text: from which issue, what each machine answered, the tests run, and what a case may not hold */
char* mcp_edenprobe_finding(long issue, const char* suite, const char* name, const char* ours, const char* theirs,
                            const char* source, Text* err);

/* The game tools (mcp_game.c): the games this machine has made, what each can play, recompiling one, and unattended
   runs. A game is named by its title ID, its folder, a program in it, or the name it was made by. game_info is a JSON
   object; game_recompile and game_run answer JSON too, their failure in err when the request itself is wrong */
char* mcp_build_info(void);
char* mcp_game_info(const char* data, const char* game, Text* err);
char* mcp_game_recompile(const char* self, const char* data, const char* game, const yyjson_val* args, Text* err);
char* mcp_game_run(const char* self, const char* data, const char* cache, const char* game, const yyjson_val* args,
                   Text* err);

/* The GDB tools (mcp_gdb.c, UNIX only): one GDB connected to a game's GDB stub, each returning its answer, malloc'd,
   or NULL with why in err. A namer writes a guest address's name (the game's module, function and offset) into t;
   NULL leaves addresses unnamed */
typedef void (*McpNamer)(Text* t, const void* ctx, SnxU64 address);
/* GDB started and connected to the stub at 127.0.0.1:port, the game stopped, a breakpoint on the runtime's
   crash_report, and SIGSEGV and SIGBUS (memory the runtime moves, its crash report) passed to the runtime unseen.
   Refused while a GDB is attached */
char* mcp_gdb_attach(long port, Text* err);
/* One of the commands gdb_command runs (mcp_gdb.c's list) and its output. One that resumes the game runs it in the
   background and waits up to wait seconds (at most 600) for it to stop, then interrupts it (0 leaves it running);
   "interrupt" stops it, and nothing else runs while it runs */
char* mcp_gdb_command(const char* command, long wait, McpNamer name, const void* ctx, Text* err);
/* Every host thread, and for a guest thread its id, name, whether it is in an SVC, and its guest pc and lr */
char* mcp_gdb_threads(McpNamer name, const void* ctx, Text* err);
/* GDB thread thread's (0: the selected one) guest registers and the guest backtrace from its frame pointer */
char* mcp_gdb_guest(long thread, int frames, McpNamer name, const void* ctx, Text* err);
/* size bytes of guest memory at address, as hex and text */
char* mcp_gdb_memory(SnxU64 address, long size, Text* err);
/* GDB detached from the game, which runs on, and ended */
char* mcp_gdb_detach(Text* err);
/* the same at the server's end, if a GDB is still connected */
void mcp_gdb_close(void);
/* The server's next message from stdin into line (pending: what was read past it, the caller's too), while GDB's
   output is taken as it comes (a running game's stops, its end), so GDB never waits for its pipe to drain: 1, or 0 at
   stdin's end */
int mcp_read_line(Text* pending, Text* line);

#endif
