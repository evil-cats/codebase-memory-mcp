/*
 * lsp_resolve.h — Shared LSP-override resolver for the call-edge pipeline.
 *
 * Both pipeline paths (sequential cbm_pipeline_pass_calls and parallel
 * cbm_parallel_extract → resolve_file_calls) need to look up an
 * LSP-resolved call for a given (caller, callee) pair before falling back
 * to the registry's name-based resolver. Before this header existed, each
 * pipeline carried its own copy of that lookup with divergent confidence
 * floors and slightly different match semantics — most production
 * indexing went through the parallel path with a 0.5 floor while the
 * sequential path used 0.6, so the same project produced different
 * CALLS-edge attributions depending on which pipeline mode kicked in.
 *
 * Centralising the lookup here means both pipelines admit exactly the
 * same set of LSP overrides. Each pipeline still owns its own edge
 * emission (sequential uses emit_classified_edge, parallel uses
 * emit_service_edge) — this header only does the matching.
 *
 * Inline-only: no .c file needed.
 */
#ifndef CBM_PIPELINE_LSP_RESOLVE_H
#define CBM_PIPELINE_LSP_RESOLVE_H

#include "cbm.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"

#include <stdio.h>
#include <string.h>

/* Confidence floor below which LSP-resolved calls are ignored and the
 * registry resolver is consulted instead. Locked at 0.6 per the v1
 * Python-LSP integration plan; revisit when telemetry justifies a knob.
 * Applies to every language whose LSP populates result->resolved_calls
 * (Go, C/C++, Python, PHP). */
#define CBM_LSP_CONFIDENCE_FLOOR 0.6f

/* Bare last segment of a (possibly qualified) name, splitting on the LAST
 * member/scope separator. C++ textual callees carry `::` (Class::method,
 * Ns::f) and `->` (p->run), while the LSP records dotted internal QNs
 * (Class.method). Splitting only on '.' (strrchr) leaves `Math::square`
 * and `p->run` intact, so they never match the LSP's `square`/`run` short
 * name and the type-aware strategy is silently dropped to the textual
 * registry. Treat '.', ':' and '>' as terminal separators so the bare
 * method name is recovered on BOTH the QN side (dotted, occasionally `::`
 * for template/alias scopes) and the textual side (`.`/`::`/`->`). Other
 * languages' callee names contain none of `::`/`->`, so this is a no-op
 * for them. */
static inline const char *cbm_lsp_bare_segment(const char *name) {
    if (!name) {
        return name;
    }
    const char *seg = name;
    for (const char *p = name; *p; p++) {
        /* '.' (dotted QN / Java-style member) and ':' (C++ `::`, last colon
         * wins) are member/scope separators. '>' is only a separator when it
         * closes the `->` arrow (preceded by '-'); a bare '>' closes a template
         * argument list ("identity<int>") and must NOT split, else the segment
         * would be the empty string after the trailing '>'. */
        if (*p == '.' || *p == ':' || (*p == '>' && p != name && p[-1] == '-')) {
            seg = p + SKIP_ONE;
        }
    }
    return seg;
}

/* Сопоставить текстовое имя вызова с последним сегментом разрешённого QN.
 * Канонический C++ QN продолжает имя параметрами шаблона и сигнатурой, поэтому
 * `f` должен совпадать с `f(int)`, но не с `foobar(int)`. */
static inline bool cbm_lsp_callable_leaf_matches(const char *resolved_qn,
                                                 const char *textual_name) {
    const char *textual = cbm_lsp_bare_segment(textual_name);
    if (!resolved_qn || !textual || !textual[0]) {
        return false;
    }

    size_t textual_len = strlen(textual);
    const char *template_args = strchr(textual, '<');
    if (template_args && strncmp(textual, "operator", strlen("operator")) != 0) {
        textual_len = (size_t)(template_args - textual);
    }
    for (const char *match = resolved_qn; (match = strstr(match, textual)) != NULL; match++) {
        const bool at_segment = match == resolved_qn || match[-1] == '.' || match[-1] == ':';
        const char next = match[textual_len];
        if (at_segment && (next == '\0' || next == '(' || next == '<')) {
            return true;
        }
    }
    return false;
}

/* Tail helper: return the start of the final two dot-separated segments
 * ("Class.method") or NULL when the QN is too short. */
static inline const char *cbm_pipeline_qn_class_method_tail(const char *qn) {
    if (!qn) {
        return NULL;
    }
    const char *last = strrchr(qn, '.');
    if (!last || last == qn) {
        return NULL;
    }
    const char *second = last;
    while (second > qn) {
        second--;
        if (*second == '.') {
            if (second == qn) {
                return qn;
            }
            return second + 1;
        }
    }
    return qn;
}

static inline const char *cbm_pipeline_call_callee_leaf(const char *callee_name) {
    return cbm_lsp_bare_segment(callee_name);
}

/* Gate for the unique-`Class.method`-tail fallbacks below. Tail-matching by
 * leaf is safe where class-per-file package semantics hold — the JVM
 * languages (Java/Kotlin): the declared `package` is ground truth, a class
 * name is unique within a package, and mixed Gradle/Maven source roots
 * (`src/main/java` + `src/main/kotlin`) legitimately produce path-derived
 * module QNs that disagree with the package-shaped QNs the LSP emits, so
 * the tail is the only reliable join key. In other languages the same-name
 * guarantee does not exist (Python/TS re-export shims, Go internal clones,
 * C++ template instantiations), and a single wrong-module coincidence
 * would fabricate a CALLS edge — so the fallbacks stay off there. */
static inline bool cbm_pipeline_lsp_allow_tail_match(CBMLanguage lang) {
    return lang == CBM_LANG_JAVA || lang == CBM_LANG_KOTLIN;
}

static inline int cbm_pipeline_qn_class_method_tail_eq(const char *qn, const char *tail) {
    const char *qt = cbm_pipeline_qn_class_method_tail(qn);
    return qt && tail && strcmp(qt, tail) == 0;
}

/* Look up the highest-confidence LSP-resolved call entry whose caller QN
 * matches the textual call's enclosing function and whose callee QN
 * short-name matches the textual callee. Returns a pointer into `arr`
 * or NULL if no qualifying entry exists.
 *
 * Match rule:
 *   1. exact caller_qn + callee short-name match wins first;
 *   2. if no exact caller match exists AND allow_tail_match is set
 *      (JVM callers only, see cbm_pipeline_lsp_allow_tail_match), a
 *      unique Class.method tail match between rc->caller_qn and
 *      call->enclosing_func_qn may win;
 *   3. ambiguous tails return NULL so the registry fallback stays in
 *      control.
 *
 * Qualified static callees (e.g. Perl `Pkg::sub`) are reduced to their
 * bare last segment by cbm_lsp_bare_segment before matching.
 *
 * The pointer returned aliases into `arr` and stays valid as long as the
 * underlying CBMFileResult is alive. */
static inline const CBMResolvedCall *cbm_pipeline_find_lsp_resolution(
    const CBMResolvedCallArray *arr, const CBMCall *call, bool allow_tail_match) {
    if (!arr || arr->count == 0 || !call) {
        return NULL;
    }
    if (!call->enclosing_func_qn || !call->callee_name) {
        return NULL;
    }

    const CBMResolvedCall *best_exact = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        if (strcmp(rc->caller_qn, call->enclosing_func_qn) != 0) {
            continue;
        }
        /* The call's callee_name is receiver-qualified for method/qualified
         * calls ("c.inc", "A.Helper", "Math::square", "p->run"); the LSP
         * records the resolved class-qualified callee_qn ("Class.inc"). Compare
         * the bare last segment on BOTH sides so method-dispatch resolutions
         * join — the LSP already did the receiver->type resolution, and matching
         * the full "c.inc" against "inc" would always miss, silently dropping the
         * type-aware LSP strategy to the weaker textual registry. Free-function
         * calls (bare callee_name) are unaffected. */
        const char *call_short = cbm_lsp_bare_segment(call->callee_name);
        if (!cbm_lsp_callable_leaf_matches(rc->callee_qn, call->callee_name)) {
            /* Indirect/implicit resolution: the textual callee differs from the
             * resolved callee_qn's short name. A function-pointer / DLL call's
             * callee is the pointer name (`fp`); a C++ destructor's only textual
             * anchor is the deleted operand (`p`, vs. the `T.~T` callee QN). In
             * both the LSP stashed the original textual name in `reason`. Match
             * the call site on that name, gated to those strategies so `reason`
             * is never misread as an unresolved-call diagnostic. */
            if (!(rc->reason && rc->strategy &&
                  (strcmp(rc->strategy, "lsp_func_ptr") == 0 ||
                   strcmp(rc->strategy, "lsp_dll_resolve") == 0 ||
                   strcmp(rc->strategy, "lsp_method_ref_ctor") == 0 ||
                   strcmp(rc->strategy, "lsp_method_ref_ctor_synth") == 0 ||
                   strcmp(rc->strategy, "lsp_dict_dispatch") == 0 ||
                   strcmp(rc->strategy, "lsp_import_alias") == 0 ||
                   strcmp(rc->strategy, "lsp_destructor") == 0 ||
                   strcmp(rc->strategy, "php_method_dynamic") == 0) &&
                  cbm_lsp_callable_leaf_matches(rc->reason, call_short))) {
                continue;
            }
        }
        if (!best_exact || rc->confidence > best_exact->confidence) {
            best_exact = rc;
        }
    }
    if (best_exact) {
        return best_exact;
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *call_tail = cbm_pipeline_qn_class_method_tail(call->enclosing_func_qn);
    if (!call_tail) {
        return NULL;
    }

    const CBMResolvedCall *best_tail = NULL;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (!rc->caller_qn || !rc->callee_qn) {
            continue;
        }
        if (rc->confidence < CBM_LSP_CONFIDENCE_FLOOR) {
            continue;
        }
        const char *call_leaf = cbm_pipeline_call_callee_leaf(call->callee_name);
        if (!call_leaf || !cbm_lsp_callable_leaf_matches(rc->callee_qn, call_leaf)) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(rc->caller_qn, call_tail)) {
            continue;
        }
        if (best_tail) {
            return NULL;
        }
        best_tail = rc;
    }
    return best_tail;
}

/* Сопоставляет локальный callee_qn от LSP с узлом графового буфера.
 * Сначала выполняется точный поиск. Если он не дал результата и разрешён
 * JVM-tail-match, индекс коротких имён сужает кандидатов до единственного
 * Function/Method с тем же хвостом Class.method. Проектный префикс здесь не
 * восстанавливается: конвейер и LSP обязаны использовать один локальный QN. */
static inline const cbm_gbuf_node_t *cbm_pipeline_lsp_target_node(const cbm_gbuf_t *gbuf,
                                                                  const char *callee_qn,
                                                                  bool allow_tail_match) {
    if (!gbuf || !callee_qn) {
        return NULL;
    }
    const cbm_gbuf_node_t *direct = cbm_gbuf_find_by_qn(gbuf, callee_qn);
    if (direct) {
        return direct;
    }
    if (!allow_tail_match) {
        return NULL;
    }

    const char *short_name = strrchr(callee_qn, '.');
    short_name = short_name ? short_name + SKIP_ONE : callee_qn;
    const char *callee_tail = cbm_pipeline_qn_class_method_tail(callee_qn);
    if (!callee_tail) {
        return NULL;
    }
    const cbm_gbuf_node_t **hits = NULL;
    int hit_count = 0;
    if (cbm_gbuf_find_by_name(gbuf, short_name, &hits, &hit_count) != 0 || hit_count == 0) {
        return NULL;
    }

    const cbm_gbuf_node_t *match = NULL;
    for (int i = 0; i < hit_count; i++) {
        const cbm_gbuf_node_t *cand = hits[i];
        if (!cand || !cand->label || !cand->qualified_name) {
            continue;
        }
        if (strcmp(cand->label, "Function") != 0 && strcmp(cand->label, "Method") != 0) {
            continue;
        }
        if (!cbm_pipeline_qn_class_method_tail_eq(cand->qualified_name, callee_tail)) {
            continue;
        }
        if (match) {
            return NULL;
        }
        match = cand;
    }
    return match;
}

#endif /* CBM_PIPELINE_LSP_RESOLVE_H */
