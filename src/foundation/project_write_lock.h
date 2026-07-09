/*
 * project_write_lock.h — OS-level writer lock для проектных индексов.
 *
 * Модуль даёт короткоживущую межпроцессную блокировку записи по финальному
 * project_name. Lock-файл живёт в активном cache dir, но владельцем считается
 * только OS-level advisory lock на открытом descriptor/handle: файл может
 * остаться после аварийного завершения и не является признаком владения.
 */
#ifndef CBM_PROJECT_WRITE_LOCK_H
#define CBM_PROJECT_WRITE_LOCK_H

#include <stddef.h>

typedef struct cbm_project_write_lock cbm_project_write_lock_t;

typedef enum {
    CBM_PROJECT_WRITE_LOCK_ERROR = -1,
    CBM_PROJECT_WRITE_LOCK_OK = 0,
    CBM_PROJECT_WRITE_LOCK_BUSY = 1,
} cbm_project_write_lock_result_t;

/* Попытаться взять non-blocking writer lock для project_name.
 *
 * project_name должен проходить cbm_validate_project_name(), потому что из него
 * строится путь <cache_dir>/locks/<project>.writer.lock. При успехе *out получает
 * владеющий handle, который нужно освободить через
 * cbm_project_write_lock_release(). При BUSY запись начинать нельзя; при ERROR
 * err содержит краткую причину, если передан буфер.
 */
cbm_project_write_lock_result_t cbm_project_write_lock_try_acquire(
    const char *project_name, cbm_project_write_lock_t **out, char *err, size_t err_sz);

/* Освободить lock и закрыть связанный descriptor/handle. NULL-safe. */
void cbm_project_write_lock_release(cbm_project_write_lock_t *lock);

#endif /* CBM_PROJECT_WRITE_LOCK_H */
