# Handoff: watcher retry не теряет ожидающую индексацию

Статус: перенесено и адаптировано для текущего upstream.
Дата переноса: 2026-08-02.
Ветка: `v0.9.1-rc.1-lora`.
Upstream-база: тег `v0.9.1-rc.1`, коммит
`ee9833df2c72b750378273897ffec9410fc2c4f2`.
Исторический источник: ветка `0.9.0-hermione`, коммит
`4826d41a9651cac4bfcd546ebe409b5836946ea1`
(`watcher: preserve pending git changes on retry`) и последующее усиление
регрессией `dirty -> retry -> clean`.

## Назначение

Карточка фиксирует инвариант watcher-а: обнаруженное изменение считается
обработанным только после успешного callback-а индексации. Retry или ошибка
оставляют работу ожидающей, даже если к следующему poll текущее Git-состояние
успело снова совпасть с последним успешно проиндексированным baseline.

## Что уже сделал upstream

В `v0.9.1-rc.1` большая часть старого fork-фикса уже присутствует:

- `cbm_index_fn` возвращает `int`: `0` означает успех, положительное значение —
  временный retry, отрицательное — ошибку;
- daemon application передаёт занятость и отмену как retry, успешное завершение
  как `0`, ошибку как отрицательное значение;
- `check_changes()` сохраняет наблюдённые HEAD и dirty signature в
  `pending_head` и `pending_dirty_sig`;
- `poll_project()` продвигает `last_head` и `last_dirty_sig` только после
  результата `0`;
- тесты уже проверяют однократную индексацию неизменного dirty-состояния и
  повтор после неуспешной индексации при неизменившемся HEAD.

Поэтому старые изменения API watcher-а и daemon callback-а в эту ветку не
переносятся.

## Оставшийся дефект

Одного непродвинутого baseline недостаточно:

1. baseline соответствует чистому дереву;
2. дерево становится dirty, watcher вызывает callback;
3. callback возвращает retry или ошибку, baseline остаётся чистым;
4. до следующего poll дерево откатывается обратно в clean;
5. HEAD и dirty signature снова совпадают с baseline;
6. без отдельного pending-признака `check_changes()` не вызывает callback и
   незавершённая индексация теряется.

Это воспроизводит тест
`watcher_retries_dirty_then_clean_after_index_retry`: на исходном
`v0.9.1-rc.1` второй callback не происходил.

## Контракт после переноса

- `project_state_t.index_pending` означает, что предыдущий callback не
  завершил обработку обнаруженного изменения.
- `check_changes()` начинает решение с `index_pending`, а затем дополняет его
  сравнением текущих HEAD и dirty signature с зафиксированным baseline.
- Каждый успешный Git probe обновляет `pending_head` и `pending_dirty_sig`;
  после успешного callback-а именно это наблюдение становится baseline.
- Callback с результатом `0` фиксирует pending-наблюдения как новый baseline и
  очищает `index_pending`.
- Положительный или отрицательный результат callback-а ставит
  `index_pending=true` и не продвигает baseline.
- После успешного retry стабильное состояние больше не вызывает callback.
- Неизменное dirty-состояние по-прежнему индексируется только один раз после
  успешного callback-а.

## Состав переноса

- `src/watcher/watcher.c`: добавлен и проведён через poll-цикл
  `index_pending`.
- `tests/test_watcher.c`: добавлена регрессия
  `watcher_retries_dirty_then_clean_after_index_retry`.
- `docs/fork/handoff_watcher_retry_pending_git_changes.md`: эта карточка.

Другие части старого fork-коммита не переносятся, потому что эквивалентный
контракт уже реализован upstream.

## Проверки

Узкая проверка watcher suite:

```bash
make -f Makefile.cbm build/c/test-runner
build/c/test-runner watcher
```

Ключевые regression guards в `tests/test_watcher.c`:

- `watcher_dirty_state_reindexes_once_issue937` — стабильное dirty-состояние
  не вызывает повторную индексацию после успеха;
- `watcher_failed_reindex_retries_issue937` — ошибка не продвигает HEAD
  baseline, следующий poll повторяет callback;
- `watcher_retries_dirty_then_clean_after_index_retry` — pending-работа не
  теряется при возврате dirty-дерева к прежнему clean baseline.

## Восстановление после внешнего merge

При следующем обновлении upstream проверь четыре свойства:

1. В `project_state_t` есть эквивалент `index_pending`.
2. `check_changes()` возвращает изменение при установленном pending-признаке,
   даже когда текущие HEAD и dirty signature совпадают с baseline.
3. Только успешный callback очищает pending-признак и продвигает baseline;
   retry и error сохраняют обязанность повторить работу.
4. Все три regression guards выше присутствуют или заменены эквивалентными
   сценариями.

Если scheduler или callback API изменились, сохраняй этот контракт по смыслу,
а не старую форму кода.

## Не цели

- Перенос не меняет daemon job scheduling и callback ABI из
  `v0.9.1-rc.1`.
- Перенос не меняет adaptive polling policy.
- Перенос не добавляет другие fork-карточки или несвязанные исправления.
