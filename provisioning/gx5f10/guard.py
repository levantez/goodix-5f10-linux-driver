"""Единственная точка контроля опасных команд.

Весь USB-трафик обоих фреймингов (classic message-pack и wrapless) проходит
через transport.GuardedUSBProtocol.write(), который спрашивает разрешения здесь.
Политика fail-closed: неизвестный байт команды по умолчанию запрещён.

Кодировка байта команды в обоих фреймингах одна и та же:
    command_byte = category << 4 | command << 1
поэтому одной таблицы достаточно.
"""

import enum
import os
import sys


class Policy(enum.Enum):
    READ = "read"            # не меняет состояние устройства
    WRITE = "write"          # меняет RAM/режим, обратимо через RESET/переподключение
    FORBIDDEN = "forbidden"  # трогает флеш / PSK / option-byte -> риск кирпича


class GuardError(RuntimeError):
    pass


# Байт команды -> (имя, политика). Имена через "/" — как команда называется
# в classic-фрейминге и в wrapless соответственно.
COMMANDS: dict[int, tuple[str, Policy]] = {
    0x00: ("NOP / ping", Policy.READ),
    0x82: ("READ_SENSOR_REGISTER / read_data", Policy.READ),
    0xA6: ("READ_OTP", Policy.READ),
    0xA8: ("FIRMWARE_VERSION", Policy.READ),
    0xE4: ("PRESET_PSK_READ_R / production_read", Policy.READ),
    0xF2: ("READ_FIRMWARE", Policy.READ),
    0xF6: ("GET_IAP_VERSION", Policy.READ),

    0x20: ("MCU_GET_IMAGE / get_image", Policy.WRITE),
    0x32: ("MCU_SWITCH_TO_FDT_DOWN", Policy.WRITE),
    0x34: ("MCU_SWITCH_TO_FDT_UP", Policy.WRITE),
    0x36: ("MCU_SWITCH_TO_FDT_MODE", Policy.WRITE),
    0x50: ("NAV", Policy.WRITE),
    0x60: ("MCU_SWITCH_TO_SLEEP_MODE / set_sleep_mode", Policy.WRITE),
    0x70: ("MCU_SWITCH_TO_IDLE_MODE", Policy.WRITE),
    0x80: ("WRITE_SENSOR_REGISTER", Policy.WRITE),
    0x90: ("UPLOAD_CONFIG_MCU / upload_config", Policy.WRITE),
    0x92: ("SWITCH_TO_SLEEP_MODE", Policy.WRITE),
    0x94: ("SET_POWERDOWN_SCAN_FREQUENCY", Policy.WRITE),
    0x96: ("ENABLE_CHIP", Policy.WRITE),
    0xA2: ("RESET", Policy.WRITE),
    0xAC: ("SET_POV_CONFIG", Policy.WRITE),
    0xAE: ("QUERY_MCU_STATE / ec_control", Policy.WRITE),
    0xB0: ("ACK", Policy.WRITE),
    0xC4: ("SET_DRV_STATE", Policy.WRITE),
    0xD0: ("REQUEST_TLS_CONNECTION", Policy.WRITE),
    0xD2: ("MCU_GET_POV_IMAGE / GTLS handshake", Policy.WRITE),
    0xD4: ("TLS_SUCCESSFULLY_ESTABLISHED", Policy.WRITE),
    0xD6: ("POV_IMAGE_CHECK", Policy.WRITE),

    0xA4: ("MCU_ERASE_APP / erase_app_firmware_info", Policy.FORBIDDEN),
    0xE0: ("PRESET_PSK_WRITE_R", Policy.FORBIDDEN),
    0xE2: ("production_write (sealed PSK / PSK white box)", Policy.FORBIDDEN),
    0xF0: ("WRITE_FIRMWARE / update_firmware", Policy.FORBIDDEN),
    0xF4: ("CHECK_FIRMWARE", Policy.FORBIDDEN),
    # 0xF8 разбирается отдельно: payload[0]==1 -> чтение option byte,
    # payload[0]==0 -> ЗАПИСЬ option byte (флеш write-protect / security биты).
}

OPTION_BYTE_CMD = 0xF8

WHY_FORBIDDEN = {
    0xA4: "стирает APP-прошивку; корректной прошивки для 5f10 у нас нет -> невосстановимый кирпич",
    0xE0: "перезаписывает PSK в сенсоре -> потеря парности с любым существующим драйвером",
    0xE2: "перезаписывает sealed PSK / white box во флеше -> то же",
    0xF0: "пишет прошивку во флеш",
    0xF4: "финализирует запись прошивки (CRC/HMAC-коммит)",
    OPTION_BYTE_CMD: "выставляет флеш-биты write-protect/security -> может залочить чип навсегда",
}


def classify(command_byte: int, payload: bytes) -> tuple[str, Policy]:
    """Вернуть (имя, политику) для байта команды с учётом payload."""
    if command_byte == OPTION_BYTE_CMD:
        if payload and payload[0] == 0x01:
            return ("read_option_byte", Policy.READ)
        return ("write_option_byte", Policy.FORBIDDEN)

    entry = COMMANDS.get(command_byte)
    if entry is None:
        return (f"UNKNOWN({command_byte:#04x})", Policy.FORBIDDEN)
    return entry


def _destructive_override_granted(name: str, command_byte: int) -> bool:
    if os.environ.get("GX_ALLOW_DESTRUCTIVE") != "1":
        return False
    if not sys.stdin.isatty():
        return False
    why = WHY_FORBIDDEN.get(command_byte, "может необратимо испортить сенсор")
    print(f"\n!!! ЗАПРЕЩЁННАЯ КОМАНДА: {name} ({command_byte:#04x})")
    print(f"!!! Почему запрещена: {why}")
    answer = input("!!! Введите ровно 'I understand this can brick the sensor': ")
    return answer.strip() == "I understand this can brick the sensor"


def check(command_byte: int, payload: bytes, *, allow_unknown: bool = False,
          allow_writes: bool = True, allow_psk_write: bool = False) -> tuple[str, Policy]:
    """Разрешить или запретить исходящую команду. Бросает GuardError при запрете.

    allow_psk_write: узкое, явно одобренное пользователем исключение ТОЛЬКО для
    PRESET_PSK_WRITE (0xe0) с флагом 0xbb010003 — провижининг нашего white-box.
    Стирание прошивки (0xa4), запись прошивки (0xf0/0xf4), option-byte (0xf8),
    production_write (0xe2) остаются запрещены всегда.
    """
    name, policy = classify(command_byte, payload)

    if command_byte == 0xE0 and allow_psk_write:
        flags = int.from_bytes(payload[0:4], "little") if len(payload) >= 4 else 0
        # 0xbb010003 — одиночный white-box; 0xbb010002 — полный production-blob
        # (sealed+seed+white-box). Оба несут только PSK white-box, флеш прошивки не трогают.
        if flags in (0xBB010003, 0xBB010002):
            return f"PRESET_PSK_WRITE({flags:#x}) — одобрено пользователем", Policy.WRITE
        raise GuardError(
            f"PRESET_PSK_WRITE разрешён только с флагом 0xbb010003/0xbb010002, получен {flags:#x}")

    if policy is Policy.READ:
        return name, policy

    if policy is Policy.WRITE:
        if not allow_writes:
            raise GuardError(
                f"{name} ({command_byte:#04x}) — запись, а сессия открыта в read-only режиме")
        return name, policy

    # FORBIDDEN
    if name.startswith("UNKNOWN") and allow_unknown:
        return name, Policy.WRITE
    if _destructive_override_granted(name, command_byte):
        print(f"!!! Разрешено вручную: {name}")
        return name, policy
    why = WHY_FORBIDDEN.get(command_byte, "неизвестная команда, политика fail-closed")
    raise GuardError(
        f"ЗАПРЕЩЕНО: {name} ({command_byte:#04x}) — {why}. "
        f"Обход: GX_ALLOW_DESTRUCTIVE=1 + интерактивное подтверждение.")
