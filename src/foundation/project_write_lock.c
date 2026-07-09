/*
 * project_write_lock.c — межпроцессная блокировка записи project DB.
 *
 * Каждый пишущий путь должен брать lock по финальному project_name прямо перед
 * записью в <cache_dir>/<project_name>.db. Время жизни lock-а совпадает с
 * конкретной write-операцией; при смерти процесса ОС закрывает descriptor/handle
 * и освобождает advisory lock. Сам lock-файл intentionally остаётся обычным
 * служебным файлом в cache dir и не хранит состояние владения.
 */
#include "foundation/project_write_lock.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foundation/win_utf8.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

enum { LOCK_FILE_PERMS = 0644 };

struct cbm_project_write_lock {
    char *path;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

static void lock_err(char *err, size_t err_sz, const char *stage, const char *detail) {
    if (!err || err_sz == 0) {
        return;
    }
    if (!stage) {
        err[0] = '\0';
        return;
    }
    snprintf(err, err_sz, "%s%s%s", stage ? stage : "lock_error", detail ? ": " : "",
             detail ? detail : "");
}

static char *lock_path_for_project(const char *project_name, char *err, size_t err_sz) {
    if (!cbm_validate_project_name(project_name)) {
        lock_err(err, err_sz, "invalid_project_name", NULL);
        return NULL;
    }

    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir || !cache_dir[0]) {
        lock_err(err, err_sz, "cache_dir_unavailable", NULL);
        return NULL;
    }
    if (!cbm_mkdir_p(cache_dir, 0755)) {
        lock_err(err, err_sz, "cache_dir_create_failed", cache_dir);
        return NULL;
    }

    char lock_dir[CBM_SZ_1K];
    int n = snprintf(lock_dir, sizeof(lock_dir), "%s/locks", cache_dir);
    if (n < 0 || (size_t)n >= sizeof(lock_dir)) {
        lock_err(err, err_sz, "lock_dir_path_too_long", NULL);
        return NULL;
    }
    if (!cbm_mkdir_p(lock_dir, 0755)) {
        lock_err(err, err_sz, "lock_dir_create_failed", lock_dir);
        return NULL;
    }

    char path_buf[CBM_SZ_1K];
    n = snprintf(path_buf, sizeof(path_buf), "%s/%s.writer.lock", lock_dir, project_name);
    if (n < 0 || (size_t)n >= sizeof(path_buf)) {
        lock_err(err, err_sz, "lock_path_too_long", NULL);
        return NULL;
    }

    char *path = (char *)malloc(strlen(path_buf) + SKIP_ONE);
    if (!path) {
        lock_err(err, err_sz, "oom", NULL);
        return NULL;
    }
    memcpy(path, path_buf, strlen(path_buf) + SKIP_ONE);
    return path;
}

cbm_project_write_lock_result_t cbm_project_write_lock_try_acquire(
    const char *project_name, cbm_project_write_lock_t **out, char *err, size_t err_sz) {
    if (out) {
        *out = NULL;
    }
    if (!out) {
        lock_err(err, err_sz, "invalid_output", NULL);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }

    char *path = lock_path_for_project(project_name, err, err_sz);
    if (!path) {
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }

    cbm_project_write_lock_t *lock =
        (cbm_project_write_lock_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_project_write_lock_t));
    if (!lock) {
        free(path);
        lock_err(err, err_sz, "oom", NULL);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
    lock->path = path;
#ifdef _WIN32
    lock->handle = INVALID_HANDLE_VALUE;
#else
    lock->fd = -1;
#endif

#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        lock_err(err, err_sz, "path_widen_failed", path);
        cbm_project_write_lock_release(lock);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        lock_err(err, err_sz, "lock_file_open_failed", path);
        cbm_project_write_lock_release(lock);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
    lock->handle = h;

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                    MAXDWORD, &ov)) {
        DWORD gle = GetLastError();
        if (gle == ERROR_LOCK_VIOLATION || gle == ERROR_SHARING_VIOLATION) {
            lock_err(err, err_sz, "lock_busy", path);
            cbm_project_write_lock_release(lock);
            return CBM_PROJECT_WRITE_LOCK_BUSY;
        }
        lock_err(err, err_sz, "lock_acquire_failed", path);
        cbm_project_write_lock_release(lock);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
#else
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags, LOCK_FILE_PERMS);
    if (fd < 0) {
        lock_err(err, err_sz, "lock_file_open_failed", strerror(errno));
        cbm_project_write_lock_release(lock);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
    lock->fd = fd;
#ifndef O_CLOEXEC
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved_errno = errno;
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            lock_err(err, err_sz, "lock_busy", path);
            cbm_project_write_lock_release(lock);
            return CBM_PROJECT_WRITE_LOCK_BUSY;
        }
        lock_err(err, err_sz, "lock_acquire_failed", strerror(saved_errno));
        cbm_project_write_lock_release(lock);
        return CBM_PROJECT_WRITE_LOCK_ERROR;
    }
#endif

    *out = lock;
    lock_err(err, err_sz, NULL, NULL);
    return CBM_PROJECT_WRITE_LOCK_OK;
}

void cbm_project_write_lock_release(cbm_project_write_lock_t *lock) {
    if (!lock) {
        return;
    }
#ifdef _WIN32
    if (lock->handle && lock->handle != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        (void)UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (lock->fd >= 0) {
        (void)flock(lock->fd, LOCK_UN);
        (void)close(lock->fd);
        lock->fd = -1;
    }
#endif
    free(lock->path);
    free(lock);
}
