"""Прямой низкоуровневый доступ к сенсору для фазы опознания.

Отличия от transport.GuardedUSBProtocol (который нужен для полноценного драйвера):
  * короткие таймауты — никаких 500-секундных ожиданий из vendor-кода;
  * слушаем ОБА входа: bulk IN 0x81 и interrupt IN 0x82 (CDC-нотификации);
  * умеет CDC-запросы (DTR/RTS) — устройство заявляет себя как ACM, и настоящий
    ACM-хост поднимает DTR прежде, чем пойдут данные;
  * есть полностью пассивный режим: слушать, ничего не отправляя.

Guard применяется к каждой исходящей посылке ровно так же, как в transport.
"""
import usb.core
import usb.util

from . import PRODUCT_ID, VENDOR_ID, guard
from .logbook import Logbook

CDC_SET_LINE_CODING = 0x20
CDC_SET_CONTROL_LINE_STATE = 0x22
FLAGS_MESSAGE_PROTOCOL = 0xA0


class RawLink:

    def __init__(self, logbook: Logbook, *, allow_writes=False, allow_unknown=False,
                 warmup=True):
        self.log = logbook
        self.allow_writes = allow_writes
        self.allow_unknown = allow_unknown
        self._last_approved_cmd: int | None = None

        self.device = usb.core.find(idVendor=VENDOR_ID, idProduct=PRODUCT_ID)
        if self.device is None:
            raise ConnectionError(f"{VENDOR_ID:#06x}:{PRODUCT_ID:#06x} не найден")

        self.device.set_configuration()
        cfg = self.device.get_active_configuration()

        self.iface_comm = None   # CDC control (класс 2), несёт interrupt IN
        self.iface_data = None   # CDC data (класс 10), несёт bulk IN/OUT
        self.ep_bulk_in = self.ep_bulk_out = self.ep_int_in = None

        for iface in cfg:
            for ep in iface:
                direction = usb.util.endpoint_direction(ep.bEndpointAddress)
                ep_type = usb.util.endpoint_type(ep.bmAttributes)
                if ep_type == usb.util.ENDPOINT_TYPE_BULK:
                    self.iface_data = iface.bInterfaceNumber
                    if direction == usb.util.ENDPOINT_IN:
                        self.ep_bulk_in = ep.bEndpointAddress
                    else:
                        self.ep_bulk_out = ep.bEndpointAddress
                elif ep_type == usb.util.ENDPOINT_TYPE_INTR and direction == usb.util.ENDPOINT_IN:
                    self.iface_comm = iface.bInterfaceNumber
                    self.ep_int_in = ep.bEndpointAddress

        for number in (self.iface_comm, self.iface_data):
            if number is None:
                continue
            if self.device.is_kernel_driver_active(number):
                self.log.note(f"отцепляю драйвер ядра от интерфейса {number}")
                self.device.detach_kernel_driver(number)
            usb.util.claim_interface(self.device, number)

        self.log.note(f"comm iface={self.iface_comm} int_in={self.ep_int_in:#04x}; "
                      f"data iface={self.iface_data} "
                      f"bulk out={self.ep_bulk_out:#04x} in={self.ep_bulk_in:#04x}")
        if warmup:
            self.warmup()

    def warmup(self) -> None:
        """Холостой NOP сразу после открытия.

        Первая команда после открытия устройства ВСЕГДА остаётся без ответа —
        сенсор её проглатывает. Без прогрева любая однокомандная операция
        (проверка ACK, одиночный RESET) молча не доходит до MCU.
        Именно поэтому все драйверы семейства начинаются с nop().
        """
        from . import frames
        try:
            self.send(frames.classic(0x00, b"\x00\x00\x00\x00", inner_checksum=False),
                      "classic", timeout_ms=1000)
            self.drain(rounds=2, timeout_ms=200)
            self.log.note("прогрев выполнен (холостой NOP проглочен)")
        except Exception as error:  # noqa: BLE001
            self.log.note(f"прогрев не удался: {error}")

    # ---------- отправка ----------

    def _inspect(self, data: bytes, framing: str) -> str:
        if framing == "raw":
            if not self.allow_writes:
                raise guard.GuardError("raw-посылка запрещена в read-only режиме")
            return "raw"
        if framing == "classic":
            if data[0] != FLAGS_MESSAGE_PROTOCOL:
                return f"raw(flags={data[0]:#04x})"
            cmd, payload = data[4], data[7:]
        else:
            cmd, payload = data[0], data[3:]
            if cmd & 1:
                if self._last_approved_cmd is None or (cmd & 0xFE) != self._last_approved_cmd:
                    raise guard.GuardError(f"продолжающий чанк {cmd:#04x} без одобренного первого")
                return "cont"
        name, _ = guard.check(cmd, payload, allow_unknown=self.allow_unknown,
                              allow_writes=self.allow_writes)
        self._last_approved_cmd = cmd
        return name

    def send(self, data: bytes, framing: str, timeout_ms: int = 1000) -> str:
        data = bytes(data)
        name = self._inspect(data, framing)
        padded = data + b"\x00" * (-len(data) % 0x40)
        self.log.out(data, name)
        for i in range(0, len(padded), 0x40):
            self.device.write(self.ep_bulk_out, padded[i:i + 0x40], timeout_ms)
        return name

    # ---------- чтение ----------

    def read_bulk(self, timeout_ms: int = 300, size: int = 0x1000) -> bytes | None:
        try:
            data = self.device.read(self.ep_bulk_in, size, timeout_ms).tobytes()
        except usb.core.USBError:
            return None
        if data:
            self.log.inp(data, "bulk")
        return data or None

    def read_interrupt(self, timeout_ms: int = 300, size: int = 64) -> bytes | None:
        if self.ep_int_in is None:
            return None
        try:
            data = self.device.read(self.ep_int_in, size, timeout_ms).tobytes()
        except usb.core.USBError:
            return None
        if data:
            self.log.inp(data, "interrupt")
        return data or None

    def drain(self, rounds: int = 4, timeout_ms: int = 250) -> list[tuple[str, bytes]]:
        """Собрать всё, что придёт по обоим входам."""
        got = []
        for _ in range(rounds):
            bulk = self.read_bulk(timeout_ms)
            if bulk:
                got.append(("bulk", bulk))
            intr = self.read_interrupt(timeout_ms)
            if intr:
                got.append(("interrupt", intr))
            if not bulk and not intr:
                break
        return got

    # ---------- CDC ----------

    def cdc_set_control_line_state(self, dtr: bool, rts: bool) -> None:
        """Поднять DTR/RTS. Стандартный ACM-запрос, флеша и PSK не касается."""
        if not self.allow_writes:
            raise guard.GuardError("CDC SET_CONTROL_LINE_STATE — запись, режим read-only")
        value = (0x01 if dtr else 0) | (0x02 if rts else 0)
        self.log.note(f"CDC SET_CONTROL_LINE_STATE dtr={dtr} rts={rts} (value={value:#04x})")
        self.device.ctrl_transfer(0x21, CDC_SET_CONTROL_LINE_STATE, value,
                                  self.iface_comm or 0, None, 1000)

    def cdc_set_line_coding(self, baud=115200) -> None:
        if not self.allow_writes:
            raise guard.GuardError("CDC SET_LINE_CODING — запись, режим read-only")
        import struct
        data = struct.pack("<IBBB", baud, 0, 0, 8)  # 1 стоп-бит, без чётности, 8 бит
        self.log.note(f"CDC SET_LINE_CODING {baud} 8N1")
        self.device.ctrl_transfer(0x21, CDC_SET_LINE_CODING, 0,
                                  self.iface_comm or 0, data, 1000)

    def close(self) -> None:
        for number in (self.iface_comm, self.iface_data):
            if number is not None:
                try:
                    usb.util.release_interface(self.device, number)
                except usb.core.USBError:
                    pass
        usb.util.dispose_resources(self.device)
