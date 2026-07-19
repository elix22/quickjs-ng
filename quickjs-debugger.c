/*
 * TNR DEBUGGER extension (fork patch) — implementation.
 *
 * NOT A STANDALONE TRANSLATION UNIT: this file is textually #included at the
 * very end of quickjs.c (anchor A11), after quickjs-aot.c, and only when
 * TNR_QJS_DEBUGGER is defined. That placement is deliberate: the helpers in
 * the "internals" half of this file need quickjs.c file-local statics
 * (find_line_num, js_free_prop_enum, get_func_name, js_class_has_bytecode,
 * the parser/eval plumbing) and internal types (JSStackFrame,
 * JSFunctionBytecode, JSVarDef, JSClosureVar). Including instead of exporting
 * keeps the upstream merge surface to the 11 tagged anchors listed in
 * quickjs-debugger.h.
 *
 * Lineage: port of koush/quickjs's quickjs-debugger.c (MIT) onto quickjs-ng.
 * Deliberate deltas from the original, so a reviewer diffing us against koush
 * knows what is intentional:
 *  - quickjs-ng API drift: find_line_num() takes a column out-param (so
 *    locations and stepping are column-aware); JSFunctionBytecode's debug
 *    fields are flat (b->filename / b->pc2line_buf, no has_debug flag —
 *    NULL pc2line_buf/filename means stripped); js_parse_init and
 *    js_new_function_def grew line/column params; bool replaced BOOL.
 *  - No getenv()-driven lazy connect (QUICKJS_DEBUG_ADDRESS): the host owns
 *    the transport lifecycle explicitly via js_debugger_attach(). Env vars
 *    are unusable on Android anyway (`am start` cannot set them).
 *  - No `this` reconstruction from operand-stack slots: koush read
 *    sf->var_buf[b->var_count] — the first operand-stack slot, which is
 *    uninitialized memory at most pause points. We report no `this` variable
 *    and evaluate with this=undefined until a sound source exists.
 *  - add_closure_variables() is NOT patched (koush added a DEBUG_SCOP_INDEX
 *    mode to it); we carry our own js_debugger_add_closure_variables() here
 *    instead, keeping that upstream function pristine.
 *  - koush's variables handler freed the request `args` object and kept using
 *    it (lived only via the request's refcount); restructured to free at end.
 *  - Protocol read errors detach cleanly instead of assert()ing.
 *
 * Breakpoints (P12.2): per-function byte maps rebuilt lazily via the global
 * dirty counter; the map builder walks quickjs-ng's pc2line encoding (which,
 * unlike Bellard's, appends a column sleb128 to every entry — the drift that
 * broke prior community ports) and supports column-qualified (inline)
 * breakpoints. See js_debugger_check_breakpoint below.
 */

#ifdef TNR_QJS_DEBUGGER

/* ------------------------------------------------------------------------ */
/* transport framing: "<8 hex chars>\n<json>\n", length covers json + '\n'   */
/* ------------------------------------------------------------------------ */

typedef struct DebuggerSuspendedState {
    uint32_t variable_reference_count;
    JSValue variable_references; /* reference -> object (holds refs while paused) */
    JSValue variable_pointers;   /* object ptr hash -> reference (dedup) */
    const uint8_t *cur_pc;
} DebuggerSuspendedState;

/* Transport error convention (size_t is unsigned; no ssize_t on MSVC):
   read/write return the byte count, 0 = EOF/error. peek returns 0 = no data,
   >0 = data pending, (size_t)-1 = error. */
static int js_transport_read_fully(JSDebuggerInfo *info, char *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t received = info->transport_read(info->transport_udata, buffer + offset, length - offset);
        if (received == 0 || received > length - offset)
            return 0;
        offset += received;
    }
    return 1;
}

static int js_transport_write_fully(JSDebuggerInfo *info, const char *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t sent = info->transport_write(info->transport_udata, buffer + offset, length - offset);
        if (sent == 0 || sent > length - offset)
            return 0;
        offset += sent;
    }
    return 1;
}

static int js_transport_write_message_newline(JSDebuggerInfo *info, const char *value, size_t len) {
    char message_length[10];
    snprintf(message_length, sizeof(message_length), "%08x\n", (unsigned)(len + 1));
    if (!js_transport_write_fully(info, message_length, 9))
        return 0;
    if (!js_transport_write_fully(info, value, len))
        return 0;
    return js_transport_write_fully(info, "\n", 1);
}

static int js_transport_write_value(JSDebuggerInfo *info, JSValue value) {
    JSValue stringified = JS_JSONStringify(info->ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    size_t len;
    const char *str = JS_ToCStringLen(info->ctx, &len, stringified);
    int ret = 0;
    if (str && len)
        ret = js_transport_write_message_newline(info, str, len);
    JS_FreeCString(info->ctx, str);
    JS_FreeValue(info->ctx, stringified);
    JS_FreeValue(info->ctx, value);
    return ret;
}

static JSValue js_transport_new_envelope(JSDebuggerInfo *info, const char *type) {
    JSValue ret = JS_NewObject(info->ctx);
    JS_SetPropertyStr(info->ctx, ret, "type", JS_NewString(info->ctx, type));
    return ret;
}

static int js_transport_send_event(JSDebuggerInfo *info, JSValue event) {
    JSValue envelope = js_transport_new_envelope(info, "event");
    JS_SetPropertyStr(info->ctx, envelope, "event", event);
    return js_transport_write_value(info, envelope);
}

static int js_transport_send_response(JSDebuggerInfo *info, JSValue request, JSValue body) {
    JSContext *ctx = info->ctx;
    JSValue envelope = js_transport_new_envelope(info, "response");
    JS_SetPropertyStr(ctx, envelope, "body", body);
    JS_SetPropertyStr(ctx, envelope, "request_seq", JS_GetPropertyStr(ctx, request, "request_seq"));
    return js_transport_write_value(info, envelope);
}

/* ------------------------------------------------------------------------ */
/* variable/scope inspection (protocol side — engine-internal-free)         */
/* ------------------------------------------------------------------------ */

/* Scope encoding, mirrored by the DAP adapter: for stack frame F,
   variablesReference (F<<2)|0 = Global, |1 = Local, |2 = Closure. Object
   drill-down references count up from stack_depth<<2. */
static JSValue js_get_scopes(JSContext *ctx, int frame) {
    JSValue scopes = JS_NewArray(ctx);
    int scope_count = 0;

    JSValue local = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, local, "name", JS_NewString(ctx, "Local"));
    JS_SetPropertyStr(ctx, local, "reference", JS_NewInt32(ctx, (frame << 2) + 1));
    JS_SetPropertyStr(ctx, local, "expensive", JS_FALSE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, local);

    JSValue closure = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, closure, "name", JS_NewString(ctx, "Closure"));
    JS_SetPropertyStr(ctx, closure, "reference", JS_NewInt32(ctx, (frame << 2) + 2));
    JS_SetPropertyStr(ctx, closure, "expensive", JS_FALSE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, closure);

    JSValue global = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "name", JS_NewString(ctx, "Global"));
    JS_SetPropertyStr(ctx, global, "reference", JS_NewInt32(ctx, (frame << 2) + 0));
    JS_SetPropertyStr(ctx, global, "expensive", JS_TRUE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, global);

    return scopes;
}

static void js_debugger_get_variable_type(JSContext *ctx,
        DebuggerSuspendedState *state, JSValue var, JSValue var_val) {
    /* variablesReference = 0 means not expandable */
    uint32_t reference = 0;
    if (JS_IsString(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "string"));
    else if (JS_VALUE_GET_TAG(var_val) == JS_TAG_INT)
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "integer"));
    else if (JS_IsBigInt(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "bigint"));
    else if (JS_IsNumber(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "float"));
    else if (JS_IsBool(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "boolean"));
    else if (JS_IsNull(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "null"));
    else if (JS_IsUndefined(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "undefined"));
    else if (JS_IsObject(var_val)) {
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "object"));

        JSObject *p = JS_VALUE_GET_OBJ(var_val);
        uint32_t pl = (uint32_t)(uintptr_t)p;
        JSValue found = JS_GetPropertyUint32(ctx, state->variable_pointers, pl);
        if (JS_IsUndefined(found)) {
            reference = state->variable_reference_count++;
            JS_SetPropertyUint32(ctx, state->variable_references, reference, JS_DupValue(ctx, var_val));
            JS_SetPropertyUint32(ctx, state->variable_pointers, pl, JS_NewInt32(ctx, reference));
        } else {
            JS_ToUint32(ctx, &reference, found);
        }
        JS_FreeValue(ctx, found);
    }
    JS_SetPropertyStr(ctx, var, "variablesReference", JS_NewInt32(ctx, reference));
}

static void js_debugger_get_value(JSContext *ctx, JSValue var_val, JSValue var, const char *value_property) {
    /* don't ToString whole arrays — that stringifies every element */
    if (JS_IsArray(var_val)) {
        JSValue length = JS_GetPropertyStr(ctx, var_val, "length");
        uint32_t len;
        JS_ToUint32(ctx, &len, length);
        JS_FreeValue(ctx, length);
        char len_buf[64];
        snprintf(len_buf, sizeof(len_buf), "Array (%u)", len);
        JS_SetPropertyStr(ctx, var, value_property, JS_NewString(ctx, len_buf));
        JS_SetPropertyStr(ctx, var, "indexedVariables", JS_NewInt32(ctx, len));
    } else {
        JS_SetPropertyStr(ctx, var, value_property, JS_ToString(ctx, var_val));
    }
}

static JSValue js_debugger_get_variable(JSContext *ctx,
        DebuggerSuspendedState *state, JSValue var_name, JSValue var_val) {
    JSValue var = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, var, "name", var_name);
    js_debugger_get_value(ctx, var_val, var, "value");
    js_debugger_get_variable_type(ctx, state, var, var_val);
    return var;
}

static int js_debugger_get_frame(JSContext *ctx, JSValue args) {
    JSValue reference_property = JS_GetPropertyStr(ctx, args, "frameId");
    int frame;
    JS_ToInt32(ctx, &frame, reference_property);
    JS_FreeValue(ctx, reference_property);
    return frame;
}

static uint32_t js_get_property_as_uint32(JSContext *ctx, JSValue obj, const char *property) {
    JSValue prop = JS_GetPropertyStr(ctx, obj, property);
    uint32_t ret;
    JS_ToUint32(ctx, &ret, prop);
    JS_FreeValue(ctx, prop);
    return ret;
}

static void js_send_stopped_event(JSDebuggerInfo *info, const char *reason) {
#ifdef TNR_QJS_DEBUGGER_TRACE
    fprintf(stderr, "TRACE stopped: %s\n", reason);
#endif
    JSContext *ctx = info->debugging_ctx;
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "StoppedEvent"));
    JS_SetPropertyStr(ctx, event, "reason", JS_NewString(ctx, reason));
    /* thread id = debuggee context pointer, like the reference protocol */
    JS_SetPropertyStr(ctx, event, "thread", JS_NewInt64(ctx, (int64_t)(intptr_t)info->ctx));
    js_transport_send_event(info, event);
}

/* ------------------------------------------------------------------------ */
/* request dispatch (runs while paused, or when peek finds pending data)    */
/* ------------------------------------------------------------------------ */

static void js_process_request(JSDebuggerInfo *info, DebuggerSuspendedState *state, JSValue request) {
    JSContext *ctx = info->ctx;
    JSValue command_property = JS_GetPropertyStr(ctx, request, "command");
    const char *command = JS_ToCString(ctx, command_property);
    if (!command)
        goto free_request;

    if (strcmp("continue", command) == 0) {
        /* "continue" still records a step location: the CONTINUE pseudo-step
           carries execution off the current statement before breakpoints on
           it re-arm (otherwise a continue at a breakpoint re-hits it). */
        info->stepping = JS_DEBUGGER_STEP_CONTINUE;
        info->step_over = js_debugger_current_location(ctx, state->cur_pc);
        info->step_depth = js_debugger_stack_depth(ctx);
        js_transport_send_response(info, request, JS_UNDEFINED);
        info->is_paused = 0;
    }
    else if (strcmp("pause", command) == 0) {
        js_transport_send_response(info, request, JS_UNDEFINED);
        js_send_stopped_event(info, "pause");
        info->is_paused = 1;
    }
    else if (strcmp("next", command) == 0) {
        info->stepping = JS_DEBUGGER_STEP;
        info->step_over = js_debugger_current_location(ctx, state->cur_pc);
        info->step_depth = js_debugger_stack_depth(ctx);
        js_transport_send_response(info, request, JS_UNDEFINED);
        info->is_paused = 0;
    }
    else if (strcmp("stepIn", command) == 0) {
        info->stepping = JS_DEBUGGER_STEP_IN;
        info->step_over = js_debugger_current_location(ctx, state->cur_pc);
        info->step_depth = js_debugger_stack_depth(ctx);
        js_transport_send_response(info, request, JS_UNDEFINED);
        info->is_paused = 0;
    }
    else if (strcmp("stepOut", command) == 0) {
        info->stepping = JS_DEBUGGER_STEP_OUT;
        info->step_over = js_debugger_current_location(ctx, state->cur_pc);
        info->step_depth = js_debugger_stack_depth(ctx);
        js_transport_send_response(info, request, JS_UNDEFINED);
        info->is_paused = 0;
    }
    else if (strcmp("evaluate", command) == 0) {
        JSValue args = JS_GetPropertyStr(ctx, request, "args");
        int frame = js_debugger_get_frame(ctx, args);
        JSValue expression = JS_GetPropertyStr(ctx, args, "expression");
        JS_FreeValue(ctx, args);
        JSValue result = js_debugger_evaluate(ctx, frame, expression);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, result);
            result = JS_GetException(ctx);
        }
        JS_FreeValue(ctx, expression);

        JSValue body = JS_NewObject(ctx);
        js_debugger_get_value(ctx, result, body, "result");
        js_debugger_get_variable_type(ctx, state, body, result);
        JS_FreeValue(ctx, result);
        js_transport_send_response(info, request, body);
    }
    else if (strcmp("stackTrace", command) == 0) {
        JSValue stack_trace = js_debugger_build_backtrace(ctx, state->cur_pc);
        js_transport_send_response(info, request, stack_trace);
    }
    else if (strcmp("scopes", command) == 0) {
        JSValue args = JS_GetPropertyStr(ctx, request, "args");
        int frame = js_debugger_get_frame(ctx, args);
        JS_FreeValue(ctx, args);
        JSValue scopes = js_get_scopes(ctx, frame);
        js_transport_send_response(info, request, scopes);
    }
    else if (strcmp("variables", command) == 0) {
        JSValue args = JS_GetPropertyStr(ctx, request, "args");
        uint32_t reference = js_get_property_as_uint32(ctx, args, "variablesReference");

        JSValue properties = JS_NewArray(ctx);
        JSValue variable = JS_GetPropertyUint32(ctx, state->variable_references, reference);

        int skip_proto = 0;
        /* an unknown reference must be a frame scope: (frame<<2)|scope */
        if (JS_IsUndefined(variable)) {
            skip_proto = 1;
            uint32_t frame = reference >> 2;
            uint32_t scope = reference % 4;

            if (frame >= js_debugger_stack_depth(ctx))
                goto reply;

            if (scope == 0)
                variable = JS_GetGlobalObject(ctx);
            else if (scope == 1)
                variable = js_debugger_local_variables(ctx, frame);
            else if (scope == 2)
                variable = js_debugger_closure_variables(ctx, frame);
            else
                goto reply;

            /* cache: subsequent lookups of this reference reuse the object */
            JS_SetPropertyUint32(ctx, state->variable_references, reference, JS_DupValue(ctx, variable));
        }

        JSValue filter = JS_GetPropertyStr(ctx, args, "filter");
        if (!JS_IsUndefined(filter)) {
            const char *filter_str = JS_ToCString(ctx, filter);
            JS_FreeValue(ctx, filter);
            /* only "indexed" paging is served; named filtering falls through */
            int indexed = filter_str && strcmp(filter_str, "indexed") == 0;
            JS_FreeCString(ctx, filter_str);
            if (indexed) {
                uint32_t start = js_get_property_as_uint32(ctx, args, "start");
                uint32_t count = js_get_property_as_uint32(ctx, args, "count");
                char name_buf[64];
                for (uint32_t i = 0; i < count; i++) {
                    JSValue value = JS_GetPropertyUint32(ctx, variable, start + i);
                    snprintf(name_buf, sizeof(name_buf), "%u", start + i);
                    JSValue variable_json = js_debugger_get_variable(ctx, state, JS_NewString(ctx, name_buf), value);
                    JS_FreeValue(ctx, value);
                    JS_SetPropertyUint32(ctx, properties, i, variable_json);
                }
                goto reply_var;
            }
        }

        {
            JSPropertyEnum *tab_atom;
            uint32_t tab_atom_count;
            if (!JS_GetOwnPropertyNames(ctx, &tab_atom, &tab_atom_count, variable,
                                        JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)) {
                uint32_t offset = 0;

                if (!skip_proto) {
                    const JSValue proto = JS_GetPrototype(ctx, variable);
                    if (!JS_IsException(proto)) {
                        JSValue variable_json = js_debugger_get_variable(ctx, state, JS_NewString(ctx, "__proto__"), proto);
                        JS_FreeValue(ctx, proto);
                        JS_SetPropertyUint32(ctx, properties, offset++, variable_json);
                    } else {
                        JS_FreeValue(ctx, proto);
                    }
                }

                for (uint32_t i = 0; i < tab_atom_count; i++) {
                    JSValue value = JS_GetProperty(ctx, variable, tab_atom[i].atom);
                    JSValue variable_json = js_debugger_get_variable(ctx, state, JS_AtomToString(ctx, tab_atom[i].atom), value);
                    JS_FreeValue(ctx, value);
                    JS_SetPropertyUint32(ctx, properties, i + offset, variable_json);
                }

                js_free_prop_enum(ctx, tab_atom, tab_atom_count);
            }
        }

    reply_var:
        JS_FreeValue(ctx, variable);
    reply:
        JS_FreeValue(ctx, args);
        js_transport_send_response(info, request, properties);
    }

free_request:
    JS_FreeCString(ctx, command);
    JS_FreeValue(ctx, command_property);
    JS_FreeValue(ctx, request);
}

static void js_process_breakpoints(JSDebuggerInfo *info, JSValue message) {
    JSContext *ctx = info->ctx;

    /* force all functions to reprocess their breakpoint maps */
    info->breakpoints_dirty_counter++;

    JSValue path_property = JS_GetPropertyStr(ctx, message, "path");
    const char *path = JS_ToCString(ctx, path_property);
    if (path) {
        /* per-path record; resolved into per-function pc maps when a function
           from this path notices the dirty counter moved */
        JSValue path_data = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, path_data, "breakpoints", JS_GetPropertyStr(ctx, message, "breakpoints"));
        JS_SetPropertyStr(ctx, path_data, "dirty", JS_NewUint32(ctx, info->breakpoints_dirty_counter));
        JS_SetPropertyStr(ctx, info->breakpoints, path, path_data);
    }
    JS_FreeCString(ctx, path);
    JS_FreeValue(ctx, path_property);
    JS_FreeValue(ctx, message);
}

JSValue js_debugger_file_breakpoints(JSContext *ctx, const char *path) {
    JSDebuggerInfo *info = js_debugger_info(JS_GetRuntime(ctx));
    return JS_GetPropertyStr(ctx, info->breakpoints, path);
}

/* The pause loop. Blocks the JS thread reading protocol messages until a
   continue/step clears is_paused (or the transport dies). Also runs unpaused
   when peek detects pending client data (breakpoint updates mid-run). */
static int js_process_debugger_messages(JSDebuggerInfo *info, const uint8_t *cur_pc) {
    JSContext *ctx = info->ctx;
    DebuggerSuspendedState state;
    state.variable_reference_count = js_debugger_stack_depth(ctx) << 2;
    state.variable_pointers = JS_NewObject(ctx);
    state.variable_references = JS_NewObject(ctx);
    state.cur_pc = cur_pc;
    int ret = 0;
    char message_length_buf[10];

    do {
        fflush(stdout);
        fflush(stderr);

        if (!js_transport_read_fully(info, message_length_buf, 9))
            goto done;

        message_length_buf[8] = '\0';
        int message_length = (int)strtol(message_length_buf, NULL, 16);
        if (message_length <= 0)
            goto done; /* framing garbage: treat as transport failure */
        if (message_length > info->message_buffer_length) {
            if (info->message_buffer) {
                js_free_rt(JS_GetRuntime(ctx), info->message_buffer);
                info->message_buffer = NULL;
                info->message_buffer_length = 0;
            }
            /* +1 for null termination */
            info->message_buffer = js_malloc_rt(JS_GetRuntime(ctx), message_length + 1);
            if (!info->message_buffer)
                goto done;
            info->message_buffer_length = message_length;
        }

        if (!js_transport_read_fully(info, info->message_buffer, message_length))
            goto done;
        info->message_buffer[message_length] = '\0';
#ifdef TNR_QJS_DEBUGGER_TRACE
        fprintf(stderr, "TRACE consumed(paused=%d): %s", info->is_paused, info->message_buffer);
#endif

        JSValue message = JS_ParseJSON(ctx, info->message_buffer, message_length, "<debugger>");
        JSValue vtype = JS_GetPropertyStr(ctx, message, "type");
        const char *type = JS_ToCString(ctx, vtype);
        if (type) {
            if (strcmp("request", type) == 0)
                js_process_request(info, &state, JS_GetPropertyStr(ctx, message, "request"));
            else if (strcmp("continue", type) == 0) {
                /* bare continue must arm the same escape as the request-form
                   continue: the interpreter's stacked CASE labels (e.g.
                   push_0..push_7 share one body) run one js_debugger_check
                   per label at the SAME pc, and a resumed breakpoint would
                   re-fire on each without the escape */
                info->stepping = JS_DEBUGGER_STEP_CONTINUE;
                info->step_over = js_debugger_current_location(ctx, state.cur_pc);
                info->step_depth = js_debugger_stack_depth(ctx);
                info->is_paused = 0;
            }
            else if (strcmp("breakpoints", type) == 0)
                js_process_breakpoints(info, JS_GetPropertyStr(ctx, message, "breakpoints"));
            else if (strcmp("stopOnException", type) == 0) {
                JSValue stop = JS_GetPropertyStr(ctx, message, "stopOnException");
                info->exception_breakpoint = JS_ToBool(ctx, stop);
                JS_FreeValue(ctx, stop);
            }
        }
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, vtype);
        JS_FreeValue(ctx, message);
    } while (info->is_paused);

    ret = 1;

done:
    JS_FreeValue(ctx, state.variable_references);
    JS_FreeValue(ctx, state.variable_pointers);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* hooks called from the quickjs.c anchors                                  */
/* ------------------------------------------------------------------------ */

void js_debugger_exception(JSContext *ctx) {
    JSDebuggerInfo *info = js_debugger_info(JS_GetRuntime(ctx));
    if (!info->exception_breakpoint)
        return;
    if (info->is_debugging)
        return;
    if (!info->transport_close)
        return;
    /* quickjs-ng drift: build_backtrace() (run by every `new Error`) uses
       JS_Throw internally to save/restore the current exception — that is
       engine bookkeeping, not a user throw. Without this guard the debugger
       stops "on exception" inside Error construction itself. */
    if (ctx->rt->in_build_stack_trace)
        return;
    info->is_debugging = 1;
    info->ctx = ctx;
    /* Debugger requests during the pause (evaluate, inspection) run JS on the
       debuggee context; if any of it throws, JS_Throw would free and replace
       the very exception being reported, and the interpreter's unwind would
       then resume with garbage (observed: the catch block's e.message threw,
       triggering a second, message-starved exception stop). Preserve the
       in-flight exception across the pause and restore it verbatim. */
    JSValue pending = JS_DupValue(ctx, ctx->rt->current_exception);
    js_send_stopped_event(info, "exception");
    info->is_paused = 1;
    js_process_debugger_messages(info, NULL);
    JS_FreeValue(ctx, ctx->rt->current_exception);
    ctx->rt->current_exception = pending;
    info->is_debugging = 0;
    info->ctx = NULL;
}

static void js_debugger_context_event(JSContext *caller_ctx, const char *reason) {
    if (!js_debugger_is_transport_connected(JS_GetRuntime(caller_ctx)))
        return;

    JSDebuggerInfo *info = js_debugger_info(JS_GetRuntime(caller_ctx));
    if (info->debugging_ctx == caller_ctx)
        return;

    JSContext *ctx = info->debugging_ctx;
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "ThreadEvent"));
    JS_SetPropertyStr(ctx, event, "reason", JS_NewString(ctx, reason));
    JS_SetPropertyStr(ctx, event, "thread", JS_NewInt64(ctx, (int64_t)(intptr_t)caller_ctx));
    js_transport_send_event(info, event);
}

void js_debugger_new_context(JSContext *ctx) {
    js_debugger_context_event(ctx, "new");
}

void js_debugger_free_context(JSContext *ctx) {
    js_debugger_context_event(ctx, "exited");
}

/* Interpreter check: called per-opcode via the debugger dispatch table (A8,
   only when a transport is attached) and once per call frame (A9). Everything
   here runs on the JS thread; socket I/O is polled, never threaded. */
void js_debugger_check(JSContext *ctx, const uint8_t *cur_pc) {
    JSDebuggerInfo *info = js_debugger_info(JS_GetRuntime(ctx));
    if (info->is_debugging)
        return;
    if (info->debugging_ctx == ctx)
        return;
    if (info->transport_close == NULL)
        return;
    info->is_debugging = 1;
    info->ctx = ctx;

    JSDebuggerLocation location;
    uint32_t depth;

    /* Stepping location check runs before the breakpoint check: a step must
       escape its own statement even when that statement carries a breakpoint.
       STEP_CONTINUE (resume-from-breakpoint) escapes by LINE, not column:
       a line often spans several pc2line segments with different columns,
       and a resumed breakpoint must not re-fire on each of them. Real steps
       keep the column compare — column-granular stepping is a feature. */
    if (info->stepping) {
        location = js_debugger_current_location(ctx, cur_pc);
        depth = js_debugger_stack_depth(ctx);
        if (info->step_depth == depth
            && location.filename == info->step_over.filename
            && location.line == info->step_over.line
            && (info->stepping == JS_DEBUGGER_STEP_CONTINUE
                || location.column == info->step_over.column))
            goto done;
    }

    int at_breakpoint = js_debugger_check_breakpoint(ctx, info->breakpoints_dirty_counter, cur_pc);
    if (at_breakpoint) {
#ifdef TNR_QJS_DEBUGGER_TRACE
        {
            JSDebuggerLocation l = js_debugger_current_location(ctx, cur_pc);
            JSObject *tf = JS_VALUE_GET_OBJ(ctx->rt->current_stack_frame->cur_func);
            JSFunctionBytecode *tb = tf->u.func.function_bytecode;
            fprintf(stderr, "TRACE bp-hit line=%d col=%d depth=%u stepping=%d pc_off=%ld op=%d\n",
                    l.line, l.column, js_debugger_stack_depth(ctx), info->stepping,
                    (long)(cur_pc - tb->byte_code_buf - 1), cur_pc[-1]);
        }
#endif
        /* reaching a breakpoint cancels any in-flight step */
        info->stepping = 0;
        info->is_paused = 1;
        js_send_stopped_event(info, "breakpoint");
    }
    else if (info->stepping) {
        if (info->stepping == JS_DEBUGGER_STEP_CONTINUE) {
            /* the statement has been escaped (checked above); resume free-run.
               Exception: a function-entry check (pc=NULL) has no location —
               don't let it cancel the escape before a real position is seen. */
            location = js_debugger_current_location(ctx, cur_pc);
            if (location.filename != 0)
                info->stepping = 0;
        }
        else if (info->stepping == JS_DEBUGGER_STEP_IN) {
            /* stop on any location change: same-depth move, deeper (into a
               call), or shallower (the step ran off the end of the frame) */
            info->stepping = 0;
            info->is_paused = 1;
            js_send_stopped_event(info, "stepIn");
        }
        else if (info->stepping == JS_DEBUGGER_STEP_OUT) {
            uint32_t current_depth = js_debugger_stack_depth(ctx);
            if (current_depth >= info->step_depth)
                goto done;
            info->stepping = 0;
            info->is_paused = 1;
            js_send_stopped_event(info, "stepOut");
        }
        else if (info->stepping == JS_DEBUGGER_STEP) {
            /* step-over: stop when the location changed at the same depth or
               shallower; keep running while inside a deeper call, and while
               the location matches at any depth (recursion on the same line) */
            location = js_debugger_current_location(ctx, cur_pc);
            if ((location.filename == info->step_over.filename
                 && location.line == info->step_over.line
                 && location.column == info->step_over.column)
                || js_debugger_stack_depth(ctx) > info->step_depth)
                goto done;
            info->stepping = 0;
            info->is_paused = 1;
            js_send_stopped_event(info, "step");
        }
        else {
            info->stepping = 0;
        }
    }

    if (!info->is_paused) {
        /* running free: peek the transport occasionally (or when the host
           called js_debugger_cooperate) for breakpoint updates / pause */
        if (info->peek_ticks++ < 10000 && !info->should_peek)
            goto done;
        info->peek_ticks = 0;
        info->should_peek = 0;

        while (!info->is_paused) {
            size_t peek = info->transport_peek(info->transport_udata);
            if (peek == (size_t)-1)
                goto fail;
            if (peek == 0)
                goto done;
            if (!js_process_debugger_messages(info, cur_pc))
                goto fail;
        }
    }

    if (js_process_debugger_messages(info, cur_pc))
        goto done;

fail:
    js_debugger_free(JS_GetRuntime(ctx), info);
done:
    info->is_debugging = 0;
    info->ctx = NULL;
}

void js_debugger_free(JSRuntime *rt, JSDebuggerInfo *info) {
    if (!info->transport_close)
        return;

    /* raw write: the runtime may be mid-teardown, don't touch JS state */
    const char *terminated = "{\"type\":\"event\",\"event\":{\"type\":\"terminated\"}}";
    js_transport_write_message_newline(info, terminated, strlen(terminated));

    info->transport_close(rt, info->transport_udata);
    info->transport_read = NULL;
    info->transport_write = NULL;
    info->transport_peek = NULL;
    info->transport_close = NULL;

    if (info->message_buffer) {
        js_free_rt(rt, info->message_buffer);
        info->message_buffer = NULL;
        info->message_buffer_length = 0;
    }

    JS_FreeValue(info->debugging_ctx, info->breakpoints);
    info->breakpoints = JS_UNDEFINED;

    JS_FreeContext(info->debugging_ctx);
    info->debugging_ctx = NULL;
    info->is_paused = 0;
    info->stepping = 0;
    info->exception_breakpoint = 0;
}

void js_debugger_attach(
    JSContext *ctx,
    size_t (*transport_read)(void *udata, char *buffer, size_t length),
    size_t (*transport_write)(void *udata, const char *buffer, size_t length),
    size_t (*transport_peek)(void *udata),
    void (*transport_close)(JSRuntime *rt, void *udata),
    void *udata) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSDebuggerInfo *info = js_debugger_info(rt);
    js_debugger_free(rt, info);

    /* transport callbacks are set only after the scratch context exists, so
       the JS_NewContext below can't emit a ThreadEvent for it */
    info->debugging_ctx = JS_NewContext(rt);
    info->transport_read = transport_read;
    info->transport_write = transport_write;
    info->transport_peek = transport_peek;
    info->transport_close = transport_close;
    info->transport_udata = udata;

    JSContext *original_ctx = info->ctx;
    info->ctx = ctx;

    js_send_stopped_event(info, "entry");

    info->breakpoints = JS_NewObject(info->debugging_ctx);
    info->is_paused = 1;

    js_process_debugger_messages(info, NULL);

    info->ctx = original_ctx;
}

int js_debugger_is_transport_connected(JSRuntime *rt) {
    return js_debugger_info(rt)->transport_close != NULL;
}

void js_debugger_cooperate(JSContext *ctx) {
    js_debugger_info(JS_GetRuntime(ctx))->should_peek = 1;
}

/* Anchor A12 support: like add_pc2line_info(), but called for EVERY emitted
   opcode of the compiler's final pass, and with a growable slot array —
   upstream sizes source_loc_slots to the surviving OP_source_loc marker
   count, which per-op recording can exceed. Same dedup rules, so the table
   only grows by one entry per actual position change. */
void js_debugger_pc2line_every_op(struct JSFunctionDef *s, uint32_t pc, int line_num, int col_num) {
    if (s->source_loc_slots == NULL)
        return; /* stripped build: no debug info wanted */
    if (pc < s->line_number_last_pc)
        return;
    if (line_num == s->line_number_last && col_num == s->col_number_last)
        return;
    if (s->source_loc_count >= s->source_loc_size) {
        int new_size = s->source_loc_size * 2 + 8;
        SourceLocSlot *slots = js_realloc(s->ctx, s->source_loc_slots,
                                          sizeof(*slots) * new_size);
        if (!slots)
            return; /* degrade to upstream behavior on OOM */
        s->source_loc_slots = slots;
        s->source_loc_size = new_size;
    }
    s->source_loc_slots[s->source_loc_count].pc = pc;
    s->source_loc_slots[s->source_loc_count].line_num = line_num;
    s->source_loc_slots[s->source_loc_count].col_num = col_num;
    s->source_loc_count++;
    s->line_number_last_pc = pc;
    s->line_number_last = line_num;
    s->col_number_last = col_num;
}

/* ------------------------------------------------------------------------ */
/* internals: everything below needs quickjs.c statics/types                */
/* ------------------------------------------------------------------------ */

JSDebuggerInfo *js_debugger_info(JSRuntime *rt) {
    return &rt->debugger_info;
}

JSDebuggerLocation js_debugger_current_location(JSContext *ctx, const uint8_t *cur_pc) {
    JSDebuggerLocation location;
    location.filename = 0;
    location.line = 0;
    location.column = 0;
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    if (!sf)
        return location;

    JSObject *p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (!p || !js_class_has_bytecode(p->class_id))
        return location;

    JSFunctionBytecode *b = p->u.func.function_bytecode;
    /* pc2line_buf may be NULL (one-line function, zero entries) —
       find_line_num falls back to the function's own line in that case */
    if (!b->filename)
        return location;

    /* at function entry (anchor A9) cur_pc is NULL and sf->cur_pc may still
       be NULL too (quickjs-ng clears it during frame setup) */
    const uint8_t *pc = cur_pc ? cur_pc : sf->cur_pc;
    if (!pc)
        return location;

    location.line = find_line_num(ctx, b, pc - b->byte_code_buf - 1, &location.column);
    location.filename = b->filename;
    return location;
}

uint32_t js_debugger_stack_depth(JSContext *ctx) {
    uint32_t stack_index = 0;
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    while (sf != NULL) {
        sf = sf->prev_frame;
        stack_index++;
    }
    return stack_index;
}

JSValue js_debugger_build_backtrace(JSContext *ctx, const uint8_t *cur_pc) {
    JSStackFrame *sf;
    const char *func_name_str;
    JSObject *p;
    JSValue ret = JS_NewArray(ctx);
    uint32_t stack_index = 0;

    for (sf = ctx->rt->current_stack_frame; sf != NULL; sf = sf->prev_frame) {
        JSValue current_frame = JS_NewObject(ctx);

        uint32_t id = stack_index++;
        JS_SetPropertyStr(ctx, current_frame, "id", JS_NewUint32(ctx, id));

        func_name_str = get_func_name(ctx, sf->cur_func);
        if (!func_name_str || func_name_str[0] == '\0')
            JS_SetPropertyStr(ctx, current_frame, "name", JS_NewString(ctx, "<anonymous>"));
        else
            JS_SetPropertyStr(ctx, current_frame, "name", JS_NewString(ctx, func_name_str));
        JS_FreeCString(ctx, func_name_str);

        p = JS_VALUE_GET_OBJ(sf->cur_func);
        if (p && js_class_has_bytecode(p->class_id)) {
            JSFunctionBytecode *b = p->u.func.function_bytecode;
            if (b->filename) {
                /* cur_pc overrides the top frame only; callers' sf->cur_pc
                   already point after their call instruction */
                const uint8_t *pc = (sf != ctx->rt->current_stack_frame || !cur_pc) ? sf->cur_pc : cur_pc;
                int line, col;
                if (pc) {
                    col = 0;
                    line = find_line_num(ctx, b, pc - b->byte_code_buf - 1, &col);
                } else {
                    /* function-entry stop (anchor A9): no pc executed yet —
                       report the function's own definition site */
                    line = b->line_num;
                    col = b->col_num;
                }
                JS_SetPropertyStr(ctx, current_frame, "filename", JS_AtomToString(ctx, b->filename));
                if (line != -1) {
                    JS_SetPropertyStr(ctx, current_frame, "line", JS_NewInt32(ctx, line));
                    JS_SetPropertyStr(ctx, current_frame, "column", JS_NewInt32(ctx, col));
                }
            }
        } else {
            JS_SetPropertyStr(ctx, current_frame, "name", JS_NewString(ctx, "(native)"));
        }
        JS_SetPropertyUint32(ctx, ret, id, current_frame);
    }
    return ret;
}

/* Breakpoint check (P12.2). The hot path is one byte load: each function
   caches a byte map parallel to its bytecode (b->debugger.breakpoints,
   1 = a breakpoint covers this pc), rebuilt lazily when the global
   breakpoints_dirty_counter has moved past the function's cached stamp.

   The rebuild walks the function's pc2line table with quickjs-ng's encoding —
   THE part that differs from every Bellard-era debugger port: ng appends a
   column sleb128 to EVERY entry (both the short form and the op==0 long
   form), exactly mirroring find_line_num() above. The walk yields segments
   [seg_start, seg_end) each carrying a (line, column) source position; a
   segment is marked when a breakpoint matches its line and, if the
   breakpoint carries a column (> 0, 1-based), the segment's column is at or
   past it — giving column-accurate (inline) breakpoints for free. */
/* Mark one pc2line segment in the breakpoint map, instruction by
   instruction, skipping declaration plumbing. Hoisted function/class
   declarations execute at scope entry with the DECLARATION's source
   position — the whole hoist block is typically one (line,col) segment of
   check_define_var / push_const+fclosure / define_func opcodes. Marking
   those would fire a `function foo() {...}`-line breakpoint spuriously
   while the enclosing scope is merely creating foo. Users break on
   statements; declarations run silently — Chrome behaves the same.
   (Skipping push_const is safe for real statements: some other opcode of
   the statement carries the mark.) */
static void js_debugger_mark_segment(JSFunctionBytecode *b, int seg_start, int seg_end) {
    int pos = seg_start;
    if (seg_end > b->byte_code_len)
        seg_end = b->byte_code_len;
    while (pos < seg_end) {
        unsigned int op = b->byte_code_buf[pos];
        int len = short_opcode_info(op).size;
        if (len <= 0 || pos + len > seg_end)
            len = seg_end - pos; /* defensive: never walk past the segment */
        switch (op) {
        case OP_check_define_var:
        case OP_define_var:
        case OP_push_const:
        case OP_push_const8:
        case OP_fclosure:
        case OP_fclosure8:
        case OP_define_func:
        case OP_define_class:
        case OP_define_class_computed:
            break;
        default:
            memset(b->debugger.breakpoints + pos, 1, len);
        }
        pos += len;
    }
}

int js_debugger_check_breakpoint(JSContext *ctx, uint32_t current_dirty, const uint8_t *cur_pc) {
    if (!ctx->rt->current_stack_frame)
        return 0;
    JSObject *f = JS_VALUE_GET_OBJ(ctx->rt->current_stack_frame->cur_func);
    if (!f || !js_class_has_bytecode(f->class_id))
        return 0;
    JSFunctionBytecode *b = f->u.func.function_bytecode;
    /* NOTE: pc2line_buf may be NULL — quickjs-ng emits ZERO pc2line entries
       for a function whose ops never change source position (a one-line
       function like `function one() { return 1; }`). Its whole body is then
       a single segment at (b->line_num, b->col_num); only a missing
       filename (stripped build) disables breakpoints. */
    if (!b->filename)
        return 0;

    if (b->debugger.dirty == current_dirty)
        goto lookup;

    {
        uint32_t prev_dirty = b->debugger.dirty;
        b->debugger.dirty = current_dirty;

        const char *filename = JS_AtomToCString(ctx, b->filename);
        JSValue path_data = filename ? js_debugger_file_breakpoints(ctx, filename) : JS_UNDEFINED;
        JS_FreeCString(ctx, filename);
        if (JS_IsUndefined(path_data))
            goto lookup; /* no client breakpoints for this file, ever */

        /* if this file's breakpoints haven't changed since our last rebuild,
           the cached map is already correct */
        uint32_t path_dirty = js_get_property_as_uint32(ctx, path_data, "dirty");
        if (path_dirty == prev_dirty && b->debugger.breakpoints) {
            JS_FreeValue(ctx, path_data);
            goto lookup;
        }

        if (!b->debugger.breakpoints) {
            b->debugger.breakpoints = js_malloc_rt(ctx->rt, b->byte_code_len);
            if (!b->debugger.breakpoints) {
                JS_FreeValue(ctx, path_data);
                return 0;
            }
        }
        memset(b->debugger.breakpoints, 0, b->byte_code_len);
#ifdef TNR_QJS_DEBUGGER_TRACE
        {
            const char *fn = JS_AtomToCString(ctx, b->func_name);
            fprintf(stderr, "TRACE rebuild func=%s line_num=%d col_num=%d bclen=%d pc2line=%p buf=%p ops=[",
                    fn ? fn : "?", b->line_num, b->col_num, b->byte_code_len, (void *)b->pc2line_buf,
                    (void *)b->byte_code_buf);
            for (int ti = 0; ti < b->byte_code_len && ti < 12; ti++)
                fprintf(stderr, "%d ", b->byte_code_buf[ti]);
            fprintf(stderr, "] p2l=[");
            for (int ti = 0; ti < b->pc2line_len && ti < 24; ti++)
                fprintf(stderr, "%d ", b->pc2line_buf[ti]);
            fprintf(stderr, "] p2llen=%d\n", b->pc2line_len);
            JS_FreeCString(ctx, fn);
        }
#endif

        /* pull the breakpoint list into plain arrays once (the segment walk
           below scans it per segment; lists are a handful of entries) */
        JSValue bp_array = JS_GetPropertyStr(ctx, path_data, "breakpoints");
        JS_FreeValue(ctx, path_data);
        uint32_t bp_count = 0;
        int *bp_lines = NULL, *bp_cols = NULL;
        if (!JS_IsUndefined(bp_array)) {
            JSValue len_val = JS_GetPropertyStr(ctx, bp_array, "length");
            JS_ToUint32(ctx, &bp_count, len_val);
            JS_FreeValue(ctx, len_val);
        }
        if (bp_count > 0) {
            bp_lines = js_malloc(ctx, sizeof(int) * bp_count * 2);
            if (!bp_lines) {
                JS_FreeValue(ctx, bp_array);
                return 0;
            }
            bp_cols = bp_lines + bp_count;
            for (uint32_t i = 0; i < bp_count; i++) {
                JSValue bp = JS_GetPropertyUint32(ctx, bp_array, i);
                bp_lines[i] = (int)js_get_property_as_uint32(ctx, bp, "line");
                bp_cols[i] = (int)js_get_property_as_uint32(ctx, bp, "column");
                JS_FreeValue(ctx, bp);
            }
        }
        JS_FreeValue(ctx, bp_array);

        if (bp_count > 0) {
            /* segment walk — decode identical to find_line_num() */
            const uint8_t *p = b->pc2line_buf;
            const uint8_t *p_end = p + b->pc2line_len;
            int pc = 0, line = b->line_num, col = b->col_num;
            int new_line, v, ret;
            unsigned int op;

            while (p && p < p_end) {
                int seg_start = pc;
                op = *p++;
                if (op == 0) {
                    uint32_t val;
                    ret = get_leb128(&val, p, p_end);
                    if (ret < 0)
                        break;
                    pc += val;
                    p += ret;
                    ret = get_sleb128(&v, p, p_end);
                    if (ret < 0)
                        break;
                    p += ret;
                    new_line = line + v;
                } else {
                    op -= PC2LINE_OP_FIRST;
                    pc += (op / PC2LINE_RANGE);
                    new_line = line + (op % PC2LINE_RANGE) + PC2LINE_BASE;
                }
                /* ng always encodes a column delta, unlike Bellard quickjs */
                ret = get_sleb128(&v, p, p_end);
                if (ret < 0)
                    break;
                p += ret;

#ifdef TNR_QJS_DEBUGGER_TRACE
                fprintf(stderr, "TRACE seg [%d,%d) line=%d col=%d op=%d\n",
                        seg_start, pc, line, col, b->byte_code_buf[seg_start]);
#endif
                /* segment [seg_start, pc) executes at (line, col) */
                if (pc > seg_start && pc <= b->byte_code_len) {
                    for (uint32_t i = 0; i < bp_count; i++) {
                        if (bp_lines[i] == line && (bp_cols[i] <= 0 || col >= bp_cols[i])) {
                            js_debugger_mark_segment(b, seg_start, pc);
                            break;
                        }
                    }
                }
                line = new_line;
                col = col + v;
            }
            /* tail segment: [pc, byte_code_len) at the final (line, col) */
            if (pc < b->byte_code_len) {
                for (uint32_t i = 0; i < bp_count; i++) {
                    if (bp_lines[i] == line && (bp_cols[i] <= 0 || col >= bp_cols[i])) {
                        js_debugger_mark_segment(b, pc, b->byte_code_len);
                        break;
                    }
                }
            }
        }
        if (bp_lines)
            js_free(ctx, bp_lines);
    }

lookup:
    if (!b->debugger.breakpoints)
        return 0;
    {
        const uint8_t *pc_ptr = cur_pc ? cur_pc : ctx->rt->current_stack_frame->cur_pc;
        if (!pc_ptr)
            return 0;
        int pc_off = (int)(pc_ptr - b->byte_code_buf) - 1;
        if (pc_off < 0 || pc_off >= b->byte_code_len)
            return 0;
        return b->debugger.breakpoints[pc_off];
    }
}

JSValue js_debugger_local_variables(JSContext *ctx, int stack_index) {
    JSValue ret = JS_NewObject(ctx);

    /* surface the in-flight exception on the top frame */
    if (stack_index == 0 && !JS_IsNull(ctx->rt->current_exception) && !JS_IsUndefined(ctx->rt->current_exception))
        JS_SetPropertyStr(ctx, ret, "<exception>", JS_DupValue(ctx, ctx->rt->current_exception));

    JSStackFrame *sf;
    int cur_index = 0;

    for (sf = ctx->rt->current_stack_frame; sf != NULL; sf = sf->prev_frame) {
        if (cur_index < stack_index) {
            cur_index++;
            continue;
        }

        JSObject *f = JS_VALUE_GET_OBJ(sf->cur_func);
        if (!f || !js_class_has_bytecode(f->class_id))
            break;
        JSFunctionBytecode *b = f->u.func.function_bytecode;
        if (!b->vardefs)
            break;

        for (uint32_t i = 0; i < (uint32_t)(b->arg_count + b->var_count); i++) {
            JSValue var_val;
            if (i < b->arg_count) {
                if (!sf->arg_buf)
                    continue;
                var_val = sf->arg_buf[i];
            } else {
                if (!sf->var_buf)
                    continue;
                var_val = sf->var_buf[i - b->arg_count];
            }

            if (JS_IsUninitialized(var_val))
                continue;

            JSVarDef *vd = b->vardefs + i;
            JS_SetProperty(ctx, ret, JS_DupAtom(ctx, vd->var_name), JS_DupValue(ctx, var_val));
        }
        break;
    }

    return ret;
}

JSValue js_debugger_closure_variables(JSContext *ctx, int stack_index) {
    JSValue ret = JS_NewObject(ctx);

    JSStackFrame *sf;
    int cur_index = 0;
    for (sf = ctx->rt->current_stack_frame; sf != NULL; sf = sf->prev_frame) {
        if (cur_index < stack_index) {
            cur_index++;
            continue;
        }

        JSObject *f = JS_VALUE_GET_OBJ(sf->cur_func);
        if (!f || !js_class_has_bytecode(f->class_id))
            break;

        JSFunctionBytecode *b = f->u.func.function_bytecode;

        for (uint32_t i = 0; i < b->closure_var_count; i++) {
            JSClosureVar *cvar = b->closure_var + i;
            JSVarRef *var_ref = NULL;
            if (f->u.func.var_refs)
                var_ref = f->u.func.var_refs[i];
            if (!var_ref || !var_ref->pvalue)
                continue;
            JSValue var_val = *var_ref->pvalue;

            if (JS_IsUninitialized(var_val))
                continue;

            JS_SetProperty(ctx, ret, JS_DupAtom(ctx, cvar->var_name), JS_DupValue(ctx, var_val));
        }
        break;
    }

    return ret;
}

/* Our stand-in for koush's DEBUG_SCOP_INDEX mode of add_closure_variables():
   capture EVERY lexical variable of the frame (not just those visible from
   one scope chain position), plus arguments, non-lexical locals, and the
   function's own closure vars — so a debug eval can see everything the
   paused frame can. Mirrors upstream add_closure_variables() with a widened
   first loop; kept here so the upstream function stays pristine. */
static __exception int js_debugger_add_closure_variables(JSContext *ctx, JSFunctionDef *s,
                                                         JSFunctionBytecode *b) {
    int i, count;
    JSVarDef *vd;

    count = b->arg_count + b->var_count + b->closure_var_count;
    s->closure_var = NULL;
    s->closure_var_count = 0;
    s->closure_var_size = count;
    if (count == 0)
        return 0;
    s->closure_var = js_malloc(ctx, sizeof(s->closure_var[0]) * count);
    if (!s->closure_var)
        return -1;
    /* all lexical variables, regardless of scope position */
    for (i = 0; i < b->var_count; i++) {
        vd = &b->vardefs[b->arg_count + i];
        if (vd->scope_level > 0) {
            JSClosureVar *cv = &s->closure_var[s->closure_var_count++];
            set_closure_from_var(ctx, cv, vd, i);
        }
    }
    /* arguments */
    for (i = 0; i < b->arg_count; i++) {
        JSClosureVar *cv = &s->closure_var[s->closure_var_count++];
        vd = &b->vardefs[i];
        cv->closure_type = JS_CLOSURE_ARG;
        cv->is_const = false;
        cv->is_lexical = false;
        cv->var_kind = JS_VAR_NORMAL;
        cv->var_idx = i;
        cv->var_name = JS_DupAtom(ctx, vd->var_name);
    }
    /* non-lexical locals */
    for (i = 0; i < b->var_count; i++) {
        vd = &b->vardefs[b->arg_count + i];
        if (vd->scope_level == 0 && vd->var_name != JS_ATOM__ret_) {
            JSClosureVar *cv = &s->closure_var[s->closure_var_count++];
            set_closure_from_var(ctx, cv, vd, i);
        }
    }
    /* the frame's own captured closure vars */
    for (i = 0; i < b->closure_var_count; i++) {
        JSClosureVar *cv0 = &b->closure_var[i];
        JSClosureVar *cv = &s->closure_var[s->closure_var_count++];
        cv->closure_type = JS_CLOSURE_REF;
        cv->is_const = cv0->is_const;
        cv->is_lexical = cv0->is_lexical;
        cv->var_kind = cv0->var_kind;
        cv->var_idx = i;
        cv->var_name = JS_DupAtom(ctx, cv0->var_name);
    }
    return 0;
}

/* Direct eval against an arbitrary stack frame. Modeled on quickjs-ng's
   __JS_EvalInternal DIRECT path, with the frame supplied by the caller
   instead of taken from the top of stack, and the closure built by
   js_debugger_add_closure_variables above. */
static JSValue js_debugger_eval_frame(JSContext *ctx, JSValueConst this_obj, JSStackFrame *sf,
                                      const char *input, size_t input_len, const char *filename) {
    JSParseState s1, *s = &s1;
    int err;
    JSValue fun_obj;
    JSVarRef **var_refs;
    JSFunctionBytecode *b;
    JSFunctionDef *fd;
    JSObject *p;

    js_parse_init(ctx, s, input, input_len, filename, 1);
    skip_shebang(&s->buf_ptr, s->buf_end);

    p = JS_VALUE_GET_OBJ(sf->cur_func);
    b = p->u.func.function_bytecode;
    var_refs = p->u.func.var_refs;

    fd = js_new_function_def(ctx, NULL, true, false, filename, 1, 1);
    if (!fd)
        return JS_EXCEPTION;
    s->cur_func = fd;
    fd->eval_type = JS_EVAL_TYPE_DIRECT;
    fd->has_this_binding = false;
    fd->new_target_allowed = b->new_target_allowed;
    fd->super_call_allowed = b->super_call_allowed;
    fd->super_allowed = b->super_allowed;
    fd->arguments_allowed = b->arguments_allowed;
    fd->is_strict_mode = b->is_strict_mode;
    fd->func_name = JS_DupAtom(ctx, JS_ATOM__eval_);
    if (js_debugger_add_closure_variables(ctx, fd, b))
        goto fail;
    fd->module = NULL;
    s->is_module = false;
    s->allow_html_comments = true;

    push_scope(s); /* body scope */
    fd->body_scope = fd->scope_level;

    err = js_parse_program(s);
    if (err) {
    fail:
        free_token(s, &s->token);
        js_free_function_def(ctx, fd);
        return JS_EXCEPTION;
    }

    fun_obj = js_create_function(ctx, fd);
    if (JS_IsException(fun_obj))
        return JS_EXCEPTION;
    return JS_EvalFunctionInternal(ctx, fun_obj, this_obj, var_refs, sf);
}

JSValue js_debugger_evaluate(JSContext *ctx, int stack_index, JSValue expression) {
    JSStackFrame *sf;
    int cur_index = 0;

    for (sf = ctx->rt->current_stack_frame; sf != NULL; sf = sf->prev_frame) {
        if (cur_index < stack_index) {
            cur_index++;
            continue;
        }

        JSObject *f = JS_VALUE_GET_OBJ(sf->cur_func);
        if (!f || !js_class_has_bytecode(f->class_id))
            return JS_UNDEFINED;

        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, expression);
        if (!str)
            return JS_EXCEPTION;
        /* this=undefined: see the file header for why the operand-stack
           `this` heuristic was dropped */
        JSValue ret = js_debugger_eval_frame(ctx, JS_UNDEFINED, sf, str, len, "<debugger>");
        JS_FreeCString(ctx, str);
        return ret;
    }
    return JS_UNDEFINED;
}

#endif /* TNR_QJS_DEBUGGER */
