/*
 * TNR DEBUGGER extension (fork patch) — public + anchor-facing API.
 *
 * VS Code breakpoint debugging for JS running inside the embedded engine.
 * Design lineage: koush/quickjs + koush/vscode-quickjs-debug (MIT), ported to
 * quickjs-ng by the threejs-native-runtime fork. The wire protocol is koush's
 * length-prefixed JSON ("<8 hex chars>\n<json>\n", DAP-shaped messages); see
 * phase12-quickjs-dap-debugger-plan.md in the consuming repo.
 *
 * MERGE-SURFACE CONTRACT (enforced by the consuming repo's gate):
 *  - quickjs.h is NEVER modified by this feature.
 *  - quickjs.c carries exactly 13 anchors, each a few lines, each wrapped in
 *    #ifdef TNR_QJS_DEBUGGER and tagged "TNR DEBUGGER (fork patch)":
 *      A1  top-of-file #include of this header (types for struct fields)
 *      A2  JSRuntime gains `JSDebuggerInfo debugger_info;`
 *      A3  JSFunctionBytecode gains `JSDebuggerFunctionInfo debugger;`
 *      A4  JS_FreeRuntime calls js_debugger_free()
 *      A5  JS_NewContext calls js_debugger_new_context()
 *      A6  JS_FreeContext calls js_debugger_free_context()
 *      A7  JS_Throw calls js_debugger_exception()
 *      A8  JS_CallInternal: debugger twin dispatch table + CASE/SWITCH macros
 *      A9  JS_CallInternal: per-call-frame check at the restart label
 *      A10 free_function_bytecode frees the per-function breakpoint map
 *      A11 tail #include of quickjs-debugger.c (textual, after quickjs-aot.c)
 *      A12 compiler final pass: per-op pc2line recording (statement-accurate
 *          breakpoint positions; debug builds only)
 *      A13 parser: one OP_source_loc per statement start (upstream marks only
 *          calls/assignments/throw/expr-statements — `return x;` and let/const
 *          lines otherwise have no position at all; debug builds only)
 *  - Everything else lives in quickjs-debugger.c, which is NOT a standalone
 *    translation unit: it is textually #included at the very end of quickjs.c
 *    (same doctrine as quickjs-aot.c) so its helpers can use file-local
 *    statics (find_line_num, JSStackFrame, vardefs, closure_var, the eval
 *    plumbing) without widening the upstream surface.
 *
 * Nothing in this feature compiles unless TNR_QJS_DEBUGGER is defined; a
 * release build is bit-for-bit free of it.
 *
 * THREADING: everything runs on the thread that owns the JSRuntime. There is
 * no debug thread; while paused, the engine blocks in a read-dispatch loop on
 * the transport. The host supplies the transport as four callbacks (TCP,
 * socketpair, pipe — the engine does not know and must not care: this file
 * stays OS-free).
 */
#ifndef QUICKJS_DEBUGGER_H
#define QUICKJS_DEBUGGER_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-function breakpoint cache, laid alongside the bytecode: one byte per
   bytecode byte, 1 = a breakpoint covers this pc. Rebuilt lazily when `dirty`
   falls behind JSDebuggerInfo.breakpoints_dirty_counter. NULL until the first
   breakpoint lands in the function's file. */
typedef struct JSDebuggerFunctionInfo {
    uint8_t *breakpoints;
    uint32_t dirty;
} JSDebuggerFunctionInfo;

/* A source position as the engine sees it. filename is a JSAtom compared by
   value only (never dereferenced after the owning function may be gone).
   Unlike the Bellard-era original, quickjs-ng's pc2line carries columns, so
   column is real (1-based) — not always 0. */
typedef struct JSDebuggerLocation {
    JSAtom filename;
    int line;
    int column;
} JSDebuggerLocation;

#define JS_DEBUGGER_STEP          1
#define JS_DEBUGGER_STEP_IN       2
#define JS_DEBUGGER_STEP_OUT      3
#define JS_DEBUGGER_STEP_CONTINUE 4

/* One per JSRuntime (embedded in it — anchor A2). All JSValues in here belong
   to debugging_ctx, a dedicated context created on attach so debugger state
   never pollutes the debuggee's context. transport_close doubles as the
   "attached" flag: the interpreter selects the instrumented dispatch table
   iff it is non-NULL, so a detached engine pays nothing per opcode. */
typedef struct JSDebuggerInfo {
    /* ctx = debuggee context of the current entry; only set while inside a
       debugger operation. debugging_ctx = the debugger's own scratch context. */
    JSContext *ctx;
    JSContext *debugging_ctx;

    int peek_ticks;
    int should_peek;
    char *message_buffer;
    int message_buffer_length;
    int is_debugging;   /* reentrancy guard for js_debugger_check */
    int is_paused;

    size_t (*transport_read)(void *udata, char *buffer, size_t length);
    size_t (*transport_write)(void *udata, const char *buffer, size_t length);
    size_t (*transport_peek)(void *udata);
    void (*transport_close)(JSRuntime *rt, void *udata);
    void *transport_udata;

    JSValue breakpoints; /* { "<engine path>": { breakpoints: [{line,column}], dirty: N } } */
    int exception_breakpoint;
    uint32_t breakpoints_dirty_counter;
    int stepping;                /* one of JS_DEBUGGER_STEP_*; 0 = running */
    JSDebuggerLocation step_over; /* location the current step must escape */
    int step_depth;               /* stack depth when the step was issued */
} JSDebuggerInfo;

/* ---- host-facing API ---------------------------------------------------- */

/* Attach a connected transport. Sends stopped("entry") and BLOCKS in the
   message loop until the client sends continue — this is the --debug-wait
   handshake (the client sets breakpoints while the engine sits at entry).
   Replaces any previous transport. The four callbacks:
     read  — blocking read, returns bytes read, 0/negative on EOF/error
     write — blocking write, returns bytes written, 0/negative on error
     peek  — non-blocking readability probe: >0 data, 0 none, <0 error
     close — release the transport; called exactly once, from js_debugger_free */
void js_debugger_attach(
    JSContext *ctx,
    size_t (*transport_read)(void *udata, char *buffer, size_t length),
    size_t (*transport_write)(void *udata, const char *buffer, size_t length),
    size_t (*transport_peek)(void *udata),
    void (*transport_close)(JSRuntime *rt, void *udata),
    void *udata);

/* Detach and tear down: emits a terminated event (raw write — no JS allocation,
   safe during runtime teardown), closes the transport, frees debugger state.
   Safe to call when never attached (no-op). */
void js_debugger_free(JSRuntime *rt, JSDebuggerInfo *info);

/* Ask the engine to service the transport at the next opcode check instead of
   waiting out the peek interval. Call once per host frame so client messages
   (new breakpoints, pause) land with bounded latency. */
void js_debugger_cooperate(JSContext *ctx);

int js_debugger_is_transport_connected(JSRuntime *rt);

JSDebuggerInfo *js_debugger_info(JSRuntime *rt);

/* ---- anchor-facing API (called from the quickjs.c anchors) -------------- */

void js_debugger_new_context(JSContext *ctx);   /* A5: ThreadEvent "new" */
void js_debugger_free_context(JSContext *ctx);  /* A6: ThreadEvent "exited" */
void js_debugger_check(JSContext *ctx, const uint8_t *pc); /* A8/A9: per-opcode + per-call */
void js_debugger_exception(JSContext *ctx);     /* A7: stop-on-exception */
/* A12: called per emitted op in the compiler's final pass so every statement
   position lands in pc2line (upstream only records at selected sites, which
   loses `return x;`-style lines entirely). Debug builds trade a slightly
   larger pc2line table for bindable breakpoints; release keeps upstream's. */
struct JSFunctionDef;
void js_debugger_pc2line_every_op(struct JSFunctionDef *s, uint32_t pc, int line_num, int col_num);

/* ---- internals implemented in quickjs-debugger.c (need quickjs.c statics) */

uint32_t js_debugger_stack_depth(JSContext *ctx);
JSValue js_debugger_build_backtrace(JSContext *ctx, const uint8_t *cur_pc);
JSDebuggerLocation js_debugger_current_location(JSContext *ctx, const uint8_t *cur_pc);
/* Breakpoint map lookup (and lazy rebuild) for the function at the top of
   the stack. Line breakpoints mark every pc segment of the line; a breakpoint
   with column > 0 (1-based) marks only segments at/after that column —
   column-accurate inline breakpoints, possible because quickjs-ng's pc2line
   carries a column per entry. */
int js_debugger_check_breakpoint(JSContext *ctx, uint32_t current_dirty, const uint8_t *cur_pc);
JSValue js_debugger_file_breakpoints(JSContext *ctx, const char *path);
JSValue js_debugger_local_variables(JSContext *ctx, int stack_index);
JSValue js_debugger_closure_variables(JSContext *ctx, int stack_index);
/* Evaluate an expression with the lexical scope of an arbitrary stack frame
   (JS_Eval* only reach the top frame). Runs the real parser as a direct eval
   whose closure captures every lexical var of that frame. */
JSValue js_debugger_evaluate(JSContext *ctx, int stack_index, JSValue expression);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_DEBUGGER_H */
