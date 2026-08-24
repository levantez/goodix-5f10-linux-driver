"""Восстановление сенсора без физического переподключения.

Сенсор внутренний (шина 1, порт 4) — выдернуть его нельзя, поэтому путь
восстановления обязан быть программным и проверенным ДО первой нестандартной
команды.

Уровни, по возрастанию:
  1. USBDEVFS_RESET через /dev/bus/usb/BBB/DDD   — не нужен root при udev-правиле
  2. authorized 0 -> 1 в sysfs                   — нужен root
  3. unbind/bind драйвера порта                  — нужен root
  4. ребут
"""
import fcntl
import pathlib

from . import PRODUCT_ID, VENDOR_ID

USBDEVFS_RESET = 0x5514  # _IO('U', 20)
SYSFS_USB = pathlib.Path("/sys/bus/usb/devices")


def find() -> dict:
    """Найти устройство в sysfs. Не требует прав на сам девайс."""
    for entry in sorted(SYSFS_USB.iterdir()):
        vid = entry / "idVendor"
        pid = entry / "idProduct"
        if not (vid.is_file() and pid.is_file()):
            continue
        if int(vid.read_text(), 16) != VENDOR_ID:
            continue
        if int(pid.read_text(), 16) != PRODUCT_ID:
            continue
        busnum = int((entry / "busnum").read_text())
        devnum = int((entry / "devnum").read_text())
        return {
            "sysfs": entry,
            "busnum": busnum,
            "devnum": devnum,
            "node": pathlib.Path(f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"),
            "product": (entry / "product").read_text().strip(),
            "serial": (entry / "serial").read_text().strip(),
            "bcdDevice": (entry / "bcdDevice").read_text().strip(),
        }
    raise FileNotFoundError(f"{VENDOR_ID:#06x}:{PRODUCT_ID:#06x} не найден в sysfs")


def drivers() -> dict[str, str]:
    """Какой драйвер ядра привязан к каждому интерфейсу (ожидаем — никакой)."""
    dev = find()
    result = {}
    for iface in sorted(dev["sysfs"].glob(f"{dev['sysfs'].name}:*")):
        link = iface / "driver"
        result[iface.name] = link.resolve().name if link.is_symlink() else "(нет)"
    return result


def reset() -> None:
    """Уровень 1: USBDEVFS_RESET. Переинициализирует устройство, не трогая ядро."""
    node = find()["node"]
    with node.open("wb") as handle:
        fcntl.ioctl(handle, USBDEVFS_RESET, 0)


def alive() -> bool:
    """Отвечает ли устройство на чтение дескрипторов."""
    try:
        dev = find()
    except FileNotFoundError:
        return False
    try:
        return bool((dev["sysfs"] / "idProduct").read_text().strip())
    except OSError:
        return False


def soft_mcu_reset(sleep_time: int = 20, wait: float = 8.0) -> bool:
    """Уровень 1.5 — программный сброс MCU командой RESET.

    Сильнее, чем USBDEVFS_RESET: тот дёргает только шину, а состояние прошивки
    остаётся. Например, после оборванного TLS-хендшейка сенсор перестаёт слать
    ACK-пакеты, и никакой сброс шины это не лечит — а soft-reset MCU лечит:
    устройство переподключается к шине (devnum меняется) и возвращается чистым.

    Кодировка payload команды RESET 0xa2: бит0 — сброс сенсора,
    бит1 — soft reset MCU, бит2 — дублирует бит0. Шлём 0b011 (сенсор + MCU):
    именно эта комбинация проверенно вызывает переподключение и чистое состояние.
    """
    import time

    from . import frames, rawlink
    from .logbook import Logbook

    log = Logbook("soft-reset")
    before = find()["devnum"]
    try:
        link = rawlink.RawLink(log, allow_writes=True)
        try:
            link.send(frames.classic(0xA2, bytes([0b011, sleep_time])), "classic",
                      timeout_ms=1000)
        finally:
            try:
                link.close()
            except Exception:  # noqa: BLE001 — устройство уже отвалилось, это норма
                pass
    except Exception as error:  # noqa: BLE001
        log.note(f"soft_mcu_reset: отправка не удалась: {error}")
    finally:
        log.close()

    deadline = time.time() + wait
    while time.time() < deadline:
        time.sleep(0.25)
        try:
            dev = find()
        except FileNotFoundError:
            continue
        if dev["node"].exists() and dev["devnum"] != before:
            time.sleep(0.5)  # дать udev выставить права на новый узел
            return True
    return alive()


def acks_ok(timeout_ms: int = 500) -> bool:
    """Шлёт FIRMWARE_VERSION и проверяет, приходит ли ACK перед данными.

    Пропавшие ACK — надёжный признак того, что MCU застрял в состоянии
    после оборванного TLS. Это дешёвая проверка «здоров ли протокол».

    ВАЖНО: первая команда после открытия устройства всегда остаётся без ответа,
    поэтому сначала шлём холостой NOP. Без него проверка всегда врёт «нет ACK».
    """
    from . import frames, rawlink
    from .logbook import Logbook

    log = Logbook("acks-check")
    try:
        link = rawlink.RawLink(log, allow_writes=True)
        try:
            link.send(frames.classic(0xA8, b"\x00\x00"), "classic", timeout_ms=1000)
            for _, data in link.drain(rounds=4, timeout_ms=timeout_ms):
                if len(data) > 7 and data[4] == 0xB0:
                    return True
            return False
        finally:
            link.close()
    finally:
        log.close()
