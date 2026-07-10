# Handoff: watcher retry не теряет ожидающую индексацию

Статус: handoff фактического коммита и восстановления после merge.
Дата коммита: 2026-07-09.
Ветка: `0.9.0-hermione`.
Коммит: `4826d41a9651cac4bfcd546ebe409b5836946ea1`
(`watcher: preserve pending git changes on retry`).
База коммита: `ee68144af5453addda995a27cce8142999f318fb`.

## Назначение

Этот handoff нужен, чтобы восстановить смысл первого fork-коммита без обращения к
чату. Коммит меняет контракт watcher callback-а: обнаруженное изменение можно
считать проиндексированным только после явного `CBM_WATCHER_INDEX_OK`. Retry и
error не должны продвигать watcher baseline.

Проблема до коммита: watcher мог увидеть изменение, вызвать callback и затем
обновить `last_head` как будто индексация состоялась. Если callback фактически
пропускал работу из-за shutdown, занятого pipeline lock-а или ошибки запуска,
изменение больше не выглядело новым и могло не попасть в индекс.

## Состав Коммита

Коммит меняет 4 файла:

- `src/watcher/watcher.h`: `cbm_index_fn` возвращает
  `cbm_watcher_index_result_t` вместо `int`, чтобы разделить успех, retry и
  ошибку на уровне API watcher-а.
- `src/watcher/watcher.c`: watcher хранит git snapshot: HEAD и hash
  `git status --porcelain`, чтобы не продвигать baseline до успешной индексации
  и замечать dirty/clean переходы.
- `src/main.c`: `watcher_index_fn()` возвращает `CBM_WATCHER_INDEX_RETRY`,
  `OK` или `ERROR`; занятый pipeline и shutdown теперь означают retry, а не
  ложный успех.
- `tests/test_watcher.c`: добавлены retry callback и регрессии для HEAD retry и
  dirty -> clean, чтобы закрепить повторную индексацию отложенного изменения.

Статистика коммита: `4 files changed, 267 insertions(+), 70 deletions(-)`.

## Контракт После Коммита

`src/watcher/watcher.h` задаёт три результата callback-а:

- `CBM_WATCHER_INDEX_OK`: индексация завершилась успешно; watcher может считать
  текущий git snapshot новым baseline.
- `CBM_WATCHER_INDEX_RETRY`: индексация не запускалась или временно невозможна;
  изменение остаётся ожидающим для следующего poll.
- `CBM_WATCHER_INDEX_ERROR`: индексация завершилась ошибкой; изменение не должно
  считаться обработанным.

Текущий код после обоих fork-коммитов находится здесь:

- `src/watcher/watcher.h`: enum и typedef callback-а.
- `src/watcher/watcher.c`: `project_state_t` хранит `last_head`,
  `last_status_sig`, `last_status_valid` и текущий `index_pending`.
- `src/watcher/watcher.c`: `git_snapshot_capture()` собирает HEAD и hash status.
- `src/watcher/watcher.c`: `state_mark_indexed()` обновляет baseline только
  после `CBM_WATCHER_INDEX_OK`.
- `src/watcher/watcher.c`: `check_changes()` возвращает `true`, если есть
  ожидающая индексация, сдвиг HEAD, dirty status или смена status signature.
- `src/watcher/watcher.c`: `poll_project()` вызывает callback и продвигает
  baseline только на `OK`.
- `src/main.c`: `watcher_index_fn()` мапит shutdown и занятый pipeline lock в
  `RETRY`, успешный supervised/in-process run в `OK`, ошибку создания или
  выполнения pipeline в `ERROR`.

Важно: первый коммит в изоляции ещё не добавлял поле `index_pending`; это
усиление появилось во втором коммите для dirty -> clean после retry. Если
восстанавливаешь только первый коммит поверх старой базы, достаточно не
обновлять baseline на `RETRY`/`ERROR`. Если восстанавливаешь текущую ветку после
внешнего merge, сохраняй и `index_pending`.

## Как Должен Работать Watcher

1. Первый poll регистрирует baseline и не вызывает callback.
2. Когда HEAD изменился, рабочее дерево dirty или status signature отличается,
   watcher вызывает `index_fn`.
3. Только `CBM_WATCHER_INDEX_OK` увеличивает счётчик `reindexed`, обновляет
   `last_head`/`last_status_sig` и пересчитывает adaptive interval.
4. `CBM_WATCHER_INDEX_RETRY` не обновляет baseline. В текущем дереве он также
   ставит `index_pending=true`.
5. `CBM_WATCHER_INDEX_ERROR` тоже не обновляет baseline. В текущем дереве он
   ставит `index_pending=true`, чтобы ошибка не превращалась в потерю изменения.
6. Dirty tree считается изменением на каждом poll, потому что одна и та же
   porcelain-строка может не меняться при повторных правках файла.
7. Clean transition после успешной индексации dirty state ловится через
   `last_status_sig`.

## Проверки

Узкая проверка watcher suite:

```bash
make -f Makefile.cbm build/c/test-runner
build/c/test-runner watcher
```

Широкая регрессия:

```bash
make -f Makefile.cbm test
```

Если нужно проверить именно коммит `4826d41` в изоляции, делай это в отдельном
worktree, а не в текущем рабочем дереве:

```bash
git worktree add /tmp/cbm-watch-retry-4826d41 4826d41
make -C /tmp/cbm-watch-retry-4826d41 -f Makefile.cbm build/c/test-runner
/tmp/cbm-watch-retry-4826d41/build/c/test-runner watcher
```

Ожидаемые regression guards в `tests/test_watcher.c`:

- `watcher_retries_head_change_after_index_retry`: первый callback возвращает
  `RETRY`, следующий poll снова видит тот же HEAD change и успешно индексирует.
- `watcher_reindexes_when_dirty_worktree_returns_clean`: dirty tree индексируется,
  затем rollback к clean tree тоже вызывает reindex и после этого больше не
  повторяется.
- После второго fork-коммита дополнительно есть
  `watcher_retries_dirty_then_clean_after_index_retry`: dirty state после retry
  остаётся ожидающим даже если дерево стало clean до следующего poll.

## Восстановление После Внешнего Merge

Если внешний upstream изменил watcher, восстанавливай контракт по смыслу:

1. Найди текущий typedef callback-а watcher. Он должен возвращать enum или
   эквивалентный tagged result, где успех отделён от retry/error.
2. Найди место, где watcher принимает решение, что project изменился. Там должен
   учитываться не только HEAD, но и состояние working tree.
3. Найди место, где callback result обрабатывается после обнаруженного изменения.
   Baseline можно обновлять только на `OK`.
4. Если upstream добавил новый scheduler, debounce или async path, проведи тот же
   result contract через новый path. Нельзя возвращаться к правилу "`callback`
   вызван, значит baseline можно продвинуть".
5. Если появился новый тип временной занятости, например lock busy, worker busy,
   supervisor busy или shutdown, мапь его в retry, а не в успех.
6. Сохрани тесты из этого handoff-а или их эквиваленты. Без regression guards
   изменение легко сломать повторно.

Типичные conflict points:

- `src/watcher/watcher.h`: внешний коммит может вернуть `int` callback.
- `src/watcher/watcher.c`: внешний коммит может обновлять `last_head` внутри
  `check_changes()`. Это снова продвигает baseline до успешной индексации.
- `src/main.c`: внешний коммит может вернуть `0` из `watcher_index_fn()` при
  `cbm_pipeline_try_lock()` failure или shutdown.
- `tests/test_watcher.c`: внешний коммит может оставить старые callback helpers,
  которые возвращают `int`.

## Не Цели

- Этот коммит не вводит межпроцессный writer lock. Это второй handoff:
  `docs/fork/handoff_project_write_lock.md`.
- Этот коммит не меняет формат project DB.
- Этот коммит не пытается решить read consistency во время замены DB.
- Этот коммит не меняет adaptive polling policy, кроме сохранения ожидающей
  индексации.
