/*
 * test_userconfig.c — Tests for project discovery and C++ preprocessing config.
 *
 * Проверяет extension mappings, project-only параметры C++/CUDA-препроцессора,
 * владение массивами и интеграцию с cbm_language_for_extension().
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "test_framework.h"
#include "discover/discover.h"
#include "discover/userconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Write a JSON file to path. Returns 0 on success. */
static int write_json(const char *path, const char *json) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs(json, f);
    fclose(f);
    return 0;
}

/* ── Tests: project config ───────────────────────────────────────── */

TEST(userconfig_project_basic) {
    /* Write a .codebase-memory.json in a temp dir */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_proj_basic", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755); /* from compat_fs.h via compat.h */

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".blade.php"), CBM_LANG_PHP);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".go"), CBM_LANG_COUNT); /* not in user config */

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* Проектная секция `cpp` сохраняет порядок defines и привязывает относительные
 * include paths к repo root; нестроковые и синтаксически неверные элементы не
 * должны удалять валидных соседей. */
TEST(userconfig_project_cpp_preprocessor) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_cpp_project", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char project_path[512];
    snprintf(project_path, sizeof(project_path), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(write_json(project_path,
                         "{\"cpp\":{"
                         "\"defines\":[\"FIRST=1\",42,\"_SECOND\",\"-DBAD=1\",\"\"],"
                         "\"include_paths\":[\"cpp_include\",false,\"\",\"/system/include\"]"
                         "}}"),
              0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);
    const char **defines = cbm_userconfig_preprocessor_defines(cfg, CBM_LANG_CPP);
    const char **include_paths = cbm_userconfig_preprocessor_include_paths(cfg, CBM_LANG_CPP);
    ASSERT_NOT_NULL(defines);
    ASSERT_NOT_NULL(include_paths);
    ASSERT_EQ(cfg->cpp_define_count, 2);
    ASSERT_STR_EQ(defines[0], "FIRST=1");
    ASSERT_STR_EQ(defines[1], "_SECOND");
    ASSERT_NULL(defines[2]);
    ASSERT_EQ(cfg->cpp_include_path_count, 2);
    char expected_relative[512];
    snprintf(expected_relative, sizeof(expected_relative), "%s/cpp_include", dir);
    ASSERT_STR_EQ(include_paths[0], expected_relative);
    ASSERT_STR_EQ(include_paths[1], "/system/include");
    ASSERT_NULL(include_paths[2]);
    ASSERT_TRUE(cbm_userconfig_preprocessor_defines(cfg, CBM_LANG_CUDA) == defines);
    ASSERT_NULL(cbm_userconfig_preprocessor_defines(cfg, CBM_LANG_C));
    ASSERT_NULL(cbm_userconfig_preprocessor_include_paths(cfg, CBM_LANG_C));

    cbm_userconfig_free(cfg);
    remove(project_path);
    PASS();
}

/* ── Tests: global config ────────────────────────────────────────── */

/* Global config продолжает управлять расширениями, но не может навязать всем
 * репозиториям единый C++ compilation context. */
TEST(userconfig_global_via_env) {
    /* Point config dir to a temp dir via the platform-appropriate env var:
     * XDG_CONFIG_HOME on Linux/macOS, APPDATA on Windows. */
    char cfg_dir[256];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/uctest_global_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", cfg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(write_json(global_path, "{\"extra_extensions\":{\".twig\":\"html\"},"
                                      "\"cpp\":{\"defines\":[\"GLOBAL_CPP=1\"],"
                                      "\"include_paths\":[\"/global/include\"]}}"),
              0);

#ifdef _WIN32
    char old_appdata[512] = "";
    cbm_safe_getenv("APPDATA", old_appdata, sizeof(old_appdata), NULL);
    cbm_setenv("APPDATA", cfg_dir, 1);
#else
    cbm_setenv("XDG_CONFIG_HOME", cfg_dir, 1);
#endif
    cbm_userconfig_t *cfg = cbm_userconfig_load(NULL); /* no project dir */
#ifdef _WIN32
    if (old_appdata[0]) {
        cbm_setenv("APPDATA", old_appdata, 1);
    } else {
        cbm_unsetenv("APPDATA");
    }
#else
    cbm_unsetenv("XDG_CONFIG_HOME");
#endif

    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".twig"), CBM_LANG_HTML);
    ASSERT_NULL(cbm_userconfig_preprocessor_defines(cfg, CBM_LANG_CPP));
    ASSERT_NULL(cbm_userconfig_preprocessor_include_paths(cfg, CBM_LANG_CPP));

    cbm_userconfig_free(cfg);
    remove(global_path);
    PASS();
}

/* ── Tests: project wins over global ────────────────────────────── */

TEST(userconfig_project_wins_over_global) {
    /* Global says .xyz → python; project says .xyz → rust */
    char xdg_dir[256];
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/uctest_priority_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", xdg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(
        write_json(global_path, "{\"extra_extensions\":{\".xyz\":\"python\"}}"),
        0);

    char proj_dir[256];
    snprintf(proj_dir, sizeof(proj_dir), "%s/uctest_priority_proj", cbm_tmpdir());
    cbm_mkdir_p(proj_dir, 0755);

    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/.codebase-memory.json", proj_dir);
    ASSERT_EQ(
        write_json(proj_path, "{\"extra_extensions\":{\".xyz\":\"rust\"}}"),
        0);

    cbm_setenv("XDG_CONFIG_HOME", xdg_dir, 1);
    cbm_userconfig_t *cfg = cbm_userconfig_load(proj_dir);
    cbm_unsetenv("XDG_CONFIG_HOME");

    ASSERT_NOT_NULL(cfg);
    /* Project definition (rust) must win */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".xyz"), CBM_LANG_RUST);

    cbm_userconfig_free(cfg);
    remove(global_path);
    remove(proj_path);
    PASS();
}

/* ── Tests: unknown language values are skipped ──────────────────── */

TEST(userconfig_unknown_lang_skipped) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_unknown_lang", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    /* "klingon" is not a valid language; ".wasm" should be silently skipped */
    ASSERT_EQ(
        write_json(proj,
                   "{\"extra_extensions\":{\".wasm\":\"klingon\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* .wasm with unknown lang → not in config */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".wasm"), CBM_LANG_COUNT);
    /* .mjs with valid lang → present */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: missing files are silently ignored ───────────────────── */

TEST(userconfig_missing_files_ok) {
    /* Point to a non-existent repo dir */
    cbm_userconfig_t *cfg = cbm_userconfig_load("/tmp/__nonexistent_repo_12345__");
    ASSERT_NOT_NULL(cfg); /* must not return NULL — just empty */
    ASSERT_EQ(cfg->count, 0);
    ASSERT_NULL(cbm_userconfig_preprocessor_defines(cfg, CBM_LANG_CPP));
    ASSERT_NULL(cbm_userconfig_preprocessor_include_paths(cfg, CBM_LANG_CPP));
    cbm_userconfig_free(cfg);
    PASS();
}

/* ── Tests: integration with cbm_language_for_extension ─────────── */

TEST(userconfig_integration_override) {
    /* Verify that setting the global config makes cbm_language_for_extension
     * respect the override. We map ".blade.php" → PHP, which is not in the
     * built-in table. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_integ", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* Before setting, .blade.php is unknown */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_COUNT);

    cbm_set_user_lang_config(cfg);
    /* After setting, .blade.php → PHP */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_PHP);
    /* Built-in extensions still work */
    ASSERT_EQ(cbm_language_for_extension(".go"), CBM_LANG_GO);

    /* Clean up global state */
    cbm_set_user_lang_config(NULL);
    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: free is NULL-safe ────────────────────────────────────── */

TEST(userconfig_free_null) {
    cbm_userconfig_free(NULL); /* must not crash */
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────── */

SUITE(userconfig) {
    RUN_TEST(userconfig_project_basic);
    RUN_TEST(userconfig_project_cpp_preprocessor);
    RUN_TEST(userconfig_global_via_env);
    RUN_TEST(userconfig_project_wins_over_global);
    RUN_TEST(userconfig_unknown_lang_skipped);
    RUN_TEST(userconfig_missing_files_ok);
    RUN_TEST(userconfig_integration_override);
    RUN_TEST(userconfig_free_null);
}
