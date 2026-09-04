#ifndef CBM_HELPERS_H
#define CBM_HELPERS_H

#include "cbm.h"

// Portable memmem: find first occurrence of `needle` (needle_len bytes) within
// `haystack` (haystack_len bytes). Returns a pointer into haystack, or NULL.
// Hand-rolled so it compiles identically on all platforms (GNU/BSD-only
// memmem is unavailable under msys2-clang on Windows).
void *cbm_memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);

// Extract text of a node from source. Returns arena-allocated string.
char *cbm_node_text(CBMArena *a, TSNode node, const char *source);

// Check if a string is a language keyword (should be skipped as callee/usage).
bool cbm_is_keyword(const char *name, CBMLanguage lang);

// Check if a name is a builtin we mint a real graph node for, so a CALL to it
// must NOT be keyword-filtered out of call extraction (the LSP resolves it to
// the injected builtin node and forms a CALLS edge). Narrower than
// cbm_is_keyword: it only covers builtins with a target node, so un-filtering
// them cannot produce a node-less / Module-sourced edge. The Python set MUST
// stay in sync with kPyBuiltinNodes in internal/cbm/lsp/py_builtins.c.
bool cbm_is_resolvable_builtin(const char *name, CBMLanguage lang);

// Classify a string literal as URL, config, or neither.
// Returns CBM_STRREF_URL (0), CBM_STRREF_CONFIG (1), or -1 for neither.
int cbm_classify_string(const char *str, int len);

// Check if a name is exported per language convention.
bool cbm_is_exported(const char *name, CBMLanguage lang);

// Check if a file is a test file based on path and language.
bool cbm_is_test_file(const char *rel_path, CBMLanguage lang);

// Find the innermost enclosing function node by walking parent chain.
// Returns a null node if none found.
TSNode cbm_find_enclosing_func(TSNode node, CBMLanguage lang);

// Возвращает локальный QN внешней функции либо `module_qn`, если функции нет.
// Использует весь контекст, чтобы receiver Go, `impl` Rust и out-of-line C++
// разрешались тем же алгоритмом, что и определения.
const char *cbm_enclosing_func_qn(CBMExtractCtx *ctx, TSNode node);

// Cached version: uses ctx->ef_cache to avoid repeated parent-chain walks.
const char *cbm_enclosing_func_qn_cached(CBMExtractCtx *ctx, TSNode node);

// Общий защитный предел обхода declarator для C/C++/CUDA/GLSL. Он ограничивает
// патологическую глубину AST, а не число поддерживаемых уровней владельца.
// DECLARATOR_DEPTH_LIMIT в extract_defs.c выводится из этого значения.
#define CBM_DECLARATOR_DEPTH_LIMIT 8

// Возвращает конечный узел имени из declarator функции C/C++/CUDA/GLSL либо
// пустой TSNode. Обходит как обёртки declarator, так и всю правовложенную цепочку
// qualified_identifier у out-of-line метода. Общий алгоритм удерживает QN
// определений, вызовов и семантических записей согласованными (#438).
TSNode cbm_resolve_c_declarator_name_node(TSNode func_node);

// Convert a resolved function/method name node to its name string, normalizing a
// C++ conversion-operator's `operator_cast` node (which spans the full
// "operator bool() const") down to "operator bool". Shared by the defs and
// unified extractors so the def name and call-scope QN agree.
// Also strips the surrounding quotes from a Nix quoted attrpath segment, so
// `"kebab-case" = a: a;` is named kebab-case rather than "kebab-case". Takes the
// language for that reason; every caller must pass ctx->language.
char *cbm_func_name_node_text(CBMArena *a, TSNode name_node, const char *source, CBMLanguage lang);

// ── Nix attrpath helpers ──
// A Nix binding's name is a PATH (`a.b.c = …`) whose segments may be quoted or
// interpolated. Shared by the defs and unified (call-scope) extractors so both
// derive the same name and the same scope prefix — divergence makes a CALLS edge
// name a source node that does not exist, and it is dropped at write.

// Strip one matching pair of surrounding double quotes, in place.
void cbm_nix_strip_attr_quotes(char *text);

// True when an attrpath segment contains a `${...}` interpolation and therefore
// has no statically knowable name.
bool cbm_nix_attr_is_interpolated(TSNode attr);

// The leaf segment of an attrpath — the name. Null node for an empty attrpath.
TSNode cbm_nix_attrpath_last_attr(TSNode attrpath);

// The scope prefix of an attrpath: all segments but the leaf, quote-stripped and
// dot-joined, so `a.b.fn = …` qualifies identically to `a = { b = { fn = …; }; }`.
// NULL for a single-segment path, or when a leading segment is interpolated.
const char *cbm_nix_attrpath_scope(CBMArena *a, TSNode attrpath, const char *source);

// True when a Nix `binding`'s value is an attribute set — the binding names a
// scope rather than defining a value. Excludes let-bindings and lambda values.
bool cbm_nix_binding_is_attrset_scope(TSNode node);

// The scope QN contributed by a Nix `binding` whose value is an attribute set.
// Called by BOTH extract_defs.c and extract_unified.c, which carry separate
// compute_class_qn implementations — sharing this makes a def/call-scope QN
// mismatch (which silently drops the CALLS edge) structurally impossible.
const char *cbm_nix_binding_scope_qn(CBMExtractCtx *ctx, TSNode node, const char *saved_enclosing);

// The QN-relative name of a Nix binding — its attrpath scope joined to `name`.
// Callers prepend the enclosing attrset scope (or the module QN), so a dotted
// attrpath and an enclosing attrset compose into one qualified name.
const char *cbm_nix_qn_name(CBMArena *a, TSNode func_node, const char *source, const char *name);

// Сформировать стабильный qualified_name перегрузки C++/CUDA. callable_name уже
// содержит принятую в графе квалификацию проекта, файла, пространства имён и класса.
// В каноническую часть входят типы параметров, cv/ref-квалификаторы, параметры
// шаблона и ограничения; имена параметров, значения по умолчанию, возвращаемый
// тип и noexcept намеренно исключаются. wrapper_node может быть
// template_declaration, а callable_node — вложенным function_definition/declaration.
const char *cbm_cpp_callable_qualified_name(CBMArena *a, const char *callable_name,
                                            TSNode wrapper_node, TSNode callable_node,
                                            const char *source);

// То же построение идентичности, но дополнительно возвращает позиционный
// NULL-terminated массив канонических типов параметров. Он нужен LSP-реестру:
// общий def.param_types исторически отбрасывал встроенные типы и повторы.
const char *cbm_cpp_callable_identity(CBMArena *a, const char *callable_name, TSNode wrapper_node,
                                      TSNode callable_node, const char *source,
                                      const char ***param_types_out);

// Resolve a function/method definition node's NAME node across all ~130 grammars
// (generic `name` field, arrow→declarator, C/C++ declarator chain, plus the many
// per-language quirks: Fortran subroutine, SCSS mixin, SQL create_function, R,
// PowerShell, Ada, the Lisp/FP family, etc.). Defined in extract_defs.c. Shared by
// the defs, calls, and unified extractors so all three agree on enclosing-function
// naming — drift between private copies caused the Module-mis-attribution of
// gap #3 (and #438 for the C-declarator case).
TSNode cbm_resolve_func_name(TSNode node, CBMLanguage lang);

// Возвращает полный QN владельца out-of-line метода C++/CUDA либо `NULL` для
// свободной функции. `lexical_scope_qn` содержит уже вычисленный namespace.
const char *cbm_cpp_out_of_line_owner_qn(CBMExtractCtx *ctx, TSNode node,
                                         const char *lexical_scope_qn);

// Возвращает базовое имя типа receiver Go без указателя и generic-аргументов.
char *cbm_go_receiver_type_name(CBMArena *a, TSNode receiver, const char *source);

// Возвращает QN типа-владельца receiver-метода Go либо `NULL` для свободной функции.
const char *cbm_go_receiver_owner_qn(CBMExtractCtx *ctx, TSNode func_node);

// Возвращает QN текущего Rust-модуля с учётом встроенных предков `mod`.
const char *cbm_rust_lexical_module_qn(CBMArena *a, TSNode node, const char *source,
                                       const char *module_qn);

// Разрешает путь Rust-типа относительно `crate`, `self`, `super` или текущего модуля.
const char *cbm_rust_type_path_qn(CBMArena *a, const char *type_path, const char *module_qn,
                                  const char *lexical_module_qn);

// Возвращает канонический QN типа-владельца Rust `impl` с учётом импортов.
const char *cbm_rust_impl_owner_qn(CBMExtractCtx *ctx, TSNode impl_node,
                                   const char *lexical_module_qn);

// Find a child node by kind string.
TSNode cbm_find_child_by_kind(TSNode parent, const char *kind);

// Check if node kind matches a set of types (NULL-terminated array of strings).
bool cbm_kind_in_set(TSNode node, const char **types);

/* Namespace/module declarations that extend a qualified-name scope without
 * turning their children into class methods. Shared by definition and unified
 * walks so TS/TSX scope attribution cannot drift. */
bool cbm_is_namespace_scope_kind(CBMLanguage lang, const char *kind);

// Free the calling thread's cbm_kind_in_set bitset cache (call at thread/process
// teardown so the thread-local cache is not reported as a leak).
void cbm_kind_in_set_free_cache(void);

// Check if node has an ancestor of the given kind, within max_depth levels.
bool cbm_has_ancestor_kind(TSNode node, const char *kind, int max_depth);

// Count nodes of given kinds in subtree (for complexity metric).
int cbm_count_branching(TSNode node, const char **branching_types);

// Per-function structural complexity, computed in a single AST walk.
typedef struct {
    int cyclomatic;       // branching-node count (matches def.complexity)
    int cognitive;        // nesting-weighted flow-break count (Campbell-style approximation)
    int loop_count;       // total loop constructs in the body
    int loop_depth;       // maximum nested-loop depth — structural bottleneck proxy
    int max_access_depth; // deepest chained member/subscript access (a.b.c.d → 4) — structure smell
} cbm_complexity_t;

// Compute the metrics above in one traversal of `node`'s subtree.
// `branching_types` is the language's branching node-type set.
void cbm_compute_complexity(TSNode node, const char **branching_types, cbm_complexity_t *out);

// Is `kind` a loop construct node type? Language-agnostic curated set (for/while/
// do/foreach/repeat/loop variants). Exposed so the unified walk can track loop
// nesting at call sites without re-deriving the set.
bool cbm_is_loop_node_type(const char *kind);

// Is this a module-level node? (not nested inside function/class body)
bool cbm_is_module_level(TSNode node, CBMLanguage lang);

// Same check, but the node's PARENT is supplied directly — avoids the
// O(n) ts_node_parent rescan. Use at call sites iterating a known
// parent's children (the common case). `parent` is the parent of the
// node being classified.
bool cbm_is_module_level_p(TSNode parent, CBMLanguage lang);

// --- FQN computation ---

// Локальный QN символа: rel_path_parts.name; проект хранится отдельно.
char *cbm_fqn_compute(CBMArena *a, const char *rel_path, const char *name);

// Локальный QN модуля: путь к файлу без имени символа.
char *cbm_fqn_module(CBMArena *a, const char *rel_path);

// Для языков с каталогом-модулем (Java package, Go package) QN строится по
// содержащему каталогу без основы имени файла: `Outer.java` в корне -> "",
// `myapp/db/conn.go` -> "myapp.db". Для остальных языков результат совпадает с
// Локальный QN модуля.
char *cbm_fqn_module_source_lang(CBMArena *a, const char *rel_path, CBMLanguage lang);

// Для языков с каталогом-модулем QN символа равен QN каталога + "." + name:
// класс `Outer` из корневого `Outer.java` получает "Outer", а не "Outer.Outer".
// Для остальных языков результат совпадает с cbm_fqn_compute.
char *cbm_fqn_compute_source_lang(CBMArena *a, const char *rel_path, const char *name,
                                  CBMLanguage lang);

// Локальный QN каталога: dir_parts.
char *cbm_fqn_folder(CBMArena *a, const char *rel_dir);

/* Flatten a JS/TS `template_string` node into plain text: string fragments are
 * kept verbatim and each ${...} substitution becomes the "{}" placeholder, so
 * client-side URLs built from template literals share the canonical parameter
 * shape that server-side route paths already use. NULL when empty/oversized. */
const char *cbm_template_string_text(CBMArena *a, TSNode node, const char *source);

#endif // CBM_HELPERS_H
