/*
 * quickjs-aot.c — TNR AOT extension (fork patch; threejs-native-runtime plan §5).
 *
 * NOT A STANDALONE TRANSLATION UNIT: this file is textually #included at the
 * very end of quickjs.c. The helpers here are the interpreter case bodies and
 * need quickjs.c's file-local internals (find_own_property, js_add_slow,
 * set_value, close_var_refs, opcode_info, ...). Including instead of exporting
 * keeps the upstream merge surface of quickjs.c to three tiny anchors:
 *   1. the aot_func field on JSFunctionBytecode
 *   2. the dispatch hook at the top of JS_CallInternal
 *   3. the #include "quickjs-aot.c" at the end of quickjs.c
 * Everything else lives here. Public declarations: quickjs.h (§5.4 block) and
 * quickjs-aot.h (the generated-twin contract).
 */

/* ------------------------------------------------------------------------------------
   TNR AOT extension implementation (fork patch; threejs-native-runtime plan §5.4).

   See the doctrine note in quickjs.h next to JSAOTFunc. The pieces here:

   * JS_AOTFunctionHash — content identity of ONE function: FNV-1a 64 over the
     function "shape" (arg/var/stack counts, flags) + the opcode stream with ATOM
     OPERANDS NORMALIZED to their string payloads. Atom indices are process-local
     (the bytecode reader remaps them into the runtime's atom table), so raw
     stream bytes never match between tnr-aotc's compile and the runtime's
     JS_ReadObject — the atom STRINGS do.
     Constant-pool VALUES are deliberately NOT hashed: a generated twin reads
     cpool/atom/var_ref operands through the runtime JSFunctionBytecode it is
     attached to, so two functions with identical streams but different constants
     share one (equally correct) twin. Collisions only matter across DIFFERENT
     opcode streams; 64-bit FNV over ~10^4 bundle functions is comfortable.
     Byte order: payloads are hashed as memory bytes — all supported targets are
     little-endian; revisit if that ever changes.

   * JS_AOTEnumFunctions — depth-first walk over cpool-reachable child functions.

   * JS_AOTInstallTable — bsearch per visited function; TNR_NO_AOT=1 disables
     (the differential-testing switch: same binary, interpreter-only).
 */

#define TNR_AOT_FNV_INIT 0xcbf29ce484222325ULL

static uint64_t tnr_aot_fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t tnr_aot_hash_u32(uint64_t h, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return tnr_aot_fnv1a(h, b, 4);
}

static uint64_t tnr_aot_hash_atom(uint64_t h, JSRuntime *rt, JSAtom atom)
{
    if (atom == JS_ATOM_NULL)
        return tnr_aot_fnv1a(h, "\0N", 2);
    if (__JS_AtomIsTaggedInt(atom)) {
        h = tnr_aot_fnv1a(h, "\0I", 2);
        return tnr_aot_hash_u32(h, __JS_AtomToUInt32(atom));
    }
    JSAtomStruct *p = rt->atom_array[atom];
    h = tnr_aot_fnv1a(h, "\0A", 2);
    h = tnr_aot_hash_u32(h, p->len);
    if (p->is_wide_char)
        return tnr_aot_fnv1a(h, str16(p), (size_t)p->len * 2);
    return tnr_aot_fnv1a(h, str8(p), p->len);
}

uint64_t JS_AOTFunctionHash(JSContext *ctx, JSFunctionBytecode *b)
{
    JSRuntime *rt = ctx->rt;
    uint64_t h = TNR_AOT_FNV_INIT;

    /* shape */
    h = tnr_aot_hash_u32(h, b->arg_count);
    h = tnr_aot_hash_u32(h, b->var_count);
    h = tnr_aot_hash_u32(h, b->defined_arg_count);
    h = tnr_aot_hash_u32(h, b->stack_size);
    h = tnr_aot_hash_u32(h, b->closure_var_count);
    h = tnr_aot_hash_u32(h, b->cpool_count);
    h = tnr_aot_hash_u32(h, (uint32_t)b->byte_code_len);
    h = tnr_aot_hash_u32(h, (uint32_t)(b->is_strict_mode |
                                       (b->func_kind << 1) |
                                       (b->has_prototype << 3) |
                                       (b->has_simple_parameter_list << 4) |
                                       (b->is_derived_class_constructor << 5) |
                                       (b->new_target_allowed << 6) |
                                       (b->super_call_allowed << 7) |
                                       (b->super_allowed << 8) |
                                       (b->arguments_allowed << 9)));
    h = tnr_aot_hash_atom(h, rt, b->func_name);

    /* opcode stream, atoms normalized */
    {
        const uint8_t *pc = b->byte_code_buf, *end = pc + b->byte_code_len;
        while (pc < end) {
            uint8_t op = *pc;
            const JSOpCode *oi = &short_opcode_info(op);
            int size = oi->size;
            if (size < 1 || pc + size > end)
                break; /* malformed stream — hash what we saw */
            h = tnr_aot_fnv1a(h, &op, 1);
            switch (oi->fmt) {
            case OP_FMT_atom:
            case OP_FMT_atom_u8:
            case OP_FMT_atom_u16:
            case OP_FMT_atom_label_u8:
            case OP_FMT_atom_label_u16:
                /* u32 atom at pc+1; hash its payload, then the rest raw */
                h = tnr_aot_hash_atom(h, rt, get_u32(pc + 1));
                h = tnr_aot_fnv1a(h, pc + 5, size - 5);
                break;
            default:
                h = tnr_aot_fnv1a(h, pc + 1, size - 1);
                break;
            }
            pc += size;
        }
    }
    return h;
}

static int tnr_aot_walk(JSContext *ctx, JSFunctionBytecode *b,
                        JSAOTEnumFunc cb, void *ud)
{
    int i, n = 1;
    cb(ud, ctx, b, JS_AOTFunctionHash(ctx, b));
    for (i = 0; i < b->cpool_count; i++) {
        if (JS_VALUE_GET_TAG(b->cpool[i]) == JS_TAG_FUNCTION_BYTECODE)
            n += tnr_aot_walk(ctx, JS_VALUE_GET_PTR(b->cpool[i]), cb, ud);
    }
    return n;
}

/* Accept every shape a "function tree root" shows up in: the value JS_ReadObject/
   JS_Eval(COMPILE_ONLY) returns (module or bare bytecode), or a live function. */
static JSFunctionBytecode *tnr_aot_root_bytecode(JSValueConst root)
{
    switch (JS_VALUE_GET_TAG(root)) {
    case JS_TAG_FUNCTION_BYTECODE:
        return JS_VALUE_GET_PTR(root);
    case JS_TAG_MODULE: {
        JSModuleDef *m = JS_VALUE_GET_PTR(root);
        return tnr_aot_root_bytecode(m->func_obj);
    }
    case JS_TAG_OBJECT: {
        JSObject *p = JS_VALUE_GET_OBJ(root);
        if (p->class_id == JS_CLASS_BYTECODE_FUNCTION)
            return p->u.func.function_bytecode;
        return NULL;
    }
    default:
        return NULL;
    }
}

int JS_AOTEnumFunctions(JSContext *ctx, JSValueConst root, JSAOTEnumFunc cb, void *ud)
{
    JSFunctionBytecode *b = tnr_aot_root_bytecode(root);
    if (!b)
        return -1;
    return tnr_aot_walk(ctx, b, cb, ud);
}

typedef struct {
    const JSAOTEntry *table;
    size_t count;
    int installed;
} TnrAotInstallCtx;

static void tnr_aot_install_cb(void *ud, JSContext *ctx, JSFunctionBytecode *b,
                               uint64_t fn_hash)
{
    TnrAotInstallCtx *ic = ud;
    size_t lo = 0, hi = ic->count;
    (void)ctx;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ic->table[mid].fn_hash < fn_hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < ic->count && ic->table[lo].fn_hash == fn_hash) {
        b->aot_func = ic->table[lo].fn;
        ic->installed++;
    }
}

int JS_AOTInstallTable(JSContext *ctx, JSValueConst root,
                       const JSAOTEntry *table, size_t count)
{
    TnrAotInstallCtx ic = { table, count, 0 };
    const char *no_aot = getenv("TNR_NO_AOT");
    if (no_aot && no_aot[0] && no_aot[0] != '0')
        return 0;
    if (!table || !count)
        return 0;
    if (JS_AOTEnumFunctions(ctx, root, tnr_aot_install_cb, &ic) < 0)
        return -1;
    return ic.installed;
}

const uint8_t *JS_AOTGetBytecode(const JSFunctionBytecode *b, int *plen)
{
    if (plen)
        *plen = b->byte_code_len;
    return b->byte_code_buf;
}

void JS_AOTGetShape(const JSFunctionBytecode *b, JSAOTShape *shape)
{
    shape->arg_count = b->arg_count;
    shape->var_count = b->var_count;
    shape->defined_arg_count = b->defined_arg_count;
    shape->stack_size = b->stack_size;
    shape->var_ref_count = b->var_ref_count;
    shape->closure_var_count = b->closure_var_count;
    shape->cpool_count = b->cpool_count;
    shape->flags = (b->is_strict_mode ? JS_AOT_SHAPE_STRICT : 0) |
                   ((b->func_kind << 1) & JS_AOT_SHAPE_FUNC_KIND) |
                   (b->arguments_allowed ? JS_AOT_SHAPE_ARGUMENTS : 0) |
                   (b->is_derived_class_constructor ? JS_AOT_SHAPE_DERIVED_CTOR : 0);
}

const char *JS_AOTGetFuncName(JSContext *ctx, const JSFunctionBytecode *b)
{
    if (b->func_name == JS_ATOM_NULL)
        return NULL;
    return JS_AtomToCString(ctx, b->func_name);
}

/* ------------------------------------------------------------------------------------
   TNR AOT twin runtime support (fork patch; plan §5.3) — the JS_AOT* surface that
   tnr-aotc-generated C functions ("twins") link against. Every JS_AOTOp* body is
   the corresponding interpreter case body, verbatim where possible: same fast
   paths, same slow calls, same refcount discipline, same sf->cur_pc updates.
   When editing an interpreter case, mirror the change here — the differential
   harness (AOT vs TNR_NO_AOT=1) is the guard.
 */
#include "quickjs-aot.h"

_Static_assert(sizeof(JSStackFrame) <= JS_AOT_FRAME_SIZE,
               "bump JS_AOT_FRAME_SIZE in quickjs-aot.h");

JSContext *JS_AOTFrameEnter(JSContext *caller_ctx, JSAOTFrame *frame,
                            JSFunctionBytecode *b, JSValueConst func_obj,
                            JSValueConst this_obj, JSValueConst new_target,
                            int argc, JSValueConst *argv,
                            JSValue *locals, JSVarRef **frame_var_refs,
                            JSValue **parg_buf)
{
    JSRuntime *rt = caller_ctx->rt;
    JSStackFrame *sf = (JSStackFrame *)frame;
    int i, n;
    (void)this_obj;
    /* upstream a3f1b38 added sf->is_constructor (CallSite.prototype
       .isConstructor). The interpreter sets it from JS_CALL_FLAG_CONSTRUCTOR
       AFTER the twin-dispatch branch returns, so twins must set it here: a
       non-undefined new_target IS the constructor signal (derived-class ctors
       carry super opcodes the translator rejects, so twins only ever see the
       plain-ctor case). */
    sf->is_constructor = !JS_IsUndefined(new_target);
    /* twin locals live on ITS C stack; mirror the interpreter's alloca guard with
       the same size accounting so recursion limits behave identically */
    size_t alloca_size = sizeof(JSValue) * ((size_t)b->arg_count + b->var_count +
                                            b->stack_size) +
                         sizeof(JSVarRef *) * b->var_ref_count;
    if (js_check_stack_overflow(rt, alloca_size)) {
        JS_ThrowStackOverflow(caller_ctx);
        return NULL;
    }
    sf->is_strict_mode = b->is_strict_mode;
    sf->cur_func = unsafe_unconst(func_obj);
    /* interpreter behavior, kept exactly: alias the caller's argv when every
       declared arg is present (put_arg then writes caller stack slots — the
       caller frees them); copy into the twin's locals only when argc falls
       short. The dispatch hook excludes JS_CALL_FLAG_COPY_ARGV calls, so
       aliasing is always legal here. */
    if (argc >= b->arg_count) {
        sf->arg_buf = (JSValue *)argv;
        sf->arg_count = argc;
    } else {
        n = min_int(argc, b->arg_count);
        for (i = 0; i < n; i++)
            locals[i] = js_dup(argv[i]);
        for (; i < b->arg_count; i++)
            locals[i] = JS_UNDEFINED;
        sf->arg_count = b->arg_count;
        sf->arg_buf = locals;
    }
    *parg_buf = sf->arg_buf;
    sf->var_buf = locals + b->arg_count;
    for (i = 0; i < b->var_count; i++)
        sf->var_buf[i] = JS_UNDEFINED;
    sf->var_refs = frame_var_refs;
    sf->var_ref_count = b->var_ref_count;
    for (i = 0; i < b->var_ref_count; i++)
        frame_var_refs[i] = NULL;
    sf->cur_pc = NULL;
    sf->cur_sp = NULL;
    sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = sf;
    return b->realm;
}

void JS_AOTFrameLeave(JSContext *ctx, JSAOTFrame *frame, JSFunctionBytecode *b,
                      JSValue *locals, JSValue *sp)
{
    JSStackFrame *sf = (JSStackFrame *)frame;
    JSRuntime *rt = ctx->rt;
    JSValue *pval;
    /* interpreter 'done:' order, kept verbatim: close_var_ref DUPS the live
       stack value, so closing must happen BEFORE the frees. When args were
       aliased from the caller (arg_buf != locals) they are the caller's to
       free — start at var_buf, exactly like the interpreter's non-allocated
       arg case. */
    if (unlikely(b->var_ref_count != 0))
        close_var_refs(rt, sf);
    pval = (sf->arg_buf == locals) ? locals : locals + b->arg_count;
    for (; pval < sp; pval++)
        JS_FreeValue(ctx, *pval);
    rt->current_stack_frame = sf->prev_frame;
}

uint8_t **JS_AOTFramePCSlot(JSAOTFrame *frame)
{
    return &((JSStackFrame *)frame)->cur_pc;
}

int JS_AOTPoll(JSContext *ctx)
{
    return js_poll_interrupts(ctx) ? -1 : 0;
}

JSValue JS_AOTCpool(JSFunctionBytecode *b, int idx)
{
    return b->cpool[idx];
}

JSValue JS_AOTVarRefGet(JSVarRef **var_refs, int idx)
{
    return js_dup(*var_refs[idx]->pvalue);
}

void JS_AOTVarRefPut(JSContext *ctx, JSVarRef **var_refs, int idx, JSValue v)
{
    set_value(ctx, var_refs[idx]->pvalue, v);
}

#define TNR_SF ((JSStackFrame *)frame)

int JS_AOTOpAddSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                    const uint8_t *next_pc)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_add_slow(ctx, *psp))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpArithSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc, int op)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_binary_arith_slow(ctx, *psp, op))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpUnarySlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc, int op)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    return js_unary_arith_slow(ctx, *psp, op) ? -1 : 0;
}

int JS_AOTOpNotSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                    const uint8_t *next_pc)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    return js_not_slow(ctx, *psp) ? -1 : 0;
}

int JS_AOTOpLogicSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc, int op)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_binary_logic_slow(ctx, *psp, op))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpShrSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                    const uint8_t *next_pc)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_shr_slow(ctx, *psp))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpCmpSlow(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                    const uint8_t *next_pc, int op)
{
    int ret;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    switch (op) {
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        ret = js_relational_slow(ctx, *psp, op);
        break;
    case OP_eq:
        ret = js_eq_slow(ctx, *psp, 0);
        break;
    case OP_neq:
        ret = js_eq_slow(ctx, *psp, 1);
        break;
    case OP_strict_eq:
        ret = js_strict_eq_slow(ctx, *psp, 0);
        break;
    case OP_strict_neq:
        ret = js_strict_eq_slow(ctx, *psp, 1);
        break;
    default:
        JS_ThrowInternalError(ctx, "AOT: bad cmp op %d", op);
        return -1;
    }
    if (ret)
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpPostIncDec(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc, int op)
{
    JSValue *sp = *psp;
    JSValue op1 = sp[-1];
    if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
        int val = JS_VALUE_GET_INT(op1);
        if (op == OP_post_inc) {
            if (unlikely(val == INT32_MAX))
                goto slow;
            sp[0] = js_int32(val + 1);
        } else {
            if (unlikely(val == INT32_MIN))
                goto slow;
            sp[0] = js_int32(val - 1);
        }
    } else {
    slow:
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        if (js_post_inc_slow(ctx, sp, op))
            return -1;
    }
    *psp = sp + 1;
    return 0;
}

int JS_AOTOpIncDecLoc(JSContext *ctx, JSAOTFrame *frame, JSValue *var_buf,
                      int idx, const uint8_t *next_pc, int op)
{
    JSValue op1 = var_buf[idx];
    if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
        int val = JS_VALUE_GET_INT(op1);
        if (op == OP_inc_loc) {
            if (unlikely(val == INT32_MAX))
                goto slow;
            var_buf[idx] = js_int32(val + 1);
        } else {
            if (unlikely(val == INT32_MIN))
                goto slow;
            var_buf[idx] = js_int32(val - 1);
        }
    } else {
    slow:
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        /* must duplicate otherwise the variable value may be destroyed before
           JS code accesses it (interpreter comment, kept verbatim) */
        op1 = js_dup(op1);
        if (js_unary_arith_slow(ctx, &op1 + 1, op == OP_inc_loc ? OP_inc : OP_dec))
            return -1;
        set_value(ctx, &var_buf[idx], op1);
    }
    return 0;
}

int JS_AOTOpAddLoc(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                   JSValue *var_buf, int idx, const uint8_t *next_pc)
{
    JSValue *sp = *psp;
    JSValue *pv = &var_buf[idx];
    if (likely(JS_VALUE_IS_BOTH_INT(*pv, sp[-1]))) {
        int64_t r = (int64_t)JS_VALUE_GET_INT(*pv) + JS_VALUE_GET_INT(sp[-1]);
        if (unlikely((int)r != r))
            *pv = __JS_NewFloat64((double)r);
        else
            *pv = js_int32((int)r);
        sp--;
    } else if (JS_VALUE_GET_TAG(*pv) == JS_TAG_STRING) {
        JSValue op1 = sp[-1];
        sp--;
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        op1 = JS_ToPrimitiveFree(ctx, op1, HINT_NONE);
        if (JS_IsException(op1)) {
            *psp = sp;
            return -1;
        }
        op1 = JS_ConcatString(ctx, js_dup(*pv), op1);
        if (JS_IsException(op1)) {
            *psp = sp;
            return -1;
        }
        set_value(ctx, pv, op1);
    } else {
        JSValue ops[2];
        /* in case of exception, js_add_slow frees ops[0] and ops[1], so we must
           duplicate *pv (interpreter comment, kept verbatim) */
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        ops[0] = js_dup(*pv);
        ops[1] = sp[-1];
        sp--;
        if (js_add_slow(ctx, ops + 2)) {
            *psp = sp;
            return -1;
        }
        set_value(ctx, pv, ops[0]);
    }
    *psp = sp;
    return 0;
}

int JS_AOTOpGetField(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                     const uint8_t *next_pc, JSAtom atom)
{
    JSValue *sp = *psp;
    JSValue val, obj;
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;

    obj = sp[-1];
    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
        p = JS_VALUE_GET_OBJ(obj);
        for (;;) {
            prs = find_own_property(&pr, p, atom);
            if (prs) {
                if (unlikely(prs->flags & JS_PROP_TMASK))
                    goto slow_path;
                val = js_dup(pr->u.value);
                break;
            }
            if (unlikely(p->is_exotic)) {
                obj = JS_MKPTR(JS_TAG_OBJECT, p);
                goto slow_path;
            }
            p = p->shape->proto;
            if (!p) {
                val = JS_UNDEFINED;
                break;
            }
        }
    } else {
    slow_path:
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        val = JS_GetPropertyInternal(ctx, obj, atom, sp[-1], false);
        if (unlikely(JS_IsException(val)))
            return -1;
    }
    JS_FreeValue(ctx, sp[-1]);
    sp[-1] = val;
    return 0;
}

int JS_AOTOpGetField2(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc, JSAtom atom)
{
    JSValue *sp = *psp;
    JSValue val, obj;
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;

    obj = sp[-1];
    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
        p = JS_VALUE_GET_OBJ(obj);
        for (;;) {
            prs = find_own_property(&pr, p, atom);
            if (prs) {
                if (unlikely(prs->flags & JS_PROP_TMASK))
                    goto slow_path;
                val = js_dup(pr->u.value);
                break;
            }
            if (unlikely(p->is_exotic)) {
                obj = JS_MKPTR(JS_TAG_OBJECT, p);
                goto slow_path;
            }
            p = p->shape->proto;
            if (!p) {
                val = JS_UNDEFINED;
                break;
            }
        }
    } else {
    slow_path:
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        val = JS_GetPropertyInternal(ctx, obj, atom, sp[-1], false);
        if (unlikely(JS_IsException(val)))
            return -1;
    }
    *sp++ = val;
    *psp = sp;
    return 0;
}

int JS_AOTOpPutField(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                     const uint8_t *next_pc, JSAtom atom)
{
    JSValue *sp = *psp;
    JSValue obj = sp[-2];
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;
    int ret;

    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
        p = JS_VALUE_GET_OBJ(obj);
        prs = find_own_property(&pr, p, atom);
        if (!prs)
            goto slow_path;
        if (likely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE |
                                  JS_PROP_LENGTH)) == JS_PROP_WRITABLE)) {
            set_value(ctx, &pr->u.value, sp[-1]);
        } else {
            goto slow_path;
        }
        JS_FreeValue(ctx, obj);
        *psp = sp - 2;
    } else {
    slow_path:
        TNR_SF->cur_pc = (uint8_t *)next_pc;
        ret = JS_SetPropertyInternal2(ctx, obj, atom, sp[-1], obj,
                                      JS_PROP_THROW_STRICT);
        JS_FreeValue(ctx, obj);
        *psp = sp - 2;
        if (unlikely(ret < 0))
            return -1;
    }
    return 0;
}

int JS_AOTOpGetArrayEl(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc)
{
    JSValue *sp = *psp;
    JSValue val;
    /* fast-array + int-index arm, hoisted from js_get_fast_array_element's
       JS_CLASS_ARRAY case: the interpreter routes every element read through
       JS_GetPropertyValue — twins skip both call levels. te[i] in three.js
       matrix code is exactly this shape, tens of millions of hits per bench. */
    if (likely(JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT &&
               JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_OBJECT)) {
        JSObject *p = JS_VALUE_GET_OBJ(sp[-2]);
        uint32_t idx = (uint32_t)JS_VALUE_GET_INT(sp[-1]);
        if (likely(p->class_id == JS_CLASS_ARRAY && p->fast_array &&
                   idx < (uint32_t)p->u.array.count)) {
            val = js_dup(p->u.array.u.values[idx]);
            JS_FreeValue(ctx, sp[-2]);
            sp[-2] = val;
            *psp = sp - 1;
            return 0;
        }
    }
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    val = JS_GetPropertyValue(ctx, sp[-2], sp[-1]);
    JS_FreeValue(ctx, sp[-2]);
    sp[-2] = val;
    *psp = sp - 1;
    return unlikely(JS_IsException(val)) ? -1 : 0;
}

int JS_AOTOpPutArrayEl(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc)
{
    JSValue *sp = *psp;
    JSValue val = sp[-1];
    uint32_t idx;
    JSObject *p;
    int ret;

    if (likely(JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_INT)) {
        idx = JS_VALUE_GET_INT(sp[-2]);
        if (likely(JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_OBJECT)) {
            p = JS_VALUE_GET_OBJ(sp[-3]);
            if (likely(p->class_id == JS_CLASS_ARRAY &&
                       idx < (uint32_t)p->u.array.count)) {
                set_value(ctx, &p->u.array.u.values[idx], val);
                JS_FreeValue(ctx, sp[-3]);
                *psp = sp - 3;
                return 0;
            }
            if (likely(p->class_id == JS_CLASS_ARRAY &&
                       idx == (uint32_t)p->u.array.count &&
                       p->fast_array &&
                       p->extensible &&
                       p->shape->proto == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]) &&
                       ctx->std_array_prototype)) {
                uint32_t array_len;
                if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
                    uint32_t new_len = idx + 1;
                    array_len = JS_VALUE_GET_INT(p->prop[0].u.value);
                    if (likely(new_len <= p->u.array.u1.size)) {
                        p->u.array.u.values[idx] = val;
                        p->u.array.count = new_len;
                        if (new_len > array_len)
                            p->prop[0].u.value = js_int32(new_len);
                        JS_FreeValue(ctx, sp[-3]);
                        *psp = sp - 3;
                        return 0;
                    }
                }
            }
        }
    }
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    ret = JS_SetPropertyValue(ctx, sp[-3], sp[-2], sp[-1], JS_PROP_THROW_STRICT);
    JS_FreeValue(ctx, sp[-3]);
    *psp = sp - 3;
    return unlikely(ret < 0) ? -1 : 0;
}

int JS_AOTOpGetVar(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                   const uint8_t *next_pc, JSAtom atom, int undef_ok)
{
    JSValue val;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    val = JS_GetGlobalVar(ctx, atom, undef_ok);
    if (unlikely(JS_IsException(val)))
        return -1;
    *(*psp)++ = val;
    return 0;
}

int JS_AOTOpPutVar(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                   const uint8_t *next_pc, JSAtom atom, int is_init)
{
    int ret;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    ret = JS_SetGlobalVar(ctx, atom, (*psp)[-1], is_init);
    (*psp)--;
    return unlikely(ret < 0) ? -1 : 0;
}

/* Direct twin->twin dispatch (v2): when the callee is a plain bytecode
   function that HAS a twin, skip JS_CallInternal's whole prologue (tag/class
   walk, arg-alloc decision, dispatch-table entry) and invoke the twin function
   pointer. Semantics preserved: the interrupt poll happens exactly where
   JS_CallInternal would poll, and everything else IS the twin's FrameEnter. */
static inline JSValue tnr_aot_call_dispatch(JSContext *ctx, JSValueConst func_obj,
                                                  JSValueConst this_obj, int argc,
                                                  JSValueConst *argv)
{
    if (likely(JS_VALUE_GET_TAG(func_obj) == JS_TAG_OBJECT)) {
        JSObject *fp = JS_VALUE_GET_OBJ(func_obj);
        if (fp->class_id == JS_CLASS_BYTECODE_FUNCTION) {
            JSFunctionBytecode *cb = fp->u.func.function_bytecode;
            if (cb->aot_func != NULL) {
                if (unlikely(js_poll_interrupts(ctx)))
                    return JS_EXCEPTION;
                return cb->aot_func(ctx, func_obj, this_obj, JS_UNDEFINED,
                                    argc, argv, cb, fp->u.func.var_refs);
            }
        }
    }
    return JS_CallInternal(ctx, func_obj, this_obj, JS_UNDEFINED, argc, argv, 0);
}

int JS_AOTOpCall(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                 const uint8_t *next_pc, int argc, int method)
{
    JSValue *sp = *psp;
    JSValue *call_argv = sp - argc;
    JSValue ret_val;
    int i;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    ret_val = tnr_aot_call_dispatch(ctx, call_argv[-1],
                                    method ? call_argv[-2] : JS_UNDEFINED,
                                    argc, vc(call_argv));
    if (unlikely(JS_IsException(ret_val)))
        return -1;
    for (i = -1 - method; i < argc; i++)
        JS_FreeValue(ctx, call_argv[i]);
    sp -= argc + 1 + method;
    *sp++ = ret_val;
    *psp = sp;
    return 0;
}

int JS_AOTOpTailCall(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                     const uint8_t *next_pc, int argc, int method, JSValue *pret)
{
    JSValue *sp = *psp;
    JSValue *call_argv = sp - argc;
    JSValue ret_val;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    ret_val = tnr_aot_call_dispatch(ctx, call_argv[-1],
                                    method ? call_argv[-2] : JS_UNDEFINED,
                                    argc, vc(call_argv));
    if (unlikely(JS_IsException(ret_val)))
        return -1;
    /* the interpreter jumps straight to done: the twin frees the whole live
       stack (args included) on its way out, exactly like the interpreter's
       exit loop over stack_buf..sp */
    *pret = ret_val;
    return 0;
}

int JS_AOTOpCallCtor(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                     const uint8_t *next_pc, int argc)
{
    /* stack: func, new_target, args... — pops argc+2, pushes the result */
    JSValue *sp = *psp;
    JSValue *call_argv = sp - argc;
    JSValue ret_val;
    int i;
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    ret_val = JS_CallConstructorInternal(ctx, call_argv[-2], call_argv[-1],
                                         argc, vc(call_argv), 0);
    if (unlikely(JS_IsException(ret_val)))
        return -1;
    for (i = -2; i < argc; i++)
        JS_FreeValue(ctx, call_argv[i]);
    sp -= argc + 2;
    *sp++ = ret_val;
    *psp = sp;
    return 0;
}

int JS_AOTOpFClosure(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                     JSFunctionBytecode *b, int cpool_idx, JSVarRef **var_refs)
{
    JSValue *sp = *psp;
    JSValue bfunc = js_dup(b->cpool[cpool_idx]);
    *sp++ = js_closure(ctx, bfunc, var_refs, (JSStackFrame *)frame);
    *psp = sp;
    return unlikely(JS_IsException(sp[-1])) ? -1 : 0;
}

int JS_AOTOpPushThis(JSContext *ctx, JSValue **psp, JSValueConst this_obj,
                     int is_strict)
{
    JSValue val;
    if (!is_strict) {
        uint32_t tag = JS_VALUE_GET_TAG(this_obj);
        if (likely(tag == JS_TAG_OBJECT)) {
            val = js_dup(this_obj);
        } else if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED) {
            val = js_dup(ctx->global_obj);
        } else {
            val = JS_ToObject(ctx, this_obj);
            if (JS_IsException(val))
                return -1;
        }
    } else {
        val = js_dup(this_obj);
    }
    *(*psp)++ = val;
    return 0;
}

int JS_AOTOpTypeof(JSContext *ctx, JSValue **psp)
{
    JSValue *sp = *psp;
    JSValue op1 = sp[-1];
    JSAtom atom = js_operator_typeof(ctx, op1);
    JS_FreeValue(ctx, op1);
    sp[-1] = JS_AtomToString(ctx, atom);
    return 0;
}

int JS_AOTOpInstanceof(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_operator_instanceof(ctx, *psp))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpIn(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
               const uint8_t *next_pc)
{
    TNR_SF->cur_pc = (uint8_t *)next_pc;
    if (js_operator_in(ctx, *psp))
        return -1;
    (*psp)--;
    return 0;
}

int JS_AOTOpObject(JSContext *ctx, JSValue **psp)
{
    JSValue v = JS_NewObject(ctx);
    if (unlikely(JS_IsException(v)))
        return -1;
    *(*psp)++ = v;
    return 0;
}

int JS_AOTOpDefineField(JSContext *ctx, JSValue **psp, JSAtom atom)
{
    JSValue *sp = *psp;
    int ret = JS_DefinePropertyValue(ctx, sp[-2], atom, sp[-1],
                                     JS_PROP_C_W_E | JS_PROP_THROW);
    *psp = sp - 1;
    return unlikely(ret < 0) ? -1 : 0;
}

int JS_AOTOpToBoolFree(JSContext *ctx, JSValue v)
{
    return JS_ToBoolFree(ctx, v);
}

int JS_AOTThrowUninit(JSContext *ctx, JSFunctionBytecode *b, int idx, int is_ref)
{
    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, is_ref != 0);
    return -1;
}


/* ---- per-site inline caches (v2) ----------------------------------------------------
   One JSAOTIC per get_field/get_field2/put_field SITE in generated code (the
   generated file defines the array and publishes it through tnr_aot_ic_table).
   Twin execution is single-threaded by construction (workers never load .qbc),
   so no atomics.

   Correctness model — a hit must be right even with quickjs's in-place shape
   mutation (unique shapes append properties without changing the pointer):
   * OWN hit: receiver shape ptr match + shape->prop[idx].atom == atom +
     value-prop flags. In-place APPEND keeps idx/atom stable; DELETE compaction
     moves props but then the atom check misses. Safe for unique shapes too.
   * PROTO hit (method loads): additionally requires the RECEIVER shape to be
     SHARED (is_hashed) — a unique receiver could gain a shadowing property
     in place without changing its shape pointer. Shared shapes change pointer
     on every own-layout change, and shape identity pins sh->proto, so the
     holder (the direct proto) is pinned too; the holder's own layout is
     guarded by its shape ptr + atom recheck. Only depth-1 holders are cached —
     deeper chains would need per-link guards.
   * Cached shape/holder pointers hold REAL references (js_dup_shape / js_dup),
     released on replace and at JS_AOTResetICs (engine dispose) — no ABA.
 */
struct JSAOTIC {
    JSShape *rshape;    /* receiver shape guard (ref held); NULL = empty */
    JSShape *hshape;    /* proto hit: holder shape guard (ref held); NULL = own hit */
    JSObject *holder;   /* proto hit: the holder object (ref held) */
    uint32_t idx;       /* property index in the holder's shape/prop array */
};

void JS_AOTResetICs(JSRuntime *rt, JSAOTIC *ics, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (ics[i].rshape)
            js_free_shape(rt, ics[i].rshape);
        if (ics[i].hshape)
            js_free_shape(rt, ics[i].hshape);
        if (ics[i].holder)
            JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ics[i].holder));
        memset(&ics[i], 0, sizeof ics[i]);
    }
}

static inline int tnr_aot_ic_get_hit(JSContext *ctx, JSAOTIC *ic, JSObject *p,
                                           JSAtom atom, JSValue *pval)
{
    if (p->shape == ic->rshape) {
        JSObject *h = ic->holder ? ic->holder : p;
        JSShape *hsh = h->shape;
        if (ic->hshape == NULL || hsh == ic->hshape) {
            /* upstream moved shape prop descriptors behind get_shape_prop()
               (they now live after the shape's flexible hash_table[]); the
               object's own prop VALUES (h->prop[]) are unchanged. */
            JSShapeProperty *prs = &get_shape_prop(hsh)[ic->idx];
            if (prs->atom == atom && !(prs->flags & JS_PROP_TMASK)) {
                *pval = js_dup(h->prop[ic->idx].u.value);
                return 1;
            }
        }
    }
    return 0;
}

static void tnr_aot_ic_clear(JSContext *ctx, JSAOTIC *ic)
{
    if (ic->rshape) js_free_shape(ctx->rt, ic->rshape);
    if (ic->hshape) js_free_shape(ctx->rt, ic->hshape);
    if (ic->holder) JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ic->holder));
    memset(ic, 0, sizeof *ic);
}

static void tnr_aot_ic_fill(JSContext *ctx, JSAOTIC *ic, JSObject *receiver,
                            JSObject *holder, JSShapeProperty *prs, JSProperty *pr)
{
    /* cache only plain value props; proto hits additionally need a SHARED
       receiver shape (see the correctness model above) */
    uint32_t idx = (uint32_t)(pr - holder->prop);
    if (prs->flags & JS_PROP_TMASK)
        return;
    if (holder != receiver && !receiver->shape->is_hashed)
        return;
    tnr_aot_ic_clear(ctx, ic);
    ic->rshape = js_dup_shape(receiver->shape);
    ic->idx = idx;
    if (holder != receiver) {
        ic->hshape = js_dup_shape(holder->shape);
        ic->holder = holder;
        js_dup(JS_MKPTR(JS_TAG_OBJECT, holder));
    }
}

/* get_field with a cache site: IC hit -> dup value; miss -> the interpreter
   case body, filling the cache when the property resolves to a cacheable slot
   at depth 0 or 1. keep_obj distinguishes OP_get_field2 (push) from
   OP_get_field (replace). */
static inline int tnr_aot_get_field_ic(JSContext *ctx, JSAOTFrame *frame,
                                             JSValue **psp, const uint8_t *next_pc,
                                             JSAtom atom, JSAOTIC *ic, int keep_obj)
{
    JSValue *sp = *psp;
    JSValue val, obj;
    JSObject *p, *receiver;
    JSProperty *pr;
    JSShapeProperty *prs;
    int depth = 0;

    obj = sp[-1];
    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
        p = receiver = JS_VALUE_GET_OBJ(obj);
        if (tnr_aot_ic_get_hit(ctx, ic, p, atom, &val))
            goto have_val;
        for (;;) {
            prs = find_own_property(&pr, p, atom);
            if (prs) {
                if (unlikely(prs->flags & JS_PROP_TMASK))
                    goto slow_path;
                if (depth <= 1)
                    tnr_aot_ic_fill(ctx, ic, receiver, p, prs, pr);
                val = js_dup(pr->u.value);
                break;
            }
            if (unlikely(p->is_exotic)) {
                obj = JS_MKPTR(JS_TAG_OBJECT, p);
                goto slow_path;
            }
            p = p->shape->proto;
            if (!p) {
                val = JS_UNDEFINED;
                break;
            }
            depth++;
        }
    } else {
    slow_path:
        ((JSStackFrame *)frame)->cur_pc = (uint8_t *)next_pc;
        val = JS_GetPropertyInternal(ctx, obj, atom, sp[-1], false);
        if (unlikely(JS_IsException(val)))
            return -1;
    }
have_val:
    if (keep_obj) {
        *sp++ = val;
        *psp = sp;
    } else {
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = val;
    }
    return 0;
}

int JS_AOTOpGetFieldIC(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc, JSAtom atom, JSAOTIC *ic)
{
    return tnr_aot_get_field_ic(ctx, frame, psp, next_pc, atom, ic, 0);
}

int JS_AOTOpGetField2IC(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                        const uint8_t *next_pc, JSAtom atom, JSAOTIC *ic)
{
    return tnr_aot_get_field_ic(ctx, frame, psp, next_pc, atom, ic, 1);
}

/* put_field with a cache site: only OWN writable plain slots are cached. */
int JS_AOTOpPutFieldIC(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                       const uint8_t *next_pc, JSAtom atom, JSAOTIC *ic)
{
    JSValue *sp = *psp;
    JSValue obj = sp[-2];
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;
    int ret;

    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
        p = JS_VALUE_GET_OBJ(obj);
        /* IC hit: own, writable, plain value */
        if (p->shape == ic->rshape && ic->hshape == NULL) {
            prs = &get_shape_prop(p->shape)[ic->idx];
            if (prs->atom == atom &&
                (prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE | JS_PROP_LENGTH)) ==
                    JS_PROP_WRITABLE) {
                set_value(ctx, &p->prop[ic->idx].u.value, sp[-1]);
                JS_FreeValue(ctx, obj);
                *psp = sp - 2;
                return 0;
            }
        }
        prs = find_own_property(&pr, p, atom);
        if (!prs)
            goto slow_path;
        if (likely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE |
                                  JS_PROP_LENGTH)) == JS_PROP_WRITABLE)) {
            if (!(prs->flags & JS_PROP_TMASK))
                tnr_aot_ic_fill(ctx, ic, p, p, prs, pr);
            set_value(ctx, &pr->u.value, sp[-1]);
        } else {
            goto slow_path;
        }
        JS_FreeValue(ctx, obj);
        *psp = sp - 2;
    } else {
    slow_path:
        ((JSStackFrame *)frame)->cur_pc = (uint8_t *)next_pc;
        ret = JS_SetPropertyInternal2(ctx, obj, atom, sp[-1], obj,
                                      JS_PROP_THROW_STRICT);
        JS_FreeValue(ctx, obj);
        *psp = sp - 2;
        if (unlikely(ret < 0))
            return -1;
    }
    return 0;
}

/* The interpreter's `exception:` label attaches the backtrace AT THE THROW-
   ADJACENT FRAME — observable via Error.prepareStackTrace: at stack-overflow
   depth the prepare call itself overflows and is suppressed (quickjs tests
   assert calls == 0, bug904). Twins must do the same at their exception label,
   or the error propagates bare until the first interpreted frame, where the
   unwound stack lets prepare SUCCEED — a semantic divergence the engine-test
   differential caught. cur_pc holds whatever the failing helper set. */
void JS_AOTExceptionBacktrace(JSContext *ctx, JSAOTFrame *frame)
{
    JSRuntime *rt = ctx->rt;
    (void)frame;
    if (needs_backtrace(rt->current_exception) ||
        JS_IsUndefined(ctx->error_back_trace))
        build_backtrace(ctx, rt->current_exception, JS_UNDEFINED, NULL, 0, 0, 0);
}

int JS_AOTThrowNonCtor(JSContext *ctx)
{
    JS_ThrowTypeError(ctx, "class constructors must be invoked with 'new'");
    return -1;
}

int JS_AOTOpPutLocCheckInit(JSContext *ctx, JSValue **psp, JSValue *var_buf, int idx)
{
    if (unlikely(!JS_IsUninitialized(var_buf[idx]))) {
        JS_ThrowReferenceError(ctx, "'this' can be initialized only once");
        return -1;
    }
    set_value(ctx, &var_buf[idx], *--(*psp));
    return 0;
}

int JS_AOTOpPutVarRefCheck(JSContext *ctx, JSValue **psp, JSFunctionBytecode *b,
                           JSVarRef **var_refs, int idx, int is_init)
{
    bool uninit = JS_IsUninitialized(*var_refs[idx]->pvalue);
    if (unlikely(is_init ? !uninit : uninit)) {
        JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, true);
        return -1;
    }
    set_value(ctx, var_refs[idx]->pvalue, *--(*psp));
    return 0;
}

void JS_AOTOpCloseLoc(JSContext *ctx, JSAOTFrame *frame, JSFunctionBytecode *b, int idx)
{
    close_lexical_var(ctx, b, (JSStackFrame *)frame, idx);
}

int JS_AOTOpSetName(JSContext *ctx, JSValue **psp, JSAtom atom)
{
    return JS_DefineObjectName(ctx, (*psp)[-1], atom, JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

int JS_AOTOpArrayFrom(JSContext *ctx, JSValue **psp, int argc)
{
    /* JS_NewArrayFrom takes ownership of the argc values */
    JSValue *sp = *psp;
    JSValue ret = JS_NewArrayFrom(ctx, argc, sp - argc);
    sp -= argc;
    if (unlikely(JS_IsException(ret))) {
        *psp = sp;
        return -1;
    }
    *sp++ = ret;
    *psp = sp;
    return 0;
}

void JS_AOTOpTypeofIs(JSContext *ctx, JSValue **psp, int is_function)
{
    JSValue *sp = *psp;
    JSAtom want = is_function ? JS_ATOM_function : JS_ATOM_undefined;
    int r = js_operator_typeof(ctx, sp[-1]) == want;
    JS_FreeValue(ctx, sp[-1]);
    sp[-1] = r ? JS_TRUE : JS_FALSE;
}

int JS_AOTOpGetLength(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc)
{
    /* OP_get_length is OP_get_field specialized to the fixed JS_ATOM_length —
       a predefined atom, identical in every runtime, so no stream read needed */
    return JS_AOTOpGetField(ctx, frame, psp, next_pc, JS_ATOM_length);
}

int JS_AOTOpToPropKey(JSContext *ctx, JSAOTFrame *frame, JSValue **psp,
                      const uint8_t *next_pc, int check_obj)
{
    JSValue *sp = *psp, ret_val;
    if (check_obj && unlikely(JS_IsUndefined(sp[-2]) || JS_IsNull(sp[-2]))) {
        JS_ThrowTypeError(ctx, "value has no property");
        return -1;
    }
    switch (JS_VALUE_GET_TAG(sp[-1])) {
    case JS_TAG_INT:
    case JS_TAG_STRING:
    case JS_TAG_SYMBOL:
        break;
    default:
        ((JSStackFrame *)frame)->cur_pc = (uint8_t *)next_pc;
        ret_val = JS_ToPropertyKey(ctx, sp[-1]);
        if (JS_IsException(ret_val))
            return -1;
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = ret_val;
        break;
    }
    return 0;
}

#undef TNR_SF

/* ---- v3 typed-region helpers (phase3-aot-v3-typed-ir-plan.md §5/§6.2) --------------
   Support for the UNBOXED double regions tnr-aotc emits: every type/fast-array
   check hoists to REGION ENTRY (before any observable effect); the body then runs
   on C doubles with zero checks; a failed entry guard jumps to the boxed
   re-emission of the same ops. All static inline — twins compile in this TU and
   the optimizer folds these into straight-line loads/stores/fp ops. */

/* Entry guard: plain fast Array with count > max_idx (every index the region
   touches, load or store, is <= max_idx — stores never grow the array). */
static inline int js_aot_arr_fast(JSValue v, uint32_t max_idx)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(v);
    return p->class_id == JS_CLASS_ARRAY && p->fast_array &&
           (uint32_t)p->u.array.count > max_idx;
}
/* Entry guard: element at a const load index is numeric (int|float tag). */
static inline int js_aot_el_isnum(JSValue arr, uint32_t idx)
{
    JSValue e = JS_VALUE_GET_OBJ(arr)->u.array.u.values[idx];
    return JS_AOT_IS_NUM(e);
}
/* Body load, UNCHECKED — legal only after arr_fast + el_isnum entry guards. */
static inline double js_aot_el_getd(JSValue arr, uint32_t idx)
{
    JSValue e = JS_VALUE_GET_OBJ(arr)->u.array.u.values[idx];
    return JS_AOT_NUM(e);
}
/* Variable-index body load, CHECKED (bounds + numeric). Only emitted at
   pre-store positions, where a failure may still restart the region boxed. */
static inline int js_aot_el_getd_chk(JSValue arr, double didx, double *out)
{
    JSObject *p = JS_VALUE_GET_OBJ(arr);
    uint32_t idx = (uint32_t)didx;
    JSValue e;
    if ((double)idx != didx || idx >= (uint32_t)p->u.array.count)
        return 0;
    e = p->u.array.u.values[idx];
    if (!JS_AOT_IS_NUM(e))
        return 0;
    *out = JS_AOT_NUM(e);
    return 1;
}
/* Body store, UNCHECKED — legal only after arr_fast(arr, idx) at entry.
   set_value semantics (free the displaced element); cannot throw. */
static inline void js_aot_el_putd(JSContext *ctx, JSValue arr, uint32_t idx, double d)
{
    JSValue *pe = &JS_VALUE_GET_OBJ(arr)->u.array.u.values[idx];
    JSValue old = *pe;
    *pe = js_aot_float64(d);
    JS_FreeValue(ctx, old);
}
/* Body read of `arr.length` — legal only after js_aot_arr_fast at entry.
   NOT u.array.count: the length property (always prop[0] for JS_CLASS_ARRAY,
   created at birth, non-configurable) may EXCEED count after `a.length = n`.
   It is always a number (int-tagged, or float64 past 2^31), so JS_AOT_NUM
   covers it unchecked. */
static inline double js_aot_arr_len(JSValue arr)
{
    return JS_AOT_NUM(JS_VALUE_GET_OBJ(arr)->prop[0].u.value);
}
/* Borrowed peek at a closure variable — the region-entry guard / body read for
   var-ref bases (v3.5a). Pure pointer load; the cell owns a reference, and no
   call can run mid-region to rebind or free it. */
static inline JSValue js_aot_var_ref_peek(JSVarRef **var_refs, int idx)
{
    return *var_refs[idx]->pvalue;
}
/* Entry guard for NUMERIC-FIELD regions (v3.4): `v` is a plain object
   (JS_CLASS_OBJECT — no exotic get/set behavior) with an own DATA property
   `atom` (not getter/varref/autoinit). Returns the property slot, NULL = deopt.
     need_num:   the region reads the field before writing it, so the current
                 value must already be numeric (the body unboxes check-free);
     need_write: the region stores to it, so it must be writable (a frozen
                 field takes the boxed path for the spec TypeError).
   The returned pointer stays valid for the whole region: the typed body never
   ADDS properties (the only thing that reallocates p->prop / mutates the
   shape), field stores only replace the value in place, and quickjs's GC does
   not move objects. Reads through the slot see in-region stores immediately,
   which is what makes aliased references (this === arg) correct. */
static inline JSValue *js_aot_fld_slot(JSValueConst v, JSAtom atom,
                                       int need_num, int need_write)
{
    JSObject *p;
    JSShapeProperty *prs;
    JSProperty *pr;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if (p->class_id != JS_CLASS_OBJECT)
        return NULL;
    prs = find_own_property(&pr, p, atom);
    if (!prs || (prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
        return NULL;
    if (need_write && !(prs->flags & JS_PROP_WRITABLE))
        return NULL;
    if (need_num && !JS_AOT_IS_NUM(pr->u.value))
        return NULL;
    return &pr->u.value;
}

/* ---- v3.5d Math intrinsics (phase3 §14.4) ------------------------------------------
   Entry guard: the global `Math` binding is pristine (not shadowed by a
   let/const global, a plain own data prop of the global object) and the method
   is the engine's own native — pointer-compared against the same-TU statics.
   Pure reads, hoistable. The typed body then calls the SAME implementation
   functions directly, so results are bit-identical to the interpreter path. */
static inline int js_aot_math_check(JSContext *ctx, JSAtom math_atom,
                                    JSAtom fn_atom, int kind)
{
    JSObject *p;
    JSShapeProperty *prs;
    JSProperty *pr;
    /* JS_GetGlobalVar consults global_var_obj first: a `let Math` shadows */
    if (JS_VALUE_GET_TAG(ctx->global_var_obj) == JS_TAG_OBJECT &&
        find_own_property1(JS_VALUE_GET_OBJ(ctx->global_var_obj), math_atom))
        return 0;
    p = JS_VALUE_GET_OBJ(ctx->global_obj);
    prs = find_own_property(&pr, p, math_atom);
    if (!prs)
        return 0;
    /* quickjs-ng builds Math (and friends) LAZILY: until first access the
       global's prop is JS_PROP_AUTOINIT, which raw find_own_property does not
       realize — so the guard would deopt every intrinsic site in a process
       that hasn't touched Math through the normal property path yet (the v3.6
       fuzzer caught this as a silent all-boxed run). Materialize exactly like
       JS_GetPropertyInternal would, then re-read. */
    if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
        if (JS_AutoInitProperty(ctx, p, math_atom, pr, prs))
            return 0;
        prs = find_own_property(&pr, p, math_atom);
        if (!prs)
            return 0;
    }
    if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
        return 0;
    if (JS_VALUE_GET_TAG(pr->u.value) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(pr->u.value);
    prs = find_own_property(&pr, p, fn_atom);
    if (!prs)
        return 0;
    /* Math's METHODS are lazy too (JS_SetPropertyFunctionList registers
       autoinit props whose cfunctions build on first access) — same
       materialization as the global Math prop above. */
    if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
        if (JS_AutoInitProperty(ctx, p, fn_atom, pr, prs))
            return 0;
        prs = find_own_property(&pr, p, fn_atom);
        if (!prs)
            return 0;
    }
    if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
        return 0;
    if (JS_VALUE_GET_TAG(pr->u.value) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(pr->u.value);
    if (p->class_id != JS_CLASS_C_FUNCTION)
        return 0;
    switch (kind) {
    case JS_AOT_MF_SQRT:
        return p->u.cfunc.cproto == JS_CFUNC_f_f && p->u.cfunc.c_function.f_f == js_math_sqrt;
    case JS_AOT_MF_ABS:
        return p->u.cfunc.cproto == JS_CFUNC_f_f && p->u.cfunc.c_function.f_f == js_math_fabs;
    case JS_AOT_MF_FLOOR:
        return p->u.cfunc.cproto == JS_CFUNC_f_f && p->u.cfunc.c_function.f_f == js_math_floor;
    case JS_AOT_MF_CEIL:
        return p->u.cfunc.cproto == JS_CFUNC_f_f && p->u.cfunc.c_function.f_f == js_math_ceil;
    case JS_AOT_MF_MIN:
        return p->u.cfunc.cproto == JS_CFUNC_generic_magic &&
               p->u.cfunc.c_function.generic_magic == js_math_min_max && p->u.cfunc.magic == 0;
    case JS_AOT_MF_MAX:
        return p->u.cfunc.cproto == JS_CFUNC_generic_magic &&
               p->u.cfunc.c_function.generic_magic == js_math_min_max && p->u.cfunc.magic == 1;
    }
    return 0;
}
/* Body calls — the guarded natives themselves (f_f wrappers over libm). */
static inline double js_aot_math1(int kind, double x)
{
    switch (kind) {
    case JS_AOT_MF_SQRT: return js_math_sqrt(x);
    case JS_AOT_MF_ABS:  return js_math_fabs(x);
    case JS_AOT_MF_FLOOR: return js_math_floor(x);
    default:             return js_math_ceil(x);
    }
}
/* js_math_min_max's double arm, verbatim (NaN propagation; js_fmin/js_fmax
   handle the -0/+0 ordering). The interpreter's int fast path returns an
   int-tagged number instead — value-identical in JS. */
static inline double js_aot_math2(int kind, double r, double a)
{
    if (!isnan(r)) {
        if (isnan(a))
            r = a;
        else
            r = kind == JS_AOT_MF_MAX ? js_fmax(r, a) : js_fmin(r, a);
    }
    return r;
}

/* Twins can be compiled INSIDE this translation unit (cmake passes
   TNR_AOT_GENERATED_C=<tnr-aotc --emit-c output>): every JS_AOTOp* helper above
   is then a same-TU definition the optimizer inlines into the generated code —
   the "unrolled interpreter" of plan §5.3 without exporting any internals.
   Compiled standalone instead, the same generated file still links against the
   extern helpers; it just pays a call per op. */
#ifdef TNR_AOT_GENERATED_C
#include TNR_AOT_GENERATED_C
#endif
