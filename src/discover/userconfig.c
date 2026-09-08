/*
 * userconfig.c — Загрузка пользовательских настроек проекта.
 *
 * Глобальный и проектный файлы объединяют `extra_extensions`, причём проектный
 * имеет приоритет. Только проектный `.codebase-memory.json` владеет секцией
 * `cpp`: она передаёт C++/CUDA extraction workers определения препроцессора и
 * каталоги заголовков. Относительные пути разрешаются от корня репозитория.
 * Некорректные значения пропускаются без отказа всего индексирования.
 */
#include "discover/userconfig.h"
#include "cbm.h" /* CBMLanguage, CBM_LANG_* */
#include "foundation/constants.h"
#include "foundation/platform.h" /* cbm_safe_getenv */
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"

enum { MAX_CONFIG_SIZE = 65536, MAX_CPP_CONFIG_VALUES = 1024 };
#include "foundation/log.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Process-global user config pointer ──────────────────────────── */

static const cbm_userconfig_t *g_userconfig = NULL;

static void userconfig_source_digest(const char *state, const void *bytes, size_t len,
                                     char out[CBM_SHA256_HEX_LEN + 1]) {
    static const char domain[] = "cbm-userconfig-source-v1";
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    cbm_sha256_update(&sha, domain, sizeof(domain));
    cbm_sha256_update(&sha, state, strlen(state) + 1);
    if (bytes && len > 0) {
        cbm_sha256_update(&sha, bytes, len);
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&sha, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

void cbm_set_user_lang_config(const cbm_userconfig_t *cfg) {
    g_userconfig = cfg;
}

const cbm_userconfig_t *cbm_get_user_lang_config(void) {
    return g_userconfig;
}

/* ── Language name → enum table ──────────────────────────────────── */

/*
 * Reverse-mapping from lowercase language name strings to CBMLanguage.
 * Covers all names exposed by cbm_language_name() plus common aliases.
 */
typedef struct {
    const char *name; /* lowercase */
    CBMLanguage lang;
} lang_name_entry_t;

static const lang_name_entry_t LANG_NAME_TABLE[] = {
    {"go", CBM_LANG_GO},
    {"python", CBM_LANG_PYTHON},
    {"javascript", CBM_LANG_JAVASCRIPT},
    {"typescript", CBM_LANG_TYPESCRIPT},
    {"tsx", CBM_LANG_TSX},
    {"rust", CBM_LANG_RUST},
    {"java", CBM_LANG_JAVA},
    {"c++", CBM_LANG_CPP},
    {"cpp", CBM_LANG_CPP},
    {"c#", CBM_LANG_CSHARP},
    {"csharp", CBM_LANG_CSHARP},
    {"php", CBM_LANG_PHP},
    {"lua", CBM_LANG_LUA},
    {"scala", CBM_LANG_SCALA},
    {"kotlin", CBM_LANG_KOTLIN},
    {"ruby", CBM_LANG_RUBY},
    {"c", CBM_LANG_C},
    {"bash", CBM_LANG_BASH},
    {"sh", CBM_LANG_BASH},
    {"zig", CBM_LANG_ZIG},
    {"elixir", CBM_LANG_ELIXIR},
    {"haskell", CBM_LANG_HASKELL},
    {"ocaml", CBM_LANG_OCAML},
    {"objective-c", CBM_LANG_OBJC},
    {"objc", CBM_LANG_OBJC},
    {"swift", CBM_LANG_SWIFT},
    {"dart", CBM_LANG_DART},
    {"perl", CBM_LANG_PERL},
    {"groovy", CBM_LANG_GROOVY},
    {"erlang", CBM_LANG_ERLANG},
    {"r", CBM_LANG_R},
    {"html", CBM_LANG_HTML},
    {"css", CBM_LANG_CSS},
    {"scss", CBM_LANG_SCSS},
    {"yaml", CBM_LANG_YAML},
    {"toml", CBM_LANG_TOML},
    {"hcl", CBM_LANG_HCL},
    {"terraform", CBM_LANG_HCL},
    {"sql", CBM_LANG_SQL},
    {"dockerfile", CBM_LANG_DOCKERFILE},
    {"clojure", CBM_LANG_CLOJURE},
    {"f#", CBM_LANG_FSHARP},
    {"fsharp", CBM_LANG_FSHARP},
    {"julia", CBM_LANG_JULIA},
    {"vimscript", CBM_LANG_VIMSCRIPT},
    {"nix", CBM_LANG_NIX},
    {"common lisp", CBM_LANG_COMMONLISP},
    {"commonlisp", CBM_LANG_COMMONLISP},
    {"lisp", CBM_LANG_COMMONLISP},
    {"elm", CBM_LANG_ELM},
    {"fortran", CBM_LANG_FORTRAN},
    {"cuda", CBM_LANG_CUDA},
    {"cobol", CBM_LANG_COBOL},
    {"verilog", CBM_LANG_VERILOG},
    {"emacs lisp", CBM_LANG_EMACSLISP},
    {"emacslisp", CBM_LANG_EMACSLISP},
    {"json", CBM_LANG_JSON},
    {"xml", CBM_LANG_XML},
    {"markdown", CBM_LANG_MARKDOWN},
    {"makefile", CBM_LANG_MAKEFILE},
    {"cmake", CBM_LANG_CMAKE},
    {"protobuf", CBM_LANG_PROTOBUF},
    {"graphql", CBM_LANG_GRAPHQL},
    {"vue", CBM_LANG_VUE},
    {"svelte", CBM_LANG_SVELTE},
    {"meson", CBM_LANG_MESON},
    {"glsl", CBM_LANG_GLSL},
    {"ini", CBM_LANG_INI},
    {"matlab", CBM_LANG_MATLAB},
    {"mojo", CBM_LANG_MOJO},
    {"lean", CBM_LANG_LEAN},
    {"form", CBM_LANG_FORM},
    {"magma", CBM_LANG_MAGMA},
    {"wolfram", CBM_LANG_WOLFRAM},
};

#define LANG_NAME_TABLE_SIZE (sizeof(LANG_NAME_TABLE) / sizeof(LANG_NAME_TABLE[0]))

/*
 * Parse a language string (case-insensitive) to a CBMLanguage enum.
 * Returns CBM_LANG_COUNT if the string is not recognized.
 */
static CBMLanguage lang_from_string(const char *s) {
    if (!s || !s[0]) {
        return CBM_LANG_COUNT;
    }

    /* Build a lowercase copy for comparison */
    char lower[CBM_SZ_64];
    size_t i;
    for (i = 0; i < sizeof(lower) - SKIP_ONE && s[i]; i++) {
        lower[i] = (char)tolower((unsigned char)s[i]);
    }
    lower[i] = '\0';

    for (size_t j = 0; j < LANG_NAME_TABLE_SIZE; j++) {
        if (strcmp(LANG_NAME_TABLE[j].name, lower) == 0) {
            return LANG_NAME_TABLE[j].lang;
        }
    }
    return CBM_LANG_COUNT;
}

/* ── Config directory helper ─────────────────────────────────────── */

/* cbm_app_config_dir() is now in platform.c (cross-platform). */

/* ── JSON parsing ────────────────────────────────────────────────── */

/*
 * Parse extra_extensions from a yyjson object root.
 * Appends valid entries to *entries / *count (growing via realloc).
 * Project-level entries (from_project=true) are appended after global
 * entries so that a later dedup pass can prefer project values.
 *
 * Returns 0 on success, -1 on alloc failure.
 */
static int parse_extra_extensions(yyjson_val *root, cbm_userext_t **entries, int *count,
                                  const char *source_label) {
    if (!yyjson_is_obj(root)) {
        cbm_log_warn("userconfig.bad_root", "file", source_label);
        return 0;
    }

    yyjson_val *extra = yyjson_obj_get(root, "extra_extensions");
    if (!extra) {
        return 0; /* key absent — fine */
    }
    if (!yyjson_is_obj(extra)) {
        cbm_log_warn("userconfig.bad_extra_extensions", "file", source_label);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(extra, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);

        const char *ext_str = yyjson_get_str(key);
        const char *lang_str = yyjson_get_str(val);

        if (!ext_str || !lang_str) {
            cbm_log_warn("userconfig.skip_non_string", "file", source_label);
            continue;
        }

        /* Extension must start with '.' */
        if (ext_str[0] != '.') {
            cbm_log_warn("userconfig.skip_bad_ext", "file", source_label, "ext", ext_str);
            continue;
        }

        CBMLanguage lang = lang_from_string(lang_str);
        if (lang == CBM_LANG_COUNT) {
            cbm_log_warn("userconfig.unknown_lang", "file", source_label, "lang", lang_str);
            continue; /* fail-open: skip unknown languages */
        }

        /* Grow the array */
        cbm_userext_t *tmp = realloc(*entries, (size_t)(*count + SKIP_ONE) * sizeof(cbm_userext_t));
        if (!tmp) {
            return CBM_NOT_FOUND;
        }
        *entries = tmp;

        char *ext_copy = strdup(ext_str);
        if (!ext_copy) {
            return CBM_NOT_FOUND;
        }

        (*entries)[*count].ext = ext_copy;
        (*entries)[*count].lang = lang;
        (*count)++;
    }
    return 0;
}

/* Проверить имя command-line определения без интерпретации его значения.
 * Правая часть после первого `=` остаётся данными для simplecpp. */
static bool cpp_define_is_valid(const char *value) {
    if (!value || (!isalpha((unsigned char)value[0]) && value[0] != '_')) {
        return false;
    }
    for (const char *p = value + 1; *p && *p != '='; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return strchr(value, '\n') == NULL && strchr(value, '\r') == NULL;
}

/* Распознать POSIX, UNC и DOS absolute paths независимо от host-платформы:
 * один config может переноситься вместе с cross-platform репозиторием. */
static bool config_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || (path[0] == '\\' && path[1] == '\\')) {
        return true;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

/* Relative include paths принадлежат проекту, поэтому их нельзя оставлять
 * зависимыми от cwd долгоживущего daemon-а. Результат владеется вызывающим. */
static char *resolve_cpp_include_path(const char *repo_path, const char *path) {
    if (config_path_is_absolute(path) || !repo_path || !repo_path[0]) {
        return strdup(path);
    }
    size_t root_len = strlen(repo_path);
    size_t path_len = strlen(path);
    bool has_separator =
        root_len > 0 && (repo_path[root_len - 1] == '/' || repo_path[root_len - 1] == '\\');
    if (path_len > SIZE_MAX - 2U || root_len > SIZE_MAX - path_len - 2U) {
        return NULL;
    }
    size_t total = root_len + (has_separator ? 0U : 1U) + path_len + 1U;
    char *resolved = malloc(total);
    if (!resolved) {
        return NULL;
    }
    snprintf(resolved, total, has_separator ? "%s%s" : "%s/%s", repo_path, path);
    return resolved;
}

static void free_string_array(char **items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

/* Разобрать один список `cpp`, сохранив порядок валидных элементов и обязательный
 * NULL-терминатор. Ошибка allocation прерывает загрузку, ошибки данных — нет. */
static int parse_cpp_string_array(yyjson_val *cpp, const char *key, const char *repo_path,
                                  const char *source_label, bool include_paths, char ***out_items,
                                  int *out_count) {
    yyjson_val *array = yyjson_obj_get(cpp, key);
    if (!array) {
        return 0;
    }
    if (!yyjson_is_arr(array)) {
        cbm_log_warn("userconfig.bad_cpp_array", "file", source_label, "key", key);
        return 0;
    }
    size_t candidate_count = yyjson_arr_size(array);
    if (candidate_count > MAX_CPP_CONFIG_VALUES) {
        cbm_log_warn("userconfig.cpp_array_too_large", "file", source_label, "key", key);
        candidate_count = MAX_CPP_CONFIG_VALUES;
    }
    char **items = calloc(candidate_count + 1U, sizeof(char *));
    if (!items) {
        return CBM_NOT_FOUND;
    }

    yyjson_arr_iter iter;
    yyjson_arr_iter_init(array, &iter);
    yyjson_val *item;
    int count = 0;
    size_t visited = 0;
    while (visited < candidate_count && (item = yyjson_arr_iter_next(&iter)) != NULL) {
        visited++;
        const char *value = yyjson_get_str(item);
        if (!value || !value[0] || (!include_paths && !cpp_define_is_valid(value))) {
            cbm_log_warn("userconfig.skip_cpp_value", "file", source_label, "key", key);
            continue;
        }
        char *copy = include_paths ? resolve_cpp_include_path(repo_path, value) : strdup(value);
        if (!copy) {
            free_string_array(items, count);
            return CBM_NOT_FOUND;
        }
        items[count++] = copy;
    }
    if (count == 0) {
        free(items);
        items = NULL;
    }
    *out_items = items;
    *out_count = count;
    return 0;
}

/* Секция `cpp` является проектным compilation context; глобальный config её не
 * читает, чтобы один пользовательский default не менял все репозитории. */
static int parse_project_cpp_config(yyjson_val *root, cbm_userconfig_t *cfg, const char *repo_path,
                                    const char *source_label) {
    yyjson_val *cpp = yyjson_obj_get(root, "cpp");
    if (!cpp) {
        return 0;
    }
    if (!yyjson_is_obj(cpp)) {
        cbm_log_warn("userconfig.bad_cpp", "file", source_label);
        return 0;
    }
    int rc = parse_cpp_string_array(cpp, "defines", repo_path, source_label, false,
                                    &cfg->cpp_defines, &cfg->cpp_define_count);
    if (rc != 0) {
        return rc;
    }
    return parse_cpp_string_array(cpp, "include_paths", repo_path, source_label, true,
                                  &cfg->cpp_include_paths, &cfg->cpp_include_path_count);
}

/* Прочитать один config и вычислить digest его фактического состояния. Global
 * разбирает только extra_extensions, project дополнительно заполняет cpp
 * snapshot. Отсутствующий или повреждённый файл пропускается; allocation
 * failure возвращается вызывающему. */
static int load_config_file(const char *path, cbm_userext_t **entries, int *count,
                            char source_sha256[CBM_SHA256_HEX_LEN + 1],
                            cbm_userconfig_t *project_cfg, const char *repo_path) {
    userconfig_source_digest("missing-or-unreadable", NULL, 0, source_sha256);
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return 0; /* file absent — silently ignore */
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        userconfig_source_digest("seek-error", NULL, 0, source_sha256);
        return 0;
    }
    long len = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        userconfig_source_digest("seek-error", NULL, 0, source_sha256);
        return 0;
    }

    if (len <= 0 || len > MAX_CONFIG_SIZE) {
        (void)fclose(f);
        if (len > MAX_CONFIG_SIZE) {
            cbm_log_warn("userconfig.file_too_large", "path", path);
            userconfig_source_digest("oversized", NULL, 0, source_sha256);
        } else {
            userconfig_source_digest("empty", NULL, 0, source_sha256);
        }
        return 0;
    }

    char *buf = malloc((size_t)len + SKIP_ONE);
    if (!buf) {
        (void)fclose(f);
        return CBM_NOT_FOUND;
    }

    size_t nread = fread(buf, SKIP_ONE, (size_t)len, f);
    (void)fclose(f);
    if (nread > (size_t)len) {
        nread = (size_t)len;
    }
    buf[nread] = '\0';
    userconfig_source_digest("present", buf, nread, source_sha256);

    yyjson_doc *doc = yyjson_read(buf, nread, 0);
    free(buf);

    if (!doc) {
        cbm_log_warn("userconfig.corrupt_json", "path", path);
        return 0; /* corrupt JSON — silently ignore (fail-open) */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    int rc = parse_extra_extensions(root, entries, count, path);
    if (rc == 0 && project_cfg && yyjson_is_obj(root)) {
        rc = parse_project_cpp_config(root, project_cfg, repo_path, path);
    }
    yyjson_doc_free(doc);
    return rc;
}

/* ── Public API ──────────────────────────────────────────────────── */

/* Загрузить единый snapshot до запуска extraction workers. Строки и массивы
 * остаются стабильны до парного cbm_userconfig_free(). */
cbm_userconfig_t *cbm_userconfig_load(const char *repo_path) {
    cbm_userconfig_t *cfg = calloc(CBM_ALLOC_ONE, sizeof(cbm_userconfig_t));
    if (!cfg) {
        return NULL;
    }

    cbm_userext_t *entries = NULL;
    int count = 0;

    /* ── Step 1: Load global config ── */
    enum { PATH_BUF_SZ = 1280 };
    const char *cfg_base = cbm_app_config_dir();
    const char *cfg_fallback = cfg_base ? cfg_base : "/tmp";
    char global_path[PATH_BUF_SZ];
    snprintf(global_path, sizeof(global_path), "%s/codebase-memory-mcp/config.json", cfg_fallback);

    if (load_config_file(global_path, &entries, &count, cfg->global_source_sha256, NULL, NULL) !=
        0) {
        for (int i = 0; i < count; i++) {
            free(entries[i].ext);
        }
        free(entries);
        free(cfg);
        return NULL;
    }

    int global_count = count; /* entries[0..global_count) are from global */

    /* ── Step 2: Load project config ── */
    userconfig_source_digest("not-applicable", NULL, 0, cfg->project_source_sha256);
    if (repo_path && repo_path[0]) {
        char project_path[PATH_BUF_SZ];
        snprintf(project_path, sizeof(project_path), "%s/.codebase-memory.json", repo_path);

        if (load_config_file(project_path, &entries, &count, cfg->project_source_sha256, cfg,
                             repo_path) != 0) {
            /* Free already-allocated entries */
            for (int i = 0; i < count; i++) {
                free(entries[i].ext);
            }
            free(entries);
            free_string_array(cfg->cpp_defines, cfg->cpp_define_count);
            free_string_array(cfg->cpp_include_paths, cfg->cpp_include_path_count);
            free(cfg);
            return NULL;
        }
    }

    /*
     * ── Step 3: Dedup — project entries win over global ──
     *
     * For any extension that appears in both global (indices 0..global_count)
     * and project (indices global_count..count), remove the global entry by
     * replacing it with the last global entry (order-insensitive dedup).
     */
    for (int p = global_count; p < count; p++) {
        for (int g = 0; g < global_count; g++) {
            if (entries[g].ext && strcmp(entries[g].ext, entries[p].ext) == 0) {
                /* Remove global entry: overwrite with last global entry */
                free(entries[g].ext);
                entries[g] = entries[global_count - SKIP_ONE];
                entries[global_count - SKIP_ONE].ext = NULL; /* mark as consumed */
                global_count--;
                break;
            }
        }
    }

    /*
     * Compact: remove any NULL-ext slots left by the dedup step.
     * (Those are the consumed "last global" entries.)
     */
    int write_idx = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].ext != NULL) {
            entries[write_idx++] = entries[i];
        }
    }
    count = write_idx;

    cfg->entries = entries;
    cfg->count = count;
    return cfg;
}

CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext) {
    if (!cfg || !ext || !ext[0]) {
        return CBM_LANG_COUNT;
    }
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].ext && strcmp(cfg->entries[i].ext, ext) == 0) {
            return cfg->entries[i].lang;
        }
    }
    return CBM_LANG_COUNT;
}

const char **cbm_userconfig_preprocessor_defines(const cbm_userconfig_t *cfg,
                                                 CBMLanguage language) {
    if (!cfg || (language != CBM_LANG_CPP && language != CBM_LANG_CUDA) ||
        cfg->cpp_define_count == 0) {
        return NULL;
    }
    return (const char **)cfg->cpp_defines;
}

const char **cbm_userconfig_preprocessor_include_paths(const cbm_userconfig_t *cfg,
                                                       CBMLanguage language) {
    if (!cfg || (language != CBM_LANG_CPP && language != CBM_LANG_CUDA) ||
        cfg->cpp_include_path_count == 0) {
        return NULL;
    }
    return (const char **)cfg->cpp_include_paths;
}

void cbm_userconfig_free(cbm_userconfig_t *cfg) {
    if (!cfg) {
        return;
    }
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].ext);
    }
    free(cfg->entries);
    free_string_array(cfg->cpp_defines, cfg->cpp_define_count);
    free_string_array(cfg->cpp_include_paths, cfg->cpp_include_path_count);
    free(cfg);
}
