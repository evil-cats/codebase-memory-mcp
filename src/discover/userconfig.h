/*
 * userconfig.h — Пользовательские настройки обнаружения и C++-препроцессора.
 *
 * Глобальный и проектный JSON задают `extra_extensions`; проектный
 * `{repo_root}/.codebase-memory.json` дополнительно может задавать
 * `cpp.defines` и `cpp.include_paths`. Объект конфигурации владеет строками до
 * конца pipeline run, а extraction workers только заимствуют указатели.
 *
 * Неизвестные и некорректные значения пропускаются с warning. Отсутствующие
 * файлы не являются ошибкой.
 */
#ifndef CBM_USERCONFIG_H
#define CBM_USERCONFIG_H

#include "cbm.h" /* CBMLanguage */
#include "foundation/sha256.h"

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    char *ext;        /* file extension including dot, e.g. ".blade.php" */
    CBMLanguage lang; /* resolved language enum */
} cbm_userext_t;

typedef struct {
    cbm_userext_t *entries; /* массив расширений во владении config */
    int count;              /* число расширений */
    char **cpp_defines;     /* владеющий NULL-терминированный NAME[=VALUE] */
    int cpp_define_count;
    char **cpp_include_paths; /* владеющие абсолютные/разрешённые пути с NULL в конце */
    int cpp_include_path_count;
    /* Digests of the exact bytes/state consumed by cbm_userconfig_load(). */
    char global_source_sha256[CBM_SHA256_HEX_LEN + 1];
    char project_source_sha256[CBM_SHA256_HEX_LEN + 1];
} cbm_userconfig_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Загрузить global и project config. Project extension mappings имеют
 * приоритет, а секция cpp читается только из project config. repo_path задаёт
 * корень для файла и относительных include paths. Результат принадлежит
 * вызывающему; NULL означает только allocation failure. */
cbm_userconfig_t *cbm_userconfig_load(const char *repo_path);

/*
 * Look up a file extension in the user config.
 * ext: extension including dot, e.g. ".blade.php"
 * Returns the mapped CBMLanguage, or CBM_LANG_COUNT if not found.
 */
CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext);

/* Вернуть проектные параметры препроцессора только для C++ и CUDA. Массивы
 * принадлежат cfg, завершаются NULL и действительны до cbm_userconfig_free(). */
const char **cbm_userconfig_preprocessor_defines(const cbm_userconfig_t *cfg, CBMLanguage language);
const char **cbm_userconfig_preprocessor_include_paths(const cbm_userconfig_t *cfg,
                                                       CBMLanguage language);

/* Free a cbm_userconfig_t returned by cbm_userconfig_load. NULL-safe. */
void cbm_userconfig_free(cbm_userconfig_t *cfg);

/* ── Integration hook ───────────────────────────────────────────── */

/*
 * Set the process-global user config that cbm_language_for_extension()
 * will consult before the built-in table.
 * cfg may be NULL to clear the override.
 * Not thread-safe — call before spawning worker threads.
 */
void cbm_set_user_lang_config(const cbm_userconfig_t *cfg);

/*
 * Get the currently active process-global user config.
 * Returns NULL if none has been set.
 * Called internally by cbm_language_for_extension().
 */
const cbm_userconfig_t *cbm_get_user_lang_config(void);

#endif /* CBM_USERCONFIG_H */
