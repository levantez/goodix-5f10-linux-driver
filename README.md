# Драйвер отпечатка Goodix `27c6:5f10` (milanG) для Linux

Userspace-драйвер сканера отпечатка **Goodix `27c6:5f10`** (платформа *milanG*),
который стоит в ноутбуках **HONOR** (MagicBook X16 Pro, FMI-XX и родственных).
Под Windows для него используется USB-драйвер `gfusb` (1.1.127.4); под Linux
готового драйвера нет ни у вендора, ни в upstream `libfprint`, ни в сообществе.

Этот проект доводит сенсор до **штатного входа в систему и `sudo` по отпечатку**
через `libfprint` + `fprintd` + `pam_fprintd`. Работает целиком в userspace
(libusb), **без модулей ядра**. Пароль всегда остаётся резервным.

> Статус: **работает** на HONOR FMI-XX (Linux Mint 22.2, libfprint 1.94.7+tod1).
> `fprintd-enroll` / `fprintd-verify` и вход/`sudo` по отпечатку функционируют.

---

## Как это устроено

Драйвер ставится **TOD-модулем** (Touch OEM Driver) — один `.so`, который
подхватывает системный `libfprint`. **Системный `libfprint` не заменяется и не
пересобирается** — только кладётся наш `.so` в каталог `tod-1/`.

Ключевые слои (`src/`):

| Файл | Роль |
|---|---|
| `goodix.c` / `goodix_proto.c` | USB-транспорт и фрейминг протокола Goodix (message-pack, bulk 0x01/0x81) |
| `goodixtls.c` | TLS-PSK-AES128-GCM в процессе (OpenSSL) — сенсор шифрует картинку |
| `goodix5f10.c` | **сам драйвер**: активация, калибровка, захват, детект пальца, SIGFM-матчинг |
| `sigfm/` | SIFT-матчер отпечатков (OpenCV) — из libfprint SIGFM-ветки |
| `goodix5f10.h` | константы устройства: USB ID, PSK-флаги, `DEVICE_CONFIG` |

### Почему не «как обычно» (NBIS)

Стандартный путь `libfprint` для image-сенсоров — извлечь минуции (NBIS `mindtct`)
и сравнить их (`bozorth3`). На этом сенсоре так **не работает**: кадр — узкая
полоса `56×176` из почти параллельных гребней, и `mindtct` находит из неё всего
**~2 минуции** (проверено; `bozorth3` нужно ~12+). Диагностика (см. ниже) показала,
что это не масштаб и не препроцессинг, а свойство изображения.

Поэтому драйвер сделан **не image-устройством**, а кастомным `FpDevice` со **своим
матчингом**: из того же кадра **SIFT даёт ~244 признака**, а геометрически
согласованное сравнение (SIGFM) чётко делит свой/чужой:

```
свой палец (перекрытие области)  → score 10^5…10^6
свой палец (без перекрытия)      → 0
чужой палец                      → 0   (ни одного ложного совпадения)
```

### Что делает драйвер при захвате

1. **Активация** (open): `nop → enable_chip → reset → upload DEVICE_CONFIG →
   powerdown_scan_freq`, затем **TLS-PSK хендшейк** нашим ключом.
2. **Калибровка FDT**: чтение базового уровня ёмкостных зон.
3. **Детект пальца**: опрос выделенного **FDT-канала** (не картинки!) — палец
   понижает показания зон; ждём полного касания (≥7 из 10 зон). *(У этого чипа
   `fdt_down` не блокирует до касания, поэтому детект — по показаниям FDT.)*
4. **Снимок**: фоновый кадр (без пальца) вычитается из кадра с пальцем,
   перцентильная нормализация → 8-бит `56×176`.
5. **SIFT**: `sigfm_extract` → дескрипторы.
6. **Матчинг/шаблон**: `enroll` собирает несколько кадров (покрытие пальца) и
   хранит их SIFT-данные в `FpPrint` (`fpi-data`, `FPI_PRINT_RAW`); `verify`/
   `identify` сравнивают пробу с каждым (`sigfm_match_score ≥ порог`).

### Криптография / провижининг

Сенсор общается по TLS-PSK. Заводской PSK запечатан Windows-DPAPI и под Linux
недоступен. Поэтому PSK **перезаписывается нашим** штатной командой
`PRESET_PSK_WRITE` (см. `provisioning/`). После этого TLS поднимается нашим
ключом (`provisioning/docs/psk.hex`). White-box для записи PSK универсален для
всего семейства `5f10`; калибровочный `DEVICE_CONFIG` (DAC) вычисляется из OTP
конкретного устройства.

---

## Требования

* Ноутбук с сенсором `27c6:5f10` (проверь: `lsusb | grep 27c6:5f10`).
* Дистрибутив с `libfprint` **со сборкой TOD** (Ubuntu/Mint: пакет
  `libfprint-2-tod1`), `fprintd`, `libpam-fprintd`.
* Для сборки: `meson`, `ninja`, `gcc`/`g++`, `pkg-config`,
  `libfprint-2-tod-dev`, `libssl-dev`, `libopencv-dev`, `libglib2.0-dev`,
  `libgusb-dev`, `libusb-1.0-0-dev`.

Установка зависимостей (Debian/Ubuntu/Mint):

```bash
sudo apt install -y meson ninja-build gcc g++ pkg-config \
  libfprint-2-tod-dev libssl-dev libopencv-dev \
  libglib2.0-dev libgusb-dev libusb-1.0-0-dev \
  fprintd libpam-fprintd
```

---

## Установка с нуля

```bash
git clone <this-repo> goodix-5f10-driver
cd goodix-5f10-driver

# 1. Собрать драйвер (.so) и тест-харнес
./build.sh

# 2. (ТОЛЬКО для «свежего» сенсора — один раз) записать наш PSK в устройство
#    Если сенсор уже провизинен этим драйвером — пропусти. См. раздел «Провижининг».
#    (нужен рабочий Python-venv и goodix-fp-dump, см. provisioning/README-ниже)

# 3. Установить драйвер в систему (sudo). Системный libfprint НЕ трогается.
./packaging/install.sh

# 4. Зарегистрировать отпечаток штатным инструментом (спросит палец ~16 раз)
fprintd-enroll

# 5. Проверить
fprintd-verify

# 6. Включить вход/sudo по отпечатку (пароль остаётся резервным)
sudo pam-auth-update       # отметить «Fingerprint authentication», сохранить
```

**Проверка перед доверием PAM** (чтобы не запереть себя):

```bash
# держи ОТКРЫТЫМ второй терминал с рабочим `sudo -i`
sudo -k; sudo true         # должно предложить палец; по паролю пускает всегда
```

Обе PAM-опции — `sufficient` над `pam_unix`, поэтому пароль всегда работает как
резерв; запереть себя нельзя. Откат: снова `sudo pam-auth-update`, снять галочку.

> На Linux Mint есть также опция **«Fingwit … keyring unlocking»** — то же самое
> плюс разблокировка связки ключей отпечатком при графическом входе. Для просто
> `sudo`/входа достаточно обычной «Fingerprint authentication».

---

## Тест без установки в систему

Драйвер можно гонять локально, не копируя в систему (через `FP_TOD_DRIVERS_DIR`):

```bash
./build.sh
packaging/run.sh list             # найти устройство (палец не нужен)
packaging/run.sh enroll myfinger  # регистрация -> /tmp/myfinger.fp
packaging/run.sh verify myfinger  # сверка (свой/чужой)
DEBUG=1 packaging/run.sh verify myfinger   # с отладочным логом (видно sigfm score)
```

---

## Провижининг (запись нашего PSK) — для «свежего» сенсора

Если сенсор ещё не провизинен этим драйвером, один раз выполняется запись PSK.
Инструменты — в `provisioning/` (Python, используют протокол-референс
`goodix-fp-dump`):

```bash
# референс протокола (MIT):
git clone https://github.com/goodix-fp-linux-dev/goodix-fp-dump vendor/goodix-fp-dump
python3 -m venv .venv && .venv/bin/pip install pyusb crcmod crccheck pycryptodome numpy opencv-python

.venv/bin/python provisioning/status.py          # адрес, права на узел (ничего не шлёт)
.venv/bin/python provisioning/provision.py --check
.venv/bin/python provisioning/provision.py --write --combined   # ЗАПИСЬ PSK во флеш
```

> ⚠️ Провижининг перезаписывает заводской PSK. После этого сенсор работает под
> Linux этим драйвером; возврат к Windows-драйверу потребует его повторной
> инициализации родным ПО. Guard в `provisioning/gx5f10/guard.py` блокирует
> опасные команды (запись прошивки/erase/option-byte) — не обходить вручную.

Константы в `provisioning/docs/`: `psk.hex` (наш PSK), `provision_blob.hex`
(white-box для записи — TLV `0xbb010003` внутри), `config_milang*.hex`
(калибровочный конфиг).

> ⚠️ **PSK у всех один и тот же.** `psk.hex` лежит в публичном репозитории, и
> провижининг записывает этот ключ в каждый сенсор. Кто имеет физический доступ к
> USB ноутбука, может пройти TLS с сенсором и снимать с него кадры. Шаблоны
> отпечатков при этом лежат на диске (`/var/lib/fprint`), их защищает только
> шифрование диска. Ключ на устройство пока не сделан: white-box в
> `provision_blob.hex` вычислен эмуляцией `gfusb.dll` под этот конкретный ключ.
> Если Windows с заводским ключом ещё на диске, ключ можно не перезаписывать —
> см. [Sbenazar/goodix-5f10-libfprint](https://github.com/Sbenazar/goodix-5f10-libfprint).

---

## Раздача другим владельцам таких ноутбуков

Драйвер **универсальный** — не требует пересборки под конкретный сенсор:

* **Калибровка (`DEVICE_CONFIG`/DAC) генерируется из OTP устройства прямо в
  драйвере** при активации (`src/milang_config.h`, порт `milang.build_config`).
  Каждый сенсор milanG имеет свой OTP → свой DAC → свой конфиг. Порт проверен
  **байт-в-байт** против рабочего конфига (см. `tools/milang_verify.c`) и на
  устройстве (сенсор принимает сгенерированный конфиг и калибруется).
* **PSK-провижининг** обязателен — один раз на каждое устройство (см. раздел
  «Провижининг»), т.к. заводской PSK под Linux недоступен.

Итого для нового такого ноутбука: `./build.sh` → провижининг PSK (один раз) →
`./packaging/install.sh` → `fprintd-enroll`. Пересобирать под устройство не нужно.

Проверить генерацию конфига (без устройства):
```bash
gcc tools/milang_verify.c -Isrc -o /tmp/milang_verify && /tmp/milang_verify
# печатает сгенерированный из OTP автора конфиг; сравни с provisioning/docs/config_milang.hex
```

---

## Как это реверсили (кратко)

* Опознание чипа: `read_sensor_register(0,4) = a4082200` → chip id `0x2208` →
  платформа **milanG**, `sensorType 2`.
* Калибровка DAC: разбор `gfusb.dll` (Ghidra) — `FUN_180036030`
  (`milanG_check_and_parse_otp`) вычисляет DAC из 32 байт OTP; воспроизведено в
  `provisioning/gx5f10/milang.py` (собранный конфиг совпал с рабочим байт-в-байт).
* PSK/white-box: эмуляция функций `gfusb.dll` (Unicorn) для записи PSK.
* Геометрия кадра `56×176` подтверждена анализом (reshape при разных ширинах).
* Выбор матчера: строгая диагностика `mindtct` (2 минуции, инвариантно к
  ppmm/препроцессингу) vs SIFT (244 признака) → SIGFM. См. `docs/`.

Подробности протокола и калибровки — в `docs/protocol.md`, `docs/calibration.md`,
`docs/windows_driver.md`.

---

## Структура проекта

```
build.sh                 сборка драйвера + тест-харнеса
meson.build              сборка .so (C + C++/OpenCV)
src/                     исходники драйвера
  goodix*.c/.h           транспорт + TLS Goodix
  goodix5f10.c/.h        драйвер milanG + SIGFM-интеграция
  sigfm/                 SIFT-матчер (OpenCV)
packaging/
  install.sh             установка .so в систему + udev + рестарт fprintd
  run.sh                 запуск тест-харнеса без установки
  70-goodix-5f10.rules   udev-правило (доступ к сенсору)
tools/
  tod_test.c             тест-харнес (enroll/verify/identify без fprintd)
  mindtct_probe.c        диагностика извлечения минуций (NBIS)
  sigfm_bench.cpp        бенчмарк SIGFM на кадрах (same/diff score)
provisioning/            запись PSK + калибровка для «свежего» сенсора (Python)
docs/                    протокол, калибровка, разбор Windows-драйвера
```

---

## Безопасность

* Никаких модулей ядра — только userspace (libusb).
* Системный `libfprint` не заменяется — драйвер ставится TOD-модулем.
* Вход/`sudo` — с обязательным резервом по паролю (`pam_fprintd` как `sufficient`).
* Провижининг-инструменты имеют guard от «кирпичащих» команд.

## Лицензия

LGPL-2.1-or-later (производное от `libfprint` / goodixtls / SIGFM). Текст лицензии — `LICENSE`,
происхождение кода и авторы — `NOTICE`.
