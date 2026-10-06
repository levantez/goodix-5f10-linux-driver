#!/usr/bin/env bash
# Установить TOD-драйвер 5f10 (SIGFM) в систему, чтобы его подхватил fprintd.
# Требует sudo. Системный libfprint НЕ трогаем — только кладём наш .so в tod-1/.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SO="$HERE/build/libfprint-tod-goodix5f10.so"
TOD_DIR=/usr/lib/x86_64-linux-gnu/libfprint-2/tod-1
RULES="$HERE/packaging/70-goodix-5f10.rules"

[ -f "$SO" ] || { echo "нет $SO — сначала ./build.sh"; exit 1; }

echo "== 1. Проверка рантайм-зависимостей .so =="
if ldd "$SO" | grep -qi 'not found'; then
  echo "ОШИБКА: не хватает библиотек:"; ldd "$SO" | grep -i 'not found'; exit 1
fi
echo "ок"

echo "== 2. Копирую драйвер в $TOD_DIR =="
sudo mkdir -p "$TOD_DIR"
sudo cp "$SO" "$TOD_DIR/"

echo "== 3. udev-правило (доступ к сенсору) =="
[ -f "$RULES" ] && sudo install -m644 "$RULES" /etc/udev/rules.d/ && sudo udevadm control --reload \
  && sudo udevadm trigger --attr-match=idVendor=27c6 || true

echo "== 4. Перезапуск fprintd =="
sudo systemctl restart fprintd 2>/dev/null || sudo pkill -f fprintd || true
sleep 1

echo "== 5. Проверка, что fprintd видит устройство =="
if command -v fprintd-list >/dev/null; then
  fprintd-list "$USER" 2>&1 | head -5 || true
fi
echo
echo "Готово. Дальше:"
echo "  fprintd-enroll            # регистрация штатным инструментом"
echo "  fprintd-verify            # проверка"
echo "  sudo pam-auth-update      # включить вход/sudo по отпечатку (галочка Fingerprint)"
