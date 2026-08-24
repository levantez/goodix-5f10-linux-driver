"""milanG: вычисление DAC-калибровки из OTP устройства и сборка DEVICE_CONFIG.

Воспроизводит FUN_180036030 (milanG_check_and_parse_otp) из gfusb.dll: DAC берётся
из 32-байтного OTP мажоритарным голосованием 3 копий (одна инвертирована), затем
патчится в базовый шаблон конфига (docs/config_milang_base.hex) с пересчётом
контрольной суммы (FUN_180014a60: (0x5a5b - Σ int16-слов[0:0x6f]) & 0xffff @0xde).

Это делает драйвер РАСПРОСТРАНЯЕМЫМ: у каждого устройства свой OTP -> свой DAC,
и конфиг генерится на месте при установке.
"""
import pathlib
import struct

DOCS = pathlib.Path(__file__).resolve().parent.parent / "docs"
BASE_CONFIG = DOCS / "config_milang_base.hex"


def _maj3(a, b, c):
    """Мажоритарное голосование как в FUN_180036030 (a,b — прямые; c — уже инверт.)."""
    if a != 0 and a == b:
        return a
    if a != 0 and a == c:
        return a
    if b != 0 and b == c:
        return b
    return 0


def dac_from_otp(otp: bytes):
    """Вернуть (dacvalue, second) — базовые DAC-значения из OTP milanG."""
    inv = lambda x: (~x) & 0xFF
    dacvalue = _maj3(otp[0x1a], otp[0x1f], inv(otp[0x1b]))
    second = _maj3(otp[0x16], otp[0x19], inv(otp[0x17]))
    return dacvalue, second


def dac_registers(otp: bytes):
    """Значения регистров конфига из OTP: {0x220, 0x5c, 0x5e}."""
    dacvalue, second = dac_from_otp(otp)
    reg220 = (dacvalue << 4 | 8) & 0xFFFF
    reg5c = (((second >> 4) + 1) * 0x10) & 0xFFFF
    reg5e = (((second & 0xF) + 2) * 0x6400 // reg5c) & 0xFFFF if reg5c else 0
    return {0x0220: reg220, 0x005c: reg5c, 0x005e: reg5e}


def _checksum(cfg: bytes) -> int:
    words = struct.unpack("<111H", cfg[:0x6f * 2])
    return (0x5A5B - sum(words)) & 0xFFFF


def build_config(otp: bytes, base: bytes | None = None) -> bytes:
    """Собрать 224-байтный DEVICE_CONFIG под конкретное устройство по его OTP."""
    if base is None:
        base = bytes.fromhex(BASE_CONFIG.read_text().strip())
    cfg = bytearray(base)
    regs = dac_registers(otp)
    for addr, val in regs.items():
        for i in range(1, len(cfg) - 3, 2):
            if int.from_bytes(cfg[i:i + 2], "little") == addr:
                cfg[i + 2:i + 4] = struct.pack("<H", val)
                break
    cfg[0xde:0xe0] = struct.pack("<H", _checksum(cfg))
    return bytes(cfg)
