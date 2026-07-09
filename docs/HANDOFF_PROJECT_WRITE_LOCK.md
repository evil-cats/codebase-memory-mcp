# Handoff: блокировка записи проекта

Статус: согласованный handoff для реализации.
Дата: 2026-07-09.
Контекст ветки: `0.9.0-hermione`.

## Цель

Несколько MCP-процессов могут работать с одним кэшем и одним индексируемым
проектом. Сейчас внутрипроцессная защита pipeline сериализует только один
процесс, поэтому второй MCP-бинарник или supervised worker всё ещё может войти
в путь записи той же проектной базы.

Эта карточка фиксирует согласованное направление: сначала убрать repo-local
путь `persistence`, затем добавить OS-level writer lock на уровне проекта.

## Согласованные решения

1. Не брать writer lock при старте MCP.
2. Брать writer lock только непосредственно перед операцией записи.
3. Владелец writer lock - процесс, который прямо сейчас выполняет запись.
4. Использовать non-blocking OS-level file lock.
5. Если lock занят, операция записи не запускается.
6. Если процесс-владелец умер, ОС освобождает lock при закрытии его file
   descriptor или handle.
7. Ключ lock-а - финальный `project_name`, а не физический `repo_path`.
8. Lock-файлы хранятся в активном каталоге кэша, не в worktree и не в git
   directory репозитория.
9. Поведение `name` override считается ответственностью пользователя.
10. Публичный artifact path `persistence=true` удаляется до реализации writer
    lock.

## Область Lock-А

Lock защищает запись в эту цель:

```text
<cache_dir>/<project_name>.db
```

Рекомендуемый lock path:

```text
<cache_dir>/locks/<project_name>.writer.lock
```

`cache_dir` определяется существующей логикой `CBM_CACHE_DIR` с запасным путём
`~/.cache/codebase-memory-mcp`.

Сам lock-файл не является источником истины. Он может остаться на диске после
аварийного завершения. Источник истины - OS-level advisory lock, удерживаемый на
открытом file descriptor или handle.

## Точки Кода

Текущая модель кэша и проектов уже подходит для project-keyed lock:

- `src/foundation/platform.c`: `cbm_resolve_cache_dir()` определяет
  `CBM_CACHE_DIR` или стандартное расположение кэша.
- `src/pipeline/fqn.c`: `cbm_project_name_from_path()` выводит project names из
  путей.
- `src/pipeline/pipeline.c`: `cbm_pipeline_new()` задаёт `p->project_name`;
  `cbm_pipeline_set_project_name()` применяет `name` override;
  `resolve_db_path()` пишет в `<cache_dir>/<project_name>.db`.
- `src/mcp/mcp.c`: `project_db_path()` определяет тот же project DB path для
  операций MCP.
- `src/mcp/mcp.c`: `handle_index_repository()` - явная точка входа для записи.
- `src/mcp/mcp.c`: `autoindex_thread()` - путь автоматической индексации
  сессии.
- `src/main.c`: `watcher_index_fn()` - callback переиндексации watcher.
- `src/watcher/watcher.c`: `poll_project()` уже понимает
  `CBM_WATCHER_INDEX_RETRY`, поэтому занятый writer lock может не продвигать
  watcher baseline.
- `src/mcp/index_supervisor.c`: `cbm_index_spawn_worker()` запускает per-run
  worker subprocess; worker должен брать project lock сам, потому что именно
  он выполняет запись.

## Удаление Persistence

`persistence=true` удаляется как поддерживаемая публичная возможность до
добавления lock-а.

Ожидаемая зачистка:

1. Убрать `persistence` из MCP schema для `index_repository`.
2. Убрать разбор `persistence` из `handle_index_repository()`.
3. Убрать использование `cbm_pipeline_set_persistence()` из MCP indexing.
4. Убрать пути artifact export в pipeline, которые пишут
   `.codebase-memory/graph.db.zst`.
5. Убрать artifact bootstrap как скрытый источник состояния проекта.
6. Обновить README/docs/tests так, чтобы `index_repository` больше не
   рекламировал и не создавал repo-local `.codebase-memory/` artifacts.

Если старый клиент всё ещё отправляет `persistence`, предпочтительна явная
ошибка `unsupported field`, а не молчаливое игнорирование.

## Контракт Writer Lock

Для каждой операции записи:

1. Получить или сконструировать финальный `project_name`.
2. Попытаться взять `<cache_dir>/locks/<project_name>.writer.lock`.
3. Если lock получен, выполнить запись и освободить lock в конце операции.
4. Если lock занят другим процессом:
   - явные MCP tools, которые пишут, возвращают понятную lock-busy error;
   - watcher reindex возвращает `CBM_WATCHER_INDEX_RETRY`;
   - запись в базу не начинается;
   - состояние watcher не продвигается.
5. Если lock file нельзя создать или залочить из-за неожиданной IO/platform
   ошибки, операция возвращает ошибку и не пишет.

Существующий `cbm_pipeline_lock()` остаётся полезной внутрипроцессной защитой,
но не заменяет project writer lock.

## Несколько Проектов

Один MCP-процесс может работать с несколькими проектами. Он не становится
глобальным владельцем записи.

Процесс может брать разные project locks для разных операций записи:

```text
write project A -> lock A -> write A -> release A
write project B -> lock B -> write B -> release B
```

Watcher уже хранит несколько отслеживаемых проектов как записи `project_name ->
root_path` и poll-ит их по очереди. Для каждого изменённого проекта нужно
пытаться взять только lock этого проекта.

## Supervised Worker

Per-write locking применяется в фактическом процессе записи:

- supervised `index_repository`: worker берёт project lock;
- in-process `index_repository`: MCP process берёт project lock;
- automatic session indexing: ветка, которая пишет, берёт project lock;
- watcher reindex: worker или in-process fallback берёт project lock.

Parent MCP process не должен брать lock при старте и не должен удерживать
project lock во время spawn worker.

## Name Override

Ключ lock-а - финальный `project_name` после обработки `name` override.

Следствия:

- два `repo_path`, принудительно сведённые к одному `name`, конкурируют за один
  lock;
- один `repo_path`, индексируемый под двумя разными names, использует два
  отдельных индекса и два отдельных lock-а;
- это поведение считается ответственностью пользователя.

Не добавлять отдельный repo-path lock для краевых случаев вокруг `name`
override.

## Форма Реализации

Добавить небольшой cross-platform lock module с API вокруг `project_name`, а не
`repo_path`.

Эскиз API:

```c
typedef struct cbm_project_write_lock cbm_project_write_lock_t;

typedef enum cbm_project_write_lock_result {
    CBM_PROJECT_WRITE_LOCK_OK = 0,
    CBM_PROJECT_WRITE_LOCK_BUSY = 1,
    CBM_PROJECT_WRITE_LOCK_ERROR = -1,
} cbm_project_write_lock_result_t;

cbm_project_write_lock_result_t cbm_project_write_lock_try_acquire(
    const char *project_name,
    cbm_project_write_lock_t **out,
    char *err,
    size_t err_sz);

void cbm_project_write_lock_release(cbm_project_write_lock_t *lock);
```

Заметки по реализации:

- создать `<cache_dir>/locks/` перед открытием lock-файла;
- POSIX может использовать `flock()` или `fcntl()` за границей модуля;
- Windows может использовать `LockFileEx()` за той же границей модуля;
- захват остаётся non-blocking;
- время жизни lock-а явное и короткое;
- log может включать lock path и project name на debug/info level, но
  существование lock-файла не считается ownership.

## План Тестов

Добавить регрессионное покрытие до или вместе с реализацией:

1. Два процесса не могут одновременно держать один project writer lock.
2. Два разных project names можно lock-ать независимо.
3. Lock освобождается, когда процесс-держатель завершается.
4. `index_repository` отказывается писать, когда другой процесс держит project
   lock.
5. Supervised indexing не попадает в deadlock: worker берёт lock сам.
6. Watcher reindex возвращает retry при конкуренции за lock и не продвигает
   baseline.
7. Удаление `persistence` не даёт `index_repository` создавать
   `.codebase-memory/` artifacts.
8. `name` override использует финальный project name как lock key.

## Не Цели

- Не вводить long-lived writer election при старте MCP.
- Не lock-ать физический repository directory для обычных project DB writes.
- Не решать в этой карточке все read-consistency вопросы во время DB
  replacement.
- Не оставлять `persistence=true` как compatibility shim.
