/*
 * test_fqn.c -- Tests for FQN (Fully Qualified Name) computation.
 *
 * Covers: cbm_pipeline_fqn_compute, cbm_pipeline_fqn_module,
 *         cbm_pipeline_fqn_folder, cbm_project_name_from_path.
 */
#include "test_framework.h"
#include "../internal/cbm/helpers.h"
#include "../src/pipeline/pipeline.h"
#include "../src/foundation/str_util.h"

#include <stdlib.h>
#include <string.h>

/* ── Helper: assert FQN result and free ────────────────────────── */

#define ASSERT_FQN(expr, expected)   \
    do {                             \
        char *_r = (expr);           \
        ASSERT_NOT_NULL(_r);         \
        ASSERT_STR_EQ(_r, expected); \
        free(_r);                    \
    } while (0)

/* ================================================================
 * cbm_pipeline_fqn_compute
 * ================================================================ */

/* ── Basic: project + path + name ─────────────────────────────── */

TEST(fqn_compute_omits_project_prefix) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("src/discover/discover.c", "detect_file_language"),
               "src.discover.discover.detect_file_language");
    PASS();
}

TEST(fqn_compute_basic_go) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("main.go", "main"), "main.main");
    PASS();
}

TEST(fqn_compute_basic_py) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("app.py", "run"), "app.run");
    PASS();
}

TEST(fqn_compute_basic_ts) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("server.ts", "handler"), "server.handler");
    PASS();
}

TEST(fqn_compute_basic_js) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("util.js", "parse"), "util.parse");
    PASS();
}

TEST(fqn_compute_basic_c) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("core.c", "init"), "core.init");
    PASS();
}

/* #1077/#964: File-node QNs (name=="__file__") must preserve the FULL filename
 * so sibling files sharing a stem get DISTINCT nodes. Extension stripping is
 * kept for module/symbol QNs (name!=__file__) — load-bearing for C/C++
 * declaration↔definition resolution. */
TEST(fqn_file_qn_preserves_dotfile_variants_issue1077) {
    /* .env / .env.local / .env.production all stripped to ".env" before,
     * colliding so only one File node survived per directory. */
    ASSERT_FQN(cbm_pipeline_fqn_compute(".env", "__file__"), ".env.__file__");
    ASSERT_FQN(cbm_pipeline_fqn_compute(".env.local", "__file__"), ".env.local.__file__");
    ASSERT_FQN(cbm_pipeline_fqn_compute(".env.production", "__file__"), ".env.production.__file__");
    PASS();
}

TEST(fqn_file_qn_distinguishes_same_stem_header_source_issue964) {
    /* NodeController.h and NodeController.cpp both stripped to
     * "NodeController", so the header's File node was merged into the .cpp's. */
    ASSERT_FQN(cbm_pipeline_fqn_compute("NodeController.h", "__file__"),
               "NodeController.h.__file__");
    ASSERT_FQN(cbm_pipeline_fqn_compute("NodeController.cpp", "__file__"),
               "NodeController.cpp.__file__");
    PASS();
}

TEST(fqn_module_qn_still_strips_extension) {
    /* The MODULE/symbol QN keeps stripping — unchanged by the File-QN fix. */
    ASSERT_FQN(cbm_pipeline_fqn_compute("core.c", "init"), "core.init");
    ASSERT_FQN(cbm_pipeline_fqn_module("NodeController.cpp"), "NodeController");
    PASS();
}

TEST(fqn_compute_basic_rs) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("lib.rs", "new"), "lib.new");
    PASS();
}

/* ── Nested paths ─────────────────────────────────────────────── */

TEST(fqn_compute_nested_two_levels) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("src/pkg/module.go", "FuncName"),
               "src.pkg.module.FuncName");
    PASS();
}

TEST(fqn_compute_nested_three_levels) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a/b/c/file.py", "Class"), "a.b.c.file.Class");
    PASS();
}

TEST(fqn_compute_nested_deep) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a/b/c/d/e/f/g.ts", "fn"), "a.b.c.d.e.f.g.fn");
    PASS();
}

/* ── Python __init__.py ───────────────────────────────────────── */

TEST(fqn_compute_init_py_with_name) {
    /* __init__ stripped when name is provided */
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/__init__.py", "MyClass"), "pkg.MyClass");
    PASS();
}

TEST(fqn_compute_init_py_without_name) {
    /* __init__ kept when no name (module QN for the file itself) */
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/__init__.py", NULL), "pkg.__init__");
    PASS();
}

TEST(fqn_compute_init_py_empty_name) {
    /* Empty string name also keeps __init__ */
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/__init__.py", ""), "pkg.__init__");
    PASS();
}

TEST(fqn_compute_init_py_nested) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a/b/__init__.py", "Foo"), "a.b.Foo");
    PASS();
}

TEST(fqn_compute_init_py_root) {
    /* __init__.py at root with name */
    ASSERT_FQN(cbm_pipeline_fqn_compute("__init__.py", "X"), "X");
    PASS();
}

TEST(fqn_compute_init_py_root_no_name) {
    /* __init__.py at root without name -- only project + __init__ (seg_count=2 > 1) */
    ASSERT_FQN(cbm_pipeline_fqn_compute("__init__.py", NULL), "__init__");
    PASS();
}

/* ── JS/TS index files ────────────────────────────────────────── */

TEST(fqn_compute_index_js_with_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/index.js", "render"), "pkg.render");
    PASS();
}

TEST(fqn_compute_index_js_without_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/index.js", NULL), "pkg.index");
    PASS();
}

TEST(fqn_compute_index_ts_with_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("src/index.ts", "App"), "src.App");
    PASS();
}

TEST(fqn_compute_index_ts_without_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("src/index.ts", NULL), "src.index");
    PASS();
}

TEST(fqn_compute_index_ts_empty_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("lib/index.ts", ""), "lib.index");
    PASS();
}

TEST(fqn_compute_index_root_with_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("index.js", "main"), "main");
    PASS();
}

TEST(fqn_compute_index_root_no_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("index.js", NULL), "index");
    PASS();
}

/* ── Empty / NULL parameters ──────────────────────────────────── */

TEST(fqn_compute_empty_rel_path) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("", "func"), "func");
    PASS();
}

TEST(fqn_compute_empty_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("mod.go", ""), "mod");
    PASS();
}

TEST(fqn_compute_both_empty) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("", ""), "");
    PASS();
}

TEST(fqn_compute_null_rel_path) {
    ASSERT_FQN(cbm_pipeline_fqn_compute(NULL, "fn"), "fn");
    PASS();
}

TEST(fqn_compute_null_name) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("mod.go", NULL), "mod");
    PASS();
}

TEST(fqn_compute_all_null) {
    ASSERT_FQN(cbm_pipeline_fqn_compute(NULL, NULL), "");
    PASS();
}

/* ── Backslash paths (Windows) ────────────────────────────────── */

TEST(fqn_compute_backslash_simple) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("src\\main.go", "run"), "src.main.run");
    PASS();
}

TEST(fqn_compute_backslash_nested) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a\\b\\c\\file.py", "X"), "a.b.c.file.X");
    PASS();
}

TEST(fqn_compute_backslash_mixed) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a/b\\c/d.ts", "fn"), "a.b.c.d.fn");
    PASS();
}

/* ── Multiple extensions ──────────────────────────────────────── */

TEST(fqn_compute_double_ext) {
    /* Only last extension stripped: foo.test.ts -> foo.test */
    ASSERT_FQN(cbm_pipeline_fqn_compute("foo.test.ts", "bar"), "foo.test.bar");
    PASS();
}

TEST(fqn_compute_spec_ext) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("util.spec.js", "it"), "util.spec.it");
    PASS();
}

/* ── Leading / trailing slashes ───────────────────────────────── */

TEST(fqn_compute_leading_slash) {
    /* Leading slash produces empty segment which is skipped */
    ASSERT_FQN(cbm_pipeline_fqn_compute("/src/main.go", "fn"), "src.main.fn");
    PASS();
}

TEST(fqn_compute_trailing_slash) {
    /* Trailing slash: path becomes empty after last /, extension strip is no-op */
    ASSERT_FQN(cbm_pipeline_fqn_compute("src/", "fn"), "src.fn");
    PASS();
}

TEST(fqn_compute_double_slash) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("a//b.go", "fn"), "a.b.fn");
    PASS();
}

/* ── No extension ─────────────────────────────────────────────── */

TEST(fqn_compute_no_ext) {
    ASSERT_FQN(cbm_pipeline_fqn_compute("Makefile", "target"), "Makefile.target");
    PASS();
}

/* ── Пустой локальный QN ──────────────────────────────────────── */

TEST(fqn_compute_empty_input) {
    ASSERT_FQN(cbm_pipeline_fqn_compute(NULL, NULL), "");
    PASS();
}

/* ── Non-init/index filenames that start similarly ────────────── */

TEST(fqn_compute_init_not_stripped) {
    /* __init_data__ is NOT __init__, should not be stripped */
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/__init_data__.py", "F"), "pkg.__init_data__.F");
    PASS();
}

TEST(fqn_compute_index2_not_stripped) {
    /* "indexer" is NOT "index", should not be stripped */
    ASSERT_FQN(cbm_pipeline_fqn_compute("pkg/indexer.ts", "F"), "pkg.indexer.F");
    PASS();
}

/* ================================================================
 * cbm_pipeline_fqn_module
 * ================================================================ */

TEST(fqn_module_basic) {
    ASSERT_FQN(cbm_pipeline_fqn_module("src/app.py"), "src.app");
    PASS();
}

TEST(fqn_module_go) {
    ASSERT_FQN(cbm_pipeline_fqn_module("cmd/server.go"), "cmd.server");
    PASS();
}

TEST(fqn_module_init_py) {
    /* fqn_module passes NULL name -> __init__ kept */
    ASSERT_FQN(cbm_pipeline_fqn_module("pkg/__init__.py"), "pkg.__init__");
    PASS();
}

TEST(fqn_module_index_js) {
    ASSERT_FQN(cbm_pipeline_fqn_module("components/index.js"), "components.index");
    PASS();
}

TEST(fqn_module_empty_path) {
    ASSERT_FQN(cbm_pipeline_fqn_module(""), "");
    PASS();
}

TEST(fqn_module_null_path) {
    ASSERT_FQN(cbm_pipeline_fqn_module(NULL), "");
    PASS();
}

TEST(fqn_module_root_file) {
    ASSERT_FQN(cbm_pipeline_fqn_module("foo.go"), "foo");
    PASS();
}

TEST(fqn_module_deep) {
    ASSERT_FQN(cbm_pipeline_fqn_module("a/b/c/d/e.rs"), "a.b.c.d.e");
    PASS();
}

/* ================================================================
 * cbm_pipeline_fqn_folder
 * ================================================================ */

TEST(fqn_folder_basic) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("src"), "src");
    PASS();
}

TEST(fqn_folder_nested) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("src/pkg/util"), "src.pkg.util");
    PASS();
}

TEST(fqn_folder_empty_dir) {
    ASSERT_FQN(cbm_pipeline_fqn_folder(""), "");
    PASS();
}

TEST(fqn_folder_null_dir) {
    ASSERT_FQN(cbm_pipeline_fqn_folder(NULL), "");
    PASS();
}

TEST(fqn_folder_backslash) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("src\\pkg\\util"), "src.pkg.util");
    PASS();
}

TEST(fqn_folder_backslash_mixed) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("src/pkg\\util"), "src.pkg.util");
    PASS();
}

TEST(fqn_folder_trailing_slash) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("src/pkg/"), "src.pkg");
    PASS();
}

TEST(fqn_folder_leading_slash) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("/src/pkg"), "src.pkg");
    PASS();
}

TEST(fqn_folder_double_slash) {
    ASSERT_FQN(cbm_pipeline_fqn_folder("a//b"), "a.b");
    PASS();
}

TEST(fqn_heap_and_arena_build_same_local_names) {
    CBMArena arena;
    cbm_arena_init(&arena);

    char *heap = cbm_pipeline_fqn_compute("src/discover/discover.c", "detect_file_language");
    char *arena_qn = cbm_fqn_compute(&arena, "src/discover/discover.c", "detect_file_language");
    ASSERT_NOT_NULL(heap);
    ASSERT_NOT_NULL(arena_qn);
    ASSERT_STR_EQ(heap, arena_qn);
    ASSERT_STR_EQ(arena_qn, "src.discover.discover.detect_file_language");
    free(heap);

    heap = cbm_pipeline_fqn_compute(".env.local", "__file__");
    arena_qn = cbm_fqn_compute(&arena, ".env.local", "__file__");
    ASSERT_NOT_NULL(heap);
    ASSERT_NOT_NULL(arena_qn);
    ASSERT_STR_EQ(heap, arena_qn);
    free(heap);

    cbm_arena_destroy(&arena);
    PASS();
}

TEST(fqn_directory_module_has_no_leading_separator) {
    CBMArena arena;
    cbm_arena_init(&arena);

    ASSERT_FQN(cbm_pipeline_fqn_module_dir("Outer.java", true), "");
    char *qn = cbm_fqn_compute_source_lang(&arena, "Outer.java", "Outer", CBM_LANG_JAVA);
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "Outer");

    cbm_arena_destroy(&arena);
    PASS();
}

/* ================================================================
 * cbm_project_name_from_path
 * ================================================================ */

TEST(project_name_unix_path) {
    ASSERT_FQN(cbm_project_name_from_path("/Users/dev/my-project"), "Users-dev-my-project");
    PASS();
}

TEST(project_name_windows_path) {
    ASSERT_FQN(cbm_project_name_from_path("C:\\Users\\dev\\project"), "C-Users-dev-project");
    PASS();
}

TEST(project_name_with_colons) {
    /* Colons replaced with dashes (e.g., C: drive) */
    ASSERT_FQN(cbm_project_name_from_path("C:/dev/proj"), "C-dev-proj");
    PASS();
}

TEST(project_name_multiple_slashes) {
    /* Consecutive slashes become one dash */
    ASSERT_FQN(cbm_project_name_from_path("/home///user//code"), "home-user-code");
    PASS();
}

TEST(project_name_leading_trailing_slashes) {
    /* Leading/trailing dashes trimmed */
    ASSERT_FQN(cbm_project_name_from_path("/foo/bar/"), "foo-bar");
    PASS();
}

TEST(project_name_empty) {
    ASSERT_FQN(cbm_project_name_from_path(""), "root");
    PASS();
}

TEST(project_name_null) {
    ASSERT_FQN(cbm_project_name_from_path(NULL), "root");
    PASS();
}

TEST(project_name_all_slashes) {
    /* All separators become dashes -> all trimmed -> "root" */
    ASSERT_FQN(cbm_project_name_from_path("///"), "root");
    PASS();
}

TEST(project_name_single_segment) {
    ASSERT_FQN(cbm_project_name_from_path("myproject"), "myproject");
    PASS();
}

TEST(project_name_mixed_separators) {
    /* Mix of forward slash, backslash, colon */
    ASSERT_FQN(cbm_project_name_from_path("C:\\Users/dev:proj"), "C-Users-dev-proj");
    PASS();
}

TEST(project_name_already_dashed) {
    /* Dashes are preserved, not collapsed unless from separator conversion */
    ASSERT_FQN(cbm_project_name_from_path("/my-great-project"), "my-great-project");
    PASS();
}

TEST(project_name_deep_path) {
    ASSERT_FQN(cbm_project_name_from_path("/a/b/c/d/e/f/g"), "a-b-c-d-e-f-g");
    PASS();
}

TEST(project_name_colon_only) {
    /* Single colon -> single dash -> trimmed -> root */
    ASSERT_FQN(cbm_project_name_from_path(":"), "root");
    PASS();
}

TEST(project_name_backslash_only) {
    ASSERT_FQN(cbm_project_name_from_path("\\"), "root");
    PASS();
}

TEST(project_name_consecutive_colons) {
    ASSERT_FQN(cbm_project_name_from_path("a::b"), "a-b");
    PASS();
}

/* issue #349: every derived project name must satisfy cbm_validate_project_name,
 * else the project is indexed + shown by list_projects but resolve_store rejects
 * the name → index_status/search_graph report project-not-found. */
TEST(project_name_always_validator_safe_issue349) {
    static const char *const paths[] = {
        "/home/u/my project", /* space */
        "/srv/app@v2",        /* @ */
        "/data/cxx/proj+1",   /* + */
        "/x/.hidden/repo",    /* leading-dot segment */
        "/x/a..b/repo",       /* .. sequence */
        "/Users/dev/caf\xc3\xa9"
        "app",                       /* non-ASCII (UTF-8) bytes */
        "C:\\Work\\Big Repo (2024)", /* space + parens + backslash */
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        char *name = cbm_project_name_from_path(paths[i]);
        ASSERT_NOT_NULL(name);
        ASSERT_TRUE(cbm_validate_project_name(name));
        free(name);
    }
    PASS();
}

TEST(project_name_encodes_unicode_segments_issue571) {
    char *got = cbm_project_name_from_path(
        "/Users/yunxin/Desktop/\xe5\xbc\x80\xe5\x8f\x91/"
        "\xe5\x90\x8e\xe7\xab\xaf/"
        "\xe4\xbf\xa1\xe7\xa7\x9f\xe9\xa3\x8e\xe6\x8e\xa7\xe9\x80\x9a\xe5\x90\x8e\xe7\xab\xaf");
    ASSERT_NOT_NULL(got);
    ASSERT_STR_EQ(got, "Users-yunxin-Desktop-e5bc80e58f91-e5908ee7abaf-"
                       "e4bfa1e7a79fe9a38ee68ea7e9809ae5908ee7abaf");
    ASSERT_TRUE(cbm_validate_project_name(got));
    free(got);
    PASS();
}

/* issue #624: #571 preserves non-ASCII path segments by hex-encoding each byte
 * (1 byte -> 2 hex chars), so a DEEP non-ASCII path triples in length and can
 * blow past the filesystem's 255-byte filename-component limit. Then
 * "<cache>/<name>.db" is un-openable (ENAMETOOLONG). The derived name must be
 * length-capped (with a hash suffix that disambiguates otherwise-identical
 * prefixes) while staying validator-safe — and SHORT names must NOT drift. */
#define FQN_CAP_UNDER_TEST 200
TEST(project_name_length_capped_issue624) {
    /* 开 = U+5F00, UTF-8 "\xe5\xbc\x80" (3 bytes -> 6 hex chars). 60 copies makes
     * the raw hex-encoded name ~360 chars, well past the 200-byte cap. */
    static const char KAI[] = "\xe5\xbc\x80";
    char deepA[512];
    char deepB[512];
    const char *prefix = "/Users/dev/";
    size_t p = strlen(prefix);
    memcpy(deepA, prefix, p);
    for (int i = 0; i < 60; i++) {
        memcpy(deepA + p, KAI, 3);
        p += 3;
    }
    /* deepB shares the entire deep prefix and differs ONLY in the trailing seg */
    memcpy(deepB, deepA, p);
    memcpy(deepA + p, "/alpha", 7); /* includes NUL */
    memcpy(deepB + p, "/omega", 7);

    char *nameA = cbm_project_name_from_path(deepA);
    char *nameB = cbm_project_name_from_path(deepB);
    ASSERT_NOT_NULL(nameA);
    ASSERT_NOT_NULL(nameB);

    /* (a) capped: name must fit within the filename-component budget */
    ASSERT_LTE(strlen(nameA), FQN_CAP_UNDER_TEST);
    ASSERT_LTE(strlen(nameB), FQN_CAP_UNDER_TEST);
    /* (b) still a valid project name (resolve_store must accept it) */
    ASSERT_TRUE(cbm_validate_project_name(nameA));
    ASSERT_TRUE(cbm_validate_project_name(nameB));
    /* (c) two deep paths differing only in the trailing segment must NOT
     * collide after capping — the hash suffix disambiguates them. */
    ASSERT_STR_NEQ(nameA, nameB);

    free(nameA);
    free(nameB);

    /* Short CJK path (the issue #571 case) must be UNCHANGED — no drift. */
    char *shortName = cbm_project_name_from_path(
        "/Users/yunxin/Desktop/\xe5\xbc\x80\xe5\x8f\x91/"
        "\xe5\x90\x8e\xe7\xab\xaf/"
        "\xe4\xbf\xa1\xe7\xa7\x9f\xe9\xa3\x8e\xe6\x8e\xa7\xe9\x80\x9a\xe5\x90\x8e\xe7\xab\xaf");
    ASSERT_NOT_NULL(shortName);
    ASSERT_LTE(strlen(shortName), FQN_CAP_UNDER_TEST);
    ASSERT_STR_EQ(shortName, "Users-yunxin-Desktop-e5bc80e58f91-e5908ee7abaf-"
                             "e4bfa1e7a79fe9a38ee68ea7e9809ae5908ee7abaf");
    free(shortName);

    PASS();
}

/* ================================================================
 * Suite
 * ================================================================ */

SUITE(fqn) {
    /* fqn_compute: basic extensions */
    RUN_TEST(fqn_compute_omits_project_prefix);
    RUN_TEST(fqn_compute_basic_go);
    RUN_TEST(fqn_compute_basic_py);
    RUN_TEST(fqn_compute_basic_ts);
    RUN_TEST(fqn_compute_basic_js);
    RUN_TEST(fqn_compute_basic_c);
    RUN_TEST(fqn_file_qn_preserves_dotfile_variants_issue1077);
    RUN_TEST(fqn_file_qn_distinguishes_same_stem_header_source_issue964);
    RUN_TEST(fqn_module_qn_still_strips_extension);
    RUN_TEST(fqn_compute_basic_rs);

    /* fqn_compute: nested paths */
    RUN_TEST(fqn_compute_nested_two_levels);
    RUN_TEST(fqn_compute_nested_three_levels);
    RUN_TEST(fqn_compute_nested_deep);

    /* fqn_compute: Python __init__.py */
    RUN_TEST(fqn_compute_init_py_with_name);
    RUN_TEST(fqn_compute_init_py_without_name);
    RUN_TEST(fqn_compute_init_py_empty_name);
    RUN_TEST(fqn_compute_init_py_nested);
    RUN_TEST(fqn_compute_init_py_root);
    RUN_TEST(fqn_compute_init_py_root_no_name);

    /* fqn_compute: JS/TS index files */
    RUN_TEST(fqn_compute_index_js_with_name);
    RUN_TEST(fqn_compute_index_js_without_name);
    RUN_TEST(fqn_compute_index_ts_with_name);
    RUN_TEST(fqn_compute_index_ts_without_name);
    RUN_TEST(fqn_compute_index_ts_empty_name);
    RUN_TEST(fqn_compute_index_root_with_name);
    RUN_TEST(fqn_compute_index_root_no_name);

    /* fqn_compute: empty / NULL parameters */
    RUN_TEST(fqn_compute_empty_rel_path);
    RUN_TEST(fqn_compute_empty_name);
    RUN_TEST(fqn_compute_both_empty);
    RUN_TEST(fqn_compute_null_rel_path);
    RUN_TEST(fqn_compute_null_name);
    RUN_TEST(fqn_compute_all_null);

    /* fqn_compute: backslash (Windows) */
    RUN_TEST(fqn_compute_backslash_simple);
    RUN_TEST(fqn_compute_backslash_nested);
    RUN_TEST(fqn_compute_backslash_mixed);

    /* fqn_compute: multiple extensions */
    RUN_TEST(fqn_compute_double_ext);
    RUN_TEST(fqn_compute_spec_ext);

    /* fqn_compute: leading / trailing slashes */
    RUN_TEST(fqn_compute_leading_slash);
    RUN_TEST(fqn_compute_trailing_slash);
    RUN_TEST(fqn_compute_double_slash);

    /* fqn_compute: edge cases */
    RUN_TEST(fqn_compute_no_ext);
    RUN_TEST(fqn_compute_empty_input);
    RUN_TEST(fqn_compute_init_not_stripped);
    RUN_TEST(fqn_compute_index2_not_stripped);

    /* fqn_module */
    RUN_TEST(fqn_module_basic);
    RUN_TEST(fqn_module_go);
    RUN_TEST(fqn_module_init_py);
    RUN_TEST(fqn_module_index_js);
    RUN_TEST(fqn_module_empty_path);
    RUN_TEST(fqn_module_null_path);
    RUN_TEST(fqn_module_root_file);
    RUN_TEST(fqn_module_deep);

    /* fqn_folder */
    RUN_TEST(fqn_folder_basic);
    RUN_TEST(fqn_folder_nested);
    RUN_TEST(fqn_folder_empty_dir);
    RUN_TEST(fqn_folder_null_dir);
    RUN_TEST(fqn_folder_backslash);
    RUN_TEST(fqn_folder_backslash_mixed);
    RUN_TEST(fqn_folder_trailing_slash);
    RUN_TEST(fqn_folder_leading_slash);
    RUN_TEST(fqn_folder_double_slash);
    RUN_TEST(fqn_heap_and_arena_build_same_local_names);
    RUN_TEST(fqn_directory_module_has_no_leading_separator);

    /* project_name_from_path */
    RUN_TEST(project_name_unix_path);
    RUN_TEST(project_name_windows_path);
    RUN_TEST(project_name_with_colons);
    RUN_TEST(project_name_multiple_slashes);
    RUN_TEST(project_name_leading_trailing_slashes);
    RUN_TEST(project_name_empty);
    RUN_TEST(project_name_null);
    RUN_TEST(project_name_all_slashes);
    RUN_TEST(project_name_single_segment);
    RUN_TEST(project_name_mixed_separators);
    RUN_TEST(project_name_already_dashed);
    RUN_TEST(project_name_deep_path);
    RUN_TEST(project_name_always_validator_safe_issue349);
    RUN_TEST(project_name_encodes_unicode_segments_issue571);
    RUN_TEST(project_name_length_capped_issue624);
    RUN_TEST(project_name_colon_only);
    RUN_TEST(project_name_backslash_only);
    RUN_TEST(project_name_consecutive_colons);
}
