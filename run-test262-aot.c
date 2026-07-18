/*
 * run-test262-aot.c — TNR fork patch: test262 through the AOT production path
 * (threejs-native-runtime phase3 plan §9.2/§15.1).
 *
 * run-test262's eval_buf calls tnr_262_eval() instead of JS_Eval (the one-line
 * anchor). With TNR_AOT_ROUNDTRIP unset this IS JS_Eval — upstream behavior,
 * zero risk. With TNR_AOT_ROUNDTRIP=1 every evaluated buffer takes the exact
 * production twin path instead:
 *
 *   JS_Eval(COMPILE_ONLY) -> JS_WriteObject -> JS_ReadObject   (the .qbc
 *   serialization round-trip that remaps atoms — property-hash binding
 *   depends on it) -> JS_AOTInstallTable -> JS_EvalFunction.
 *
 * The twin table is whatever libqjs was built with (-DTNR_AOT_C=<registry>,
 * else the all-zero stub): tests whose functions hash-match get twins, the
 * rest run interpreted — which is exactly the neutral/divergent discipline
 * the engine suite uses. The v3.6 sweep runs the suite twice with
 * TNR_AOT_ROUNDTRIP=1 — TNR_NO_AOT=1 (round-trip, interp) vs twins on — so
 * the ONLY delta between the runs is twin execution; any report difference
 * is a translator bug.
 *
 * Negative parse tests throw at COMPILE_ONLY exactly where JS_Eval throws.
 * Strict-mode reruns (eval_flags | JS_EVAL_FLAG_STRICT) recompile to
 * different bytecode; if the hash misses the test just runs interpreted —
 * conservative and sound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "quickjs-c-atomics.h"
#include "quickjs.h"
#include "quickjs-aot.h"

/* provided by the TNR_AOT_C registry compiled into libqjs, or the stub */
extern const JSAOTEntry tnr_aot_table[];
extern const size_t tnr_aot_count;

/* Evidence the sweep engaged twins at all: total installs across every eval,
   printed to stderr at exit AND written through to $TNR_AOT_COUNT_FILE every
   512 installs — run-test262 sometimes dies by SIGABRT in teardown (upstream,
   pre-existing), which skips atexit, so the file is the reliable channel.
   tools/test-aot-262.sh gates on it: a 0-install sweep tests nothing.
   (run-test262 runs tests on worker threads, hence the atomics.) */
static atomic_long tnr_262_installed;
static void tnr_262_write_count(long n) {
    const char *path = getenv("TNR_AOT_COUNT_FILE");
    if (!path)
        return;
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld\n", n);
        fclose(f);
    }
}
static void tnr_262_report(void) {
    long n = atomic_load(&tnr_262_installed);
    fprintf(stderr, "[tnr-262] twins installed across suite: %ld\n", n);
    tnr_262_write_count(n);
}
static void tnr_262_account(int installed) {
    if (installed <= 0)
        return;
    long prev = atomic_fetch_add(&tnr_262_installed, installed);
    if (prev / 512 != (prev + installed) / 512) /* periodic durable checkpoint */
        tnr_262_write_count(prev + installed);
}

/* The IC lifecycle contract (quickjs-aot.h): cached shapes/holders hold REAL
   references into their runtime, and the static IC table outlives it — the
   engine must call JS_AOTResetICs before JS_FreeRuntime. run-test262 creates
   a runtime PER TEST, so run_test_buf calls this before teardown (the missing
   call was a heap-use-after-free ASan caught across Iterator/zip tests).
   Corollary: the shared table supports ONE twin-running runtime at a time —
   the twins-on sweep runs -t 1 (interp runs never fill ICs and stay -t N). */
extern JSAOTIC *tnr_aot_ic_table;
extern const size_t tnr_aot_ic_count;

void tnr_262_reset_ics(JSContext *ctx)
{
    const char *rtenv = getenv("TNR_AOT_ROUNDTRIP");
    if (!rtenv || !rtenv[0] || rtenv[0] == '0')
        return;
    if (tnr_aot_ic_table)
        JS_AOTResetICs(JS_GetRuntime(ctx), tnr_aot_ic_table, tnr_aot_ic_count);
}

JSValue tnr_262_eval(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename, int eval_flags)
{
    const char *rtenv = getenv("TNR_AOT_ROUNDTRIP");
    if (!rtenv || !rtenv[0] || rtenv[0] == '0')
        return JS_Eval(ctx, buf, buf_len, filename, eval_flags);

    static atomic_int report_registered;
    if (!atomic_exchange(&report_registered, 1))
        atexit(tnr_262_report);

    /* Twins depend on the .qbc serialization round-trip's ATOM REMAP (the twin
       hash is over atom PAYLOADS, so it survives; the install must run against
       remapped indices). SCRIPTS get the full round-trip — that is exactly
       what product bundles are (esbuild emits one script), so scripts are the
       representative and load-bearing case. MODULES do NOT round-trip:
       JS_ReadObject of a module with imports resolves them EAGERLY through the
       host loader, which re-reads and re-resolves — cyclic imports
       (verify-dfs <-> its FIXTUREs) then re-enter without hitting the module
       cache and run away (upstream read-path 1-byte overread + glibc heap
       abort; ASan). That is upstream's module deserializer, not our twins.
       So modules compile in-place, install twins on the SAME tree (identical
       bytecode -> identical hash -> same twins engage), and run normally —
       full twin coverage, none of the read-path fragility, and no second
       module copy (which is what caused the earlier self-import
       double-registration bug too). */
    int is_module = (eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE;

    if (is_module) {
        JSValue obj = JS_Eval(ctx, buf, buf_len, filename,
                              eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(obj))
            return obj;
        tnr_262_account(JS_AOTInstallTable(ctx, obj, tnr_aot_table, tnr_aot_count));
        if (JS_ResolveModule(ctx, obj) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        return JS_EvalFunction(ctx, obj); /* consumes obj */
    }

    JSValue obj = JS_Eval(ctx, buf, buf_len, filename,
                          eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj))
        return obj;
    size_t bc_len = 0;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);
    if (!bc)
        return JS_EXCEPTION;
    obj = JS_ReadObject(ctx, bc, bc_len, JS_READ_OBJ_BYTECODE);
    js_free(ctx, bc);
    if (JS_IsException(obj))
        return obj;
    tnr_262_account(JS_AOTInstallTable(ctx, obj, tnr_aot_table, tnr_aot_count));
    return JS_EvalFunction(ctx, obj); /* consumes obj */
}
