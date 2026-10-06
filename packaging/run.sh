#!/usr/bin/env bash
# Тест драйвера БЕЗ установки в систему (через FP_TOD_DRIVERS_DIR).
#   packaging/run.sh list            — найти устройство (палец не нужен)
#   packaging/run.sh enroll <имя>    — регистрация (прикладывай/отпускай палец)
#   packaging/run.sh verify <имя>    — сверка
#   DEBUG=1 packaging/run.sh verify <имя>   — с полным отладочным логом
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f build/libfprint-tod-goodix5f10.so ] && [ -x build/tod_test ] || { echo "сначала ./build.sh"; exit 1; }
if [ $# -eq 0 ]; then set -- list; fi
echo "== запуск: $* =="
if [ "${DEBUG:-0}" = 1 ]; then
  FP_TOD_DRIVERS_DIR="$ROOT/build" G_MESSAGES_DEBUG=all build/tod_test "$@"
else
  FP_TOD_DRIVERS_DIR="$ROOT/build" build/tod_test "$@" 2>/dev/null
fi
