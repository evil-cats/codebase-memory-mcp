# Handoff: project writer lock для MCP write paths

Статус: handoff фактического коммита и восстановления после merge.
Дата коммита: 2026-07-09.
Ветка: `0.9.0-hermione`.
Коммит: `48238b88c2feec5a2d00c2feb87662d7e18824aa`
(`Add project writer locks for MCP writes`).
Предыдущий fork-коммит: `4826d41a9651cac4bfcd546ebe409b5836946ea1`.

## Назначение

Этот handoff описывает второй fork-коммит. Он вводит короткоживущий OS-level
writer lock для file-backed операций записи project DB и одновременно убирает
публичный путь `persistence=true`.

Проблема до коммита: `cbm_pipeline_lock()` сериализовал только один процесс.
Два MCP-процесса, два supervised worker-а или MCP process плюс watcher могли
писать один и тот же `<cache_dir>/<project>.db` параллельно. SQLite сам по себе
не заменяет project-level ownership, потому что pipeline может пересоздавать DB,
WAL/SHM и связанные file-backed данные.

## Состав Коммита

Коммит меняет 18 файлов:

- Модуль lock: `src/foundation/project_write_lock.c`,
  `src/foundation/project_write_lock.h`, `Makefile.cbm`. Новый
  cross-platform lock module добавлен в сборку.
- Пути записи MCP: `src/mcp/mcp.c`, `src/mcp/mcp.h`, `src/main.c`.
  `index_repository`, watcher reindex, autoindex, `delete_project` и
  `manage_adr` берут project writer lock.
- Режим cross-repo: `src/pipeline/pass_cross_repo.c`,
  `src/pipeline/pass_cross_repo.h`. Список target projects можно собрать до
  записи, чтобы взять locks на source и targets.
- Зачистка `persistence`: `src/pipeline/pipeline.c`,
  `src/pipeline/pipeline.h`, `src/pipeline/pipeline_incremental.c`,
  `tests/repro/repro_issue434.c`, `tests/test_artifact.c`. `persistence=true`
  больше не является публичным contract; artifact export/bootstrap убран из
  indexing path.
- Тесты: `tests/test_platform.c`, `tests/test_mcp.c`, `tests/test_watcher.c`,
  `tests/test_artifact.c`, `tests/repro/repro_issue434.c`. Поведение lock,
  отказ legacy persistence и watcher retry покрыты regression tests.

Статистика коммита: `18 files changed, 1364 insertions(+), 276 deletions(-)`.

## Контракт Writer Lock

1. Lock берётся не при старте MCP, а непосредственно перед file-backed write.
2. Ключ lock-а - финальный `project_name`, включая обработанный `name` override.
3. Lock-файл живёт в активном cache dir:

   ```text
   <cache_dir>/locks/<project_name>.writer.lock
   ```

4. Защищаемая DB живёт в:

   ```text
   <cache_dir>/<project_name>.db
   ```

5. Lock non-blocking. Если lock занят, операция записи не начинается.
6. Источник истины - OS-level advisory lock на открытом descriptor/handle, а не
   существование lock-файла на диске.
7. Если владелец процесса умер, ОС закрывает descriptor/handle и освобождает
   lock. Оставшийся lock-файл не означает ownership.
8. `CBM_PROJECT_WRITE_LOCK_BUSY` должен возвращаться как понятная `lock_busy`
   ошибка для MCP tools или как retry для watcher.
9. `CBM_PROJECT_WRITE_LOCK_ERROR` означает неожиданную IO/platform проблему;
   запись тоже не начинается.
10. `cbm_pipeline_lock()` остаётся внутрипроцессной защитой, но не заменяет
    project writer lock.

## Реализация Lock Module

`src/foundation/project_write_lock.h` задаёт API:

```c
typedef struct cbm_project_write_lock cbm_project_write_lock_t;

typedef enum {
    CBM_PROJECT_WRITE_LOCK_ERROR = -1,
    CBM_PROJECT_WRITE_LOCK_OK = 0,
    CBM_PROJECT_WRITE_LOCK_BUSY = 1,
} cbm_project_write_lock_result_t;

cbm_project_write_lock_result_t cbm_project_write_lock_try_acquire(
    const char *project_name,
    cbm_project_write_lock_t **out,
    char *err,
    size_t err_sz);

void cbm_project_write_lock_release(cbm_project_write_lock_t *lock);
```

`src/foundation/project_write_lock.c` делает следующее:

- валидирует `project_name` через `cbm_validate_project_name()`;
- создаёт `<cache_dir>/locks/`;
- на POSIX открывает lock file и берёт `flock(fd, LOCK_EX | LOCK_NB)`;
- на Windows открывает file handle и берёт `LockFileEx()` с
  `LOCKFILE_FAIL_IMMEDIATELY`;
- возвращает `BUSY` отдельно от остальных ошибок;
- освобождает lock через `cbm_project_write_lock_release()`;
- не удаляет lock-файл как часть ownership protocol.

`Makefile.cbm` должен включать `src/foundation/project_write_lock.c` в
`FOUNDATION_SRCS`.

## Реализация MCP Write Paths

### `index_repository`

`handle_index_repository()` сначала отвергает legacy `persistence`:

```json
{"status":"error","error":"unsupported field: persistence"}
```

Проверка должна стоять до supervisor gate, чтобы legacy client получал тот же
ответ и в parent, и в worker path.

После создания pipeline и применения `name` override код копирует финальный
`cbm_pipeline_project_name(p)` и берёт project writer lock. Если lock занят,
возвращается MCP tool result с `isError=true`, `status=lock_busy` и `project`.
Pipeline не запускается.

### Supervised Worker

Родительский MCP process не берёт project writer lock вокруг spawn. Lock должен
брать тот процесс, который фактически пишет DB. Поэтому:

- родительский process получает response worker-а;
- `cbm_mcp_result_is_error()` читает `isError`;
- `lock_busy` в response не считается успешной индексацией;
- watcher и autoindex превращают supervised `lock_busy` в retry/ошибку, а не в
  успех.

### Watcher Reindex

`src/main.c` берёт writer lock только в in-process fallback. Если supervised
worker path доступен, lock берёт worker. При `lock_busy` watcher получает
`CBM_WATCHER_INDEX_RETRY`, поэтому изменение остаётся pending по handoff-у
первого коммита.

### Automatic Session Indexing

`autoindex_thread()` использует тот же принцип:

- supervised worker сам берёт lock;
- in-process fallback берёт lock перед `cbm_pipeline_run()`;
- lock busy логируется как `project_writer_lock_busy`, DB write не стартует.

### `delete_project`

`delete_project` сначала проверяет, есть ли `<project>.db`. Если DB нет, он
возвращает historical `not_found` и не создаёт cache/lock scaffolding.

Если DB есть, tool берёт project writer lock, закрывает cached store для этого
project и только затем удаляет `.db`, `.db-wal`, `.db-shm`. При `lock_busy`
файлы остаются на месте.

### `manage_adr`

`manage_adr` различает file-backed store и in-memory / embedded store:

- если `cbm_store_db_path(resolved) != NULL`, write/update и legacy ADR import
  требуют project writer lock;
- если store не file-backed, lock не нужен;
- lock busy возвращает MCP error до записи ADR.

### Cross-Repo Intelligence Mode

`mode="cross-repo-intelligence"` может писать cross-project edges. Поэтому он
берёт locks не только на source project, но и на target projects.

Для `target_projects=["*"]` список projects собирается заранее через
`cbm_cross_repo_collect_projects()` и освобождается через
`cbm_cross_repo_free_project_list()`. Дубликаты project names пропускаются, чтобы
не пытаться второй раз взять тот же lock.

## Удаление `persistence=true`

Коммит переводит старый `persistence=true` из поддерживаемой возможности в
явную ошибку для legacy clients:

- schema `index_repository` больше не содержит поле `persistence`;
- `handle_index_repository()` возвращает `unsupported field: persistence`, если
  ключ присутствует в args;
- `cbm_pipeline_set_persistence()` удалён из `pipeline.h` / `pipeline.c`;
- full pipeline больше не экспортирует `.codebase-memory/graph.db.zst`;
- incremental pipeline больше не auto-updates artifact, даже если artifact уже
  был рядом с repo;
- artifact bootstrap из `.codebase-memory/graph.db.zst` при отсутствии cache DB
  убран из MCP indexing path;
- repro `#434` теперь проверяет явный отказ, а не создание artifact.

`src/pipeline/artifact.c` и `src/pipeline/artifact.h` остаются в дереве, потому
что artifact utilities и tests ещё существуют. Этот коммит убирает именно
публичный `index_repository(persistence=true)` contract.

## Проверки

Узкая проверка lock/persistence/watch paths:

```bash
make -f Makefile.cbm build/c/test-runner
build/c/test-runner platform mcp watcher artifact
make -f Makefile.cbm build/c/test-repro-runner
CBM_REPRO_ONLY=repro_issue434 build/c/test-repro-runner
```

Широкая проверка:

```bash
make -f Makefile.cbm test
```

Регрессионные проверки:

- `project_write_lock_same_project_busy`: второй lock того же project получает
  `BUSY`.
- `project_write_lock_different_projects_independent`: разные project names
  lock-аются независимо.
- `project_write_lock_released_when_owner_process_exits`: POSIX owner death
  освобождает lock без явного release.
- `mcp_index_repository_schema_omits_persistence`: schema не рекламирует
  legacy field.
- `tool_index_repository_rejects_legacy_persistence`: `persistence=true`
  возвращает явную ошибку и не создаёт `.codebase-memory`.
- `tool_index_repository_lock_busy`: занятый project lock блокирует indexing.
- `tool_index_repository_name_override_uses_lock_key`: lock key - финальный
  `name` override.
- `tool_index_repository_cross_repo_target_lock_busy`: cross-repo target lock
  тоже блокирует write.
- `tool_delete_project_lock_busy`: delete не удаляет DB под чужим lock.
- `tool_manage_adr_update_lock_busy`: ADR update не пишет под чужим lock.
- `watcher_retries_dirty_then_clean_after_index_retry`: watcher не теряет dirty
  change, если первая индексация вернула retry, а дерево стало clean до
  следующего poll.
- `repro_issue434_persistence_is_rejected`: legacy persistence теперь явно
  отклоняется.

## Восстановление После Внешнего Merge

Если внешний upstream меняет те же файлы, восстанавливай этот порядок:

1. Убедись, что `src/foundation/project_write_lock.c` и `.h` существуют, а
   `Makefile.cbm` собирает `.c` файл.
2. Проверь, что lock path строится от `cbm_resolve_cache_dir()` и финального
   `project_name`, а не от physical `repo_path`.
3. Проверь, что `index_repository` отвергает ключ `persistence` до supervisor
   gate.
4. Проверь, что `index_repository` берёт lock после `name` override и до
   `cbm_pipeline_run()`.
5. Проверь, что supervised worker сам берёт lock, а родительский process не
   держит lock во время spawn.
6. Проверь watcher: `lock_busy` должен стать `CBM_WATCHER_INDEX_RETRY`, не
   `OK`.
7. Проверь `autoindex_thread()`: in-process fallback должен брать lock;
   supervised response с `isError=true` не должен логироваться как успех.
8. Проверь `delete_project`: `not_found` остаётся no-op без создания lock dir;
   existing DB удаляется только под lock.
9. Проверь `manage_adr`: lock нужен только file-backed writes и legacy ADR
   import, не read-only/get и не in-memory store.
10. Проверь cross-repo mode: source и target project names lock-аются до
    `cbm_cross_repo_match()`, включая expanded `target_projects=["*"]`.
11. Убедись, что artifact bootstrap/export не вернулся в MCP indexing path.
12. Запусти проверки из раздела `Проверки`.

Типичные conflict points:

- upstream может вернуть `persistence` в schema как поле совместимости;
- upstream может перенести supervisor gate выше валидации legacy field;
- upstream может добавить новый write tool, который пишет file-backed DB без
  project writer lock;
- upstream может оптимизировать `delete_project` и снова брать lock до проверки
  `not_found`, создавая lock/cache dirs для no-op;
- upstream может изменить cross-repo target expansion, и locks для `["*"]`
  перестанут покрывать все target projects;
- upstream может оставить `cbm_pipeline_lock()` и счесть его достаточным. Это
  неверно для нескольких MCP processes.

## Не Цели

- Не вводить long-lived writer election при старте MCP.
- Не lock-ать physical repository directory.
- Не lock-ать read-only query tools.
- Не решать все read-consistency вопросы во время DB replacement.
- Не поддерживать `persistence=true` как compatibility shim.
- Не удалять artifact utility module целиком, если он ещё нужен другим tests или
  standalone artifact code.
