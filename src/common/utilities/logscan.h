/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's log read as a summary: which build ran which game on which GPU, how the run ended (a crash, svcBreak, an
   unhandled exception, BRK, an instruction the recompiler cannot run, a fatal error, or nothing logged), the game
   function it ended in, the service commands the runtime doesn't implement, what else it missed (shaders, GPU
   methods, macros, SVCs, guest code the recompiler missed), warnings, and a signature, the same for every log that
   failed the same way.

   It reads a game's .log and the launcher's Share Log file (its game log section). What it writes is meant for public
   reports: paths under a home folder become ~, a line is cut at SNX_LOGSCAN_LINE bytes, and the crash report's code
   and memory dumps (the game's own bytes) are never copied. Only the C library: the launcher, the runtime's driver
   (scarletnx --scan-log) and the game reports workflow's snx-logscan build it alike.

   A log can be gigabytes: it is read once, a line at a time, into a state of fixed size (and a pool that holds only
   what the summary keeps), so a file is streamed in pieces and never held whole */
#ifndef SNX_LOGSCAN_H
#define SNX_LOGSCAN_H
#include <stddef.h>
#include <stdio.h>

#define SNX_LOGSCAN_SCHEMA   1 /* the JSON's "schema": a change of a field's meaning or a removal bumps it */
#define SNX_LOGSCAN_LINE     300
#define SNX_LOGSCAN_FRAMES   16
#define SNX_LOGSCAN_DETAILS  12
#define SNX_LOGSCAN_IPC      64
#define SNX_LOGSCAN_GAPS     64
#define SNX_LOGSCAN_WARNINGS 30
#define SNX_LOGSCAN_OUTPUT   10
#define SNX_LOGSCAN_TAIL     15
#define SNX_LOGSCAN_SLOW     5    /* the slowest frames whose lines the summary quotes */
#define SNX_LOGSCAN_RAW      1024 /* what is kept of a line for the last lines, before it is cut to SNX_LOGSCAN_LINE */

/* how the run ended, the first failure marker in the log */
enum {
    SNX_LOG_NONE,
    SNX_LOG_CRASH,
    SNX_LOG_SVCBREAK,
    SNX_LOG_EXCEPTION,
    SNX_LOG_BRK,
    SNX_LOG_UNDEFINED, /* an instruction the recompiler cannot run */
    SNX_LOG_THREAD_STOPPED,
    SNX_LOG_HOST_ABORT,
    SNX_LOG_FATAL
};
/* the service command lines, in the order the summary sorts them */
enum { SNX_IPC_BAD_REQUEST, SNX_IPC_UNDECLARED, SNX_IPC_UNKNOWN, SNX_IPC_UNKNOWN_CONTROL };
/* what the runtime missed, in the order the summary lists them. The GPU's: a graphics or compute shader the translator
   can't express, methods of a class no engine runs, another GPU operation skipped. The macros': one with no native
   form, one the compiler can't follow, an operation the interpreter doesn't know. The CPU's: an SVC with no handler,
   another SVC's unknown argument */
enum {
    SNX_GAP_SHADER,
    SNX_GAP_COMPUTE,
    SNX_GAP_CLASS,
    SNX_GAP_GPU,
    SNX_GAP_MACRO,
    SNX_GAP_MACRO_COMPILE,
    SNX_GAP_MME,
    SNX_GAP_SVC,
    SNX_GAP_CPU
};
/* where a crash report put the host pc */
enum { SNX_PLACE_NONE, SNX_PLACE_GAME, SNX_PLACE_BLOCK, SNX_PLACE_RUNTIME, SNX_PLACE_LIBRARY };

/* Every text is an offset into the scan's pool, 0 for none (the pool's first byte is an empty string) */
typedef struct {
    unsigned name, offset; /* "<name+offset>": a function or a module, the offset in hex */
} SnxLogSym;
typedef struct {
    unsigned address;
    SnxLogSym sym; /* name 0 for a frame the log names no symbol for */
} SnxLogFrame;
typedef struct {
    int kind;
    unsigned iface, command, count;
} SnxLogIpc;
typedef struct {
    unsigned message, count;
} SnxLogWarning;
typedef struct {
    int kind;
    unsigned what, count; /* count: the lines, for a shader the shaders */
} SnxLogGap;

typedef struct {
    char* pool; /* the texts, each terminated, grown as the scan adds them */
    size_t used, cap;
    /* the header: the game log's lines, else the launcher's build line */
    unsigned title, title_id, game_version, build, branch, commit, system, gpu, vulkan, driver;
    unsigned launcher_build, launcher_message; /* the Share Log file's; launcher_build 0 for a game log */
    long centiseconds;                         /* the last line's time, -1 without one */
    int outcome;
    unsigned signature;
    /* the failure, when outcome isn't SNX_LOG_NONE: the marker's fields, then its report's */
    unsigned signal, game_address, reason, result, imm, address, word, set, what, code, fatal;
    unsigned details[SNX_LOGSCAN_DETAILS];
    int ndetails;
    SnxLogSym pc;
    SnxLogFrame frames[SNX_LOGSCAN_FRAMES];
    int nframes;
    int place;
    unsigned place_detail[3];
    int function; /* the function the failure is pinned on: 0 none, 1 the pc, 2 + i frames[i] */
    SnxLogIpc ipc[SNX_LOGSCAN_IPC];
    int nipc;
    SnxLogWarning warnings[SNX_LOGSCAN_WARNINGS];
    int nwarnings;
    SnxLogGap gaps[SNX_LOGSCAN_GAPS];
    int ngaps;
    /* The macro calls by how they ran (mme.c's line every million calls, the last one, when some were interpreted) and
       its line of the macros interpreted most, both from snx_logscan_end. The guest code the recompiler missed, which
       the JIT compiles at run time (guest_jit.c): the pcs earlier runs missed, from its first line, and from its line
       at the end the pcs compiled, failed and still queued. jit says which lines were read: 1 the first, 2 the end */
    unsigned mme, mme_most;
    int jit;
    unsigned long jit_earlier, jit_compiled, jit_failed, jit_queued;
    /* The frames over 50 ms (gpu/vk/renderer.c): its stats line every 600 presents counts them, and each logs a line
       of what it did (200 at most): the counted ones, the lines since the last stats line, the slowest frame, and the
       slowest frames' lines with their times */
    unsigned long slow_counted, slow_since, slowest_ms;
    unsigned slow_line[SNX_LOGSCAN_SLOW]; /* from snx_logscan_end */
    unsigned long slow_ms[SNX_LOGSCAN_SLOW];
    int nslow;
    unsigned frame_stats, memory;        /* the last stats line and memory line, from snx_logscan_end */
    unsigned output[SNX_LOGSCAN_OUTPUT]; /* the game's own last lines ([game]), from snx_logscan_end */
    int noutput;
    unsigned tail[SNX_LOGSCAN_TAIL]; /* the last lines that aren't dumps, from snx_logscan_end */
    int ntail;
    /* the scan's state between lines */
    int phase;          /* a game log, or which section of the launcher's file the lines are in */
    int report_left;    /* lines the failure's report may still take, 0 once it ended */
    size_t mark;        /* the pool's size when the launcher's run section began */
    unsigned long seen; /* lines read */
    /* the newest lines as read, rings by seen and output_seen, made texts of the summary at the end */
    char tail_raw[SNX_LOGSCAN_TAIL][SNX_LOGSCAN_RAW];
    unsigned tail_len[SNX_LOGSCAN_TAIL];
    unsigned long tail_seen;
    char slow_raw[SNX_LOGSCAN_SLOW][SNX_LOGSCAN_RAW]; /* the slowest frames' lines, as they are read */
    unsigned slow_len[SNX_LOGSCAN_SLOW];
    char frame_stats_raw[SNX_LOGSCAN_RAW], memory_raw[SNX_LOGSCAN_RAW]; /* as they are read */
    unsigned frame_stats_len, memory_len;
    char mme_raw[SNX_LOGSCAN_RAW], mme_most_raw[SNX_LOGSCAN_RAW];
    unsigned mme_len, mme_most_len;
    char output_raw[SNX_LOGSCAN_OUTPUT][SNX_LOGSCAN_RAW];
    unsigned output_len[SNX_LOGSCAN_OUTPUT];
    unsigned long output_seen;
} SnxLogScan;

/* A mangled C++ name demangled into a malloc'd string, or NULL to keep it as it is */
typedef char* (*SnxDemangle)(const char* name);

/* A scan a line at a time: begin (0, or -1 out of memory), each line without its newline, then end, which makes the
   summary. snx_logscan_free empties s again, whichever way the scan ended */
int snx_logscan_begin(SnxLogScan* s);
void snx_logscan_line(SnxLogScan* s, const char* line, size_t n);
void snx_logscan_end(SnxLogScan* s);
/* The whole scan of size bytes of text, or of a file read in pieces (a line past 64 KiB keeps its first 64 KiB).
   0, or -1 out of memory or for a read error */
int snx_logscan(SnxLogScan* s, const char* text, size_t size);
int snx_logscan_file(SnxLogScan* s, FILE* f);
void snx_logscan_free(SnxLogScan* s);
const char* snx_logscan_text(const SnxLogScan* s, unsigned at);
const char* snx_logscan_outcome_name(int outcome);
const SnxLogSym* snx_logscan_function(const SnxLogScan* s);
/* The summary as one JSON object (one line) or as markdown for an issue or the Share Log file, malloc'd, NULL out of
   memory. The markdown escapes everything the log said (no mentions, links, images or closed code blocks): heading is
   its title's level ("###"), and brief leaves out the log's last lines, for a file that holds the whole log anyway.
   demangle may be NULL */
char* snx_logscan_json(const SnxLogScan* s, SnxDemangle demangle);
char* snx_logscan_markdown(const SnxLogScan* s, const char* name, const char* heading, int brief, SnxDemangle demangle);
/* the n bytes of text with each home folder's part as ~, as a report writes it (/home/<user>, C:\Users\<user>, Android's
   /data/user/<n>/<package>...), allocated and ended by a 0, its length in *size; NULL out of memory */
char* snx_logscan_tilde(const char* text, size_t n, size_t* size);
/* scarletnx --scan-log: LOG... (- for stdin) [--format json|markdown] [--name NAME], a JSON object a line or a summary
   a log, NAME the markdown's title for a log without a title line (its path otherwise). argv[0] is the command's name */
int snx_logscan_main(int argc, char** argv, SnxDemangle demangle);

#endif
