"""USB-транспорт с guard'ом и логированием.

Наследуем vendor USBProtocol (он корректно находит CDC-Data интерфейс и bulk
endpoint'ы), но перехватываем write()/read(): весь исходящий трафик проходит
через guard.check(), весь трафик в обе стороны — в logbook.

Байт команды в исходящем буфере:
  classic  : flags(1) len(2) csum(1) | cmd(1) len(2) payload... csum(1)
             -> cmd = data[4], payload = data[7:]  (только при flags == 0xa0;
                0xb0/0xb2 — это TLS-данные, команд там нет)
  wrapless : cmd(1) len(2) payload... csum(1)
             -> cmd = data[0], payload = data[3:];
                продолжающие чанки имеют младший бит cmd выставленным
"""
from . import vendor_path  # noqa: F401  (side effect: sys.path)

import protocol as vendor_protocol  # from vendor/goodix-fp-dump
import usb.util

from . import guard
from .logbook import Logbook

FLAGS_MESSAGE_PROTOCOL = 0xA0


class GuardedUSBProtocol(vendor_protocol.USBProtocol):
    """USBProtocol + контроль команд + лог."""

    def __init__(self, vendor, product, timeout=5, *, framing="classic",
                 logbook: Logbook | None = None, allow_writes=True,
                 allow_unknown=False, allow_psk_write=False):
        if framing not in ("classic", "wrapless"):
            raise ValueError(f"неизвестный фрейминг: {framing}")
        self.framing = framing
        # лог закрываем только если создали его сами — иначе владелец переоткроет
        # устройство и напорется на закрытый файл
        self._owns_log = logbook is None
        self.log = logbook or Logbook(f"transport-{framing}")
        self.allow_writes = allow_writes
        self.allow_unknown = allow_unknown
        self.allow_psk_write = allow_psk_write
        self._last_approved_cmd: int | None = None
        self.log.note(f"открываю {vendor:#06x}:{product:#06x}, фрейминг={framing}, "
                      f"записи={'разрешены' if allow_writes else 'запрещены'}")
        super().__init__(vendor, product, timeout)
        self.log.note(f"endpoints: out={self.endpoint_out:#04x} in={self.endpoint_in:#04x}")
        self.drain()

    def drain(self, rounds: int = 8, timeout: float = 0.15) -> int:
        """Выгрести залежавшиеся ответы.

        После аварийного обрыва (например, фатального TLS-alert) в очереди
        остаётся ответ от предыдущей команды, и дальше КАЖДЫЙ ответ приходит
        на команду позже — vendor-код падает с ValueError на несовпадении ACK.
        """
        dropped = 0
        for _ in range(rounds):
            try:
                stale = super().read(0x10000, timeout)
            except Exception:  # noqa: BLE001  — таймаут = очередь пуста
                break
            if not stale:
                break
            dropped += 1
            self.log.note(f"дренаж: выброшен залежавшийся ответ {stale.hex(' ')}")
        return dropped

    def _inspect(self, data: bytes) -> str:
        """Достать команду, спросить guard. Возвращает имя команды для лога."""
        if not data:
            raise guard.GuardError("пустая запись")

        if self.framing == "classic":
            if data[0] != FLAGS_MESSAGE_PROTOCOL:
                return f"raw(flags={data[0]:#04x})"  # TLS-данные, команд нет
            if len(data) < 7:
                raise guard.GuardError(f"обрезанный classic-пакет: {data.hex(' ')}")
            cmd, payload = data[4], data[7:]
        else:
            cmd, payload = data[0], data[3:]
            if cmd & 1:  # продолжение уже одобренного сообщения
                if self._last_approved_cmd is None or (cmd & 0xFE) != self._last_approved_cmd:
                    raise guard.GuardError(
                        f"продолжающий чанк {cmd:#04x} без одобренного первого чанка")
                return "cont"

        name, _ = guard.check(cmd, payload, allow_unknown=self.allow_unknown,
                              allow_writes=self.allow_writes,
                              allow_psk_write=self.allow_psk_write)
        self._last_approved_cmd = cmd
        return name

    def write(self, data, timeout=5):
        data = bytes(data)
        try:
            name = self._inspect(data)
        except guard.GuardError as error:
            self.log.note(f"GUARD ОТКЛОНИЛ: {error}")
            self.log.out(data, "ОТКЛОНЕНО")
            raise
        self.log.out(data, name)
        return super().write(data, timeout)

    def read(self, size=0x10000, timeout=5):
        data = super().read(size, timeout)
        self.log.inp(data)
        return data

    def close(self):
        """Обязательно отпускаем USB-хэндл.

        vendor USBProtocol интерфейс не освобождает, а pyusb захватывает его при
        первой же передаче. Без dispose_resources следующее открытие устройства
        в ТОМ ЖЕ процессе падает с USBError 16 Resource busy — и выглядит это как
        занятость чужим процессом, хотя держим его мы сами.
        """
        try:
            usb.util.dispose_resources(self.device)
        except Exception:  # noqa: BLE001
            pass
        if self._owns_log:
            self.log.close()


def make_factory(framing: str, logbook: Logbook, *, allow_writes=True,
                 allow_unknown=False, allow_psk_write=False):
    """vendor Device(...) ждёт КЛАСС протокола вида proto(vendor, product, timeout).

    Возвращаем совместимую фабрику с зафиксированными фреймингом/логом/политикой.
    """

    def factory(vendor, product, timeout=5):
        return GuardedUSBProtocol(vendor, product, timeout, framing=framing,
                                  logbook=logbook, allow_writes=allow_writes,
                                  allow_unknown=allow_unknown,
                                  allow_psk_write=allow_psk_write)

    return factory
