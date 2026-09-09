#!/usr/bin/env bash
set -euo pipefail

# Устанавливает уже собранный `build/c/codebase-memory-mcp` в
# `/usr/local/bin/codebase-memory-mcp`. Скрипт не принимает аргументов и при
# обычном запуске повторно запускает себя через `sudo`; также требуются команды
# `install`, `mktemp`, `cmp` и `mv`.
#
# Бинарник сначала копируется в уникальный временный файл в целевом
# каталоге, а затем атомарно заменяет целевой файл. При ошибке временный
# файл удаляется. Сборка, конфигурация клиентов и перезапуск процессов в его
# контракт не входят.

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
SCRIPT_PATH="$ROOT/scripts/install.sh"
SOURCE="$ROOT/build/c/codebase-memory-mcp"
INSTALL_DIR="/usr/local/bin"
TARGET="$INSTALL_DIR/codebase-memory-mcp"
STAGED=""

# Удалить только созданный этим запуском временный файл, если установка
# завершилась до атомарной публикации целевого бинарника.
cleanup_staged() {
    if [[ -n "$STAGED" && -e "$STAGED" ]]; then
        rm -f -- "$STAGED"
    fi
}
trap cleanup_staged EXIT

if (( $# != 0 )); then
    printf 'Использование: scripts/install.sh\n' >&2
    exit 2
fi

if (( EUID != 0 )); then
    if ! command -v sudo >/dev/null 2>&1; then
        printf 'Ошибка: для установки в %s нужны права root, а команда sudo не найдена.\n' \
            "$INSTALL_DIR" >&2
        exit 1
    fi
    exec sudo -- "$SCRIPT_PATH"
fi

if [[ ! -f "$SOURCE" || ! -x "$SOURCE" ]]; then
    printf 'Ошибка: не найден исполняемый бинарник %s.\n' "$SOURCE" >&2
    printf 'Сначала выполни scripts/build.sh.\n' >&2
    exit 1
fi

if [[ ! -d "$INSTALL_DIR" ]]; then
    install -d -m 0755 "$INSTALL_DIR"
fi

STAGED="$(mktemp "$INSTALL_DIR/.codebase-memory-mcp.install.XXXXXX")"
install -m 0755 "$SOURCE" "$STAGED"

if ! cmp -s "$SOURCE" "$STAGED"; then
    printf 'Ошибка: временная копия отличается от собранного бинарника.\n' >&2
    exit 1
fi

mv -f "$STAGED" "$TARGET"
STAGED=""
trap - EXIT

printf 'Установлено: %s\n' "$TARGET"
