#!/usr/bin/env bash
# Сборка TOD-драйвера Goodix 5f10 (SIGFM) и тестового харнеса.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "== сборка драйвера (meson) =="
[ -d build ] || meson setup build
meson compile -C build

echo "== сборка тест-харнеса tod_test =="
gcc tools/tod_test.c -o build/tod_test $(pkg-config --cflags --libs libfprint-2)

echo "== сборка проверки генерации конфига (milang_verify) =="
gcc tools/milang_verify.c -Isrc -o build/milang_verify

echo
echo "Готово:"
echo "  драйвер:  build/libfprint-tod-goodix5f10.so"
echo "  тест:     build/tod_test"
echo
echo "Дальше: ./packaging/install.sh   (установка в систему)"
