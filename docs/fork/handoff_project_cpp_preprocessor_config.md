# Проектные настройки C++-препроцессора

Статус: реализовано; целевые регрессии пройдены.
Дата: 2026-09-08.
Ветка: `v0.10.8-lora`.
Текущий коммит ветки на момент диагностики:
`f2380693033479607ccfcb8546bed1908ba307d3`.
Текущая upstream-база: `v0.10.8`
(`46ae198fc11cda80e817acbc5f5908d7c2de7032`).

## Назначение

Добавить в корневой `.codebase-memory.json` проектные списки определений
препроцессора и каталогов заголовков для C++ и CUDA. Индексатор должен загружать
их вместе с существующим `extra_extensions` и передавать во второй проход
`simplecpp` во всех полных, параллельных и инкрементальных маршрутах извлечения.

Карточка предназначена для агента без контекста исходного разговора. Она задаёт
пользовательский формат, семантику путей, владение данными, область проводки,
регрессии и границы доработки.

## Наблюдаемый дефект

Публичный контракт `cbm_extract_file_ex()` уже принимает два
NULL-терминированных массива:

- `extra_defines` в форме `NAME` или `NAME=VALUE`;
- `include_paths` с каталогами поиска заголовков.

Однако production-конвейер во всех обычных вызовах передаёт `NULL, NULL`.
Пользователь не может задать эти значения ни глобально, ни в репозитории.

Это особенно заметно на сгенерированных protobuf-заголовках. Raw-разбор
`tree-sitter-cpp` может потерять контейнер класса из-за синтаксиса, разделённого
ветвями `#ifdef/#else`. Предусмотренный preprocessed recovery способен вернуть
класс, но без каталога protobuf-заголовков не получает определения из
`google/protobuf/port_def.inc`; активный version guard с `#error` очищает output
`simplecpp`, и recovery остаётся без исходника.

## Пользовательский контракт

Проектный файл располагается в уже поддерживаемом месте:

```text
{repo_root}/.codebase-memory.json
```

Новая необязательная секция:

```json
{
  "cpp": {
    "defines": [
      "__GNUC__=16",
      "FEATURE_FLAG=1"
    ],
    "include_paths": [
      "/usr/include",
      "build/generated/include"
    ]
  }
}
```

Действуют следующие правила:

1. `cpp` применяется только к `CBM_LANG_CPP` и `CBM_LANG_CUDA`.
2. Поведение `CBM_LANG_C` этой доработкой не меняется.
3. `defines` и `include_paths` являются массивами строк и сохраняют порядок.
4. Определение принимает форму `NAME` либо `NAME=VALUE`; ведущий `-D` не нужен.
5. Пустые и синтаксически недопустимые определения пропускаются с warning.
6. Абсолютный include path используется как задано.
7. Относительный include path разрешается от `repo_root`, а не от cwd daemon-а.
8. Пустые и нестроковые элементы пропускаются с warning.
9. Отсутствующая секция сохраняет текущее поведение и передаёт `NULL, NULL`.
10. Глобальный config продолжает владеть только `extra_extensions`; новая секция
    читается из проектного `.codebase-memory.json`.
11. Индексатор не сканирует перечисленные каталоги как новые проекты. Они
    используются только для разрешения директив `#include` из индексируемого
    C++/CUDA-исходника.

Значения из файла являются проектными входными данными, а не compiler flags
самого `codebase-memory-mcp`. Специальных правил для protobuf, GCC или DSP нет.

## Владение и жизненный цикл

`cbm_userconfig_t` владеет копиями строк и NULL-терминированными массивами до
завершения pipeline run. `cbm_pipeline_ctx_t` и parallel worker contexts только
заимствуют указатели. Конфигурация загружается до запуска worker threads и
освобождается после завершения всех extraction/resolution passes.

Изменение байтов `.codebase-memory.json` уже входит в semantic manifest. Поэтому
изменение `cpp.defines` либо `cpp.include_paths` не должно требовать нового
механизма invalidation: существующее сравнение manifest обязано не переиспользовать
поколение индекса с прежним конфигом.

## Состав реализации

- `src/discover/userconfig.h`: расширить `cbm_userconfig_t` и объявить read-only
  accessors для C++/CUDA-параметров.
- `src/discover/userconfig.c`: разобрать проектную секцию `cpp`, разрешить
  относительные include paths от корня и освободить все новые данные.
- `src/pipeline/pipeline_internal.h`: передать snapshot user config через общий
  контекст pipeline.
- `src/pipeline/pipeline.c` и `src/pipeline/pipeline_incremental.c`: заполнить
  поле контекста во всех full/incremental/probe маршрутах.
- `src/pipeline/pass_definitions.c`, `pass_parallel.c`, `pass_calls.c`,
  `pass_usages.c`, `pass_semantic.c`: заменить C++/CUDA `NULL, NULL` на значения
  из текущего проектного snapshot.
- `docs/CONFIGURATION.md`: документировать формат и ограничения.
- `tests/test_userconfig.c`: проверить разбор, порядок, относительные пути,
  некорректные элементы и освобождение.
- `tests/test_pipeline.c`: проверить наблюдаемый end-to-end результат индексации
  protobuf-подобного C++-заголовка.
- `tests/test_parallel.c`: подтвердить ту же конфигурацию в parallel extraction.

## Обязательные регрессии

### Разбор конфигурации

Тест создаёт `.codebase-memory.json` с двумя defines и двумя include paths.
Он обязан проверить:

1. точное число и порядок определений;
2. точное число и порядок каталогов;
3. сохранение абсолютного пути;
4. разрешение относительного пути от корня тестового репозитория;
5. NULL-терминатор обоих массивов;
6. отсутствие настроек при отсутствующей секции;
7. пропуск элементов неверного типа без потери валидных соседей.

### Полный pipeline

Временный репозиторий содержит:

- локальный include root с `google/protobuf/port_def.inc`, который задаёт
  `PROTOBUF_VERSION`;
- проектный define, необходимый для прохождения второго guard-а;
- C++-заголовок с version guards и `#ifdef/#else`-разделённым телом метода,
  из-за которого raw AST не содержит определения класса;
- `.codebase-memory.json` с относительным include path и project define.

После `cbm_pipeline_run()` граф обязан содержать `Class Au` с исходным путём и
диапазоном. Удаление либо непроводка любого из двух списков должна оставлять
регрессию красной: `simplecpp` достигает `#error`, а raw-разбор не возвращает
контейнер класса.

### Parallel extraction

Отдельная регрессия передаёт загруженный `cbm_userconfig_t` в
`cbm_parallel_extract()` и требует тот же `Class Au`. Это защищает путь,
используемый большими проектами вроде DSP, независимо от порога выбора
последовательного или параллельного pipeline.

## Проверка

Сначала подтвердить красную фазу новыми тестами на неизменённой production
проводке. После исправления выполнить:

```bash
scripts/test.sh --suites "userconfig pipeline parallel"
scripts/lint.sh --ci
git diff --check
```

Полный `scripts/test.sh` не выполнять: пользователь явно исключил
общий тестовый маршрут из этой доработки. Её регрессионный контракт
закрывают указанные целевые suites.

Карточку проверить локальным Markdown-lint. Если локальной конфигурации нет,
использовать резервную:

```bash
markdownlint-cli2 --config /home/slader/.codex/.markdownlint-cli2.yaml \
  docs/fork/handoff_project_cpp_preprocessor_config.md
```

Перед тестами выполнить семантическую вычитку каждой изменённой функции и всех
новых тестов. После любой последующей правки повторить вычитку и относящиеся к
ней проверки.

### Фактические результаты

- Красная фаза подтверждена: до проводки production-контекста новый pipeline-тест
  не нашёл `Class Au`; результат прогона — `255 passed, 1 failed`.
- `scripts/test.sh --suites "userconfig pipeline parallel"` — `338 passed`.
- Дополнительный прогон suites `userconfig incremental` дал `161 passed, 2 failed`.
  Оба падения находятся в неизменённых `tool_snippet_include_neighbors` и
  `tool_snippet_neighbors_false`: тесты ещё ожидают JSON-поле `source`, хотя текущий
  `get_code_snippet` возвращает plain-text. Повтор с отключённым LeakSanitizer подтвердил
  те же два функциональных падения.
- `scripts/lint.sh --ci` прошёл проверки no-skips и `clang-format`, но завершился с
  кодом `2` из-за известных `cppcheck internalAstError` в неизменённых
  `internal/cbm/extract_defs.c:3027`, `internal/cbm/extract_calls.c:2694` и
  `internal/cbm/extract_unified.c:1778`.
- Markdown-lint карточки завершился без ошибок. Совместная проверка с
  `docs/CONFIGURATION.md` остаётся красной на 26 существовавших до этой доработки
  замечаниях; в новой секции замечаний нет.
- `git diff --check` завершился без ошибок.
- Полный `scripts/test.sh` не запускался по явному решению пользователя.

## Критерии готовности

- Проектный JSON загружает упорядоченные `cpp.defines` и `cpp.include_paths`.
- Относительные include paths не зависят от cwd процесса.
- Настройки применяются только к C++ и CUDA.
- Sequential, parallel и incremental extraction используют один snapshot.
- Конфиг с include path и define восстанавливает `Class Au` в end-to-end тесте.
- Изменение `.codebase-memory.json` инвалидирует прежний semantic manifest.
- Отсутствующая либо частично некорректная секция остаётся fail-open.
- Документация, целевые тесты, lint и `git diff --check` прошли либо для
  каждого ограничения зафиксирована точная причина; полный тестовый маршрут
  исключён по решению пользователя.
- Установка бинарника, перезапуск MCP и переиндексация DSP не выполнялись без
  отдельного разрешения.

## Вне границ доработки

- Поддержка отдельной секции для `CBM_LANG_C`.
- Автоматическое обнаружение системных include paths или GCC built-in defines.
- Чтение `compile_commands.json` для этой настройки.
- Специальные значения `PROTOBUF_VERSION` либо правила для `.pb.h`/`.pb.cc`.
- Изменение поведения `simplecpp` при активном `#error`.
- Изменение raw-грамматики `tree-sitter-cpp`.
- Установка собранного бинарника, reload/restart daemon-а или reindex проектов.

## Восстановление после обновления upstream

После обновления проверить:

1. формат и merge-правила `.codebase-memory.json` в `userconfig.c`;
2. сигнатуру `cbm_extract_file_ex()` и все pipeline call sites;
3. состав `cbm_pipeline_ctx_t` и parallel worker context;
4. semantic manifest digest project config;
5. обе end-to-end регрессии C++-препроцессора.

Если upstream добавил эквивалентную project-local настройку, не переносить
старую проводку механически. Сначала подтвердить совпадение формата, семантики
относительных путей, языка применения и invalidation точными тестами.
