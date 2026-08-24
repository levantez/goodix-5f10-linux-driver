"""Переиспользуемый захват кадра с сенсора milanG (после провижининга).

Высокоуровневая функция capture_frames(): поднимает TLS через openssl-мост, грузит
milanG-конфиг, снимает фоновый кадр (без пальца) и кадр с пальцем, возвращает сырые
расшифрованные байты. Используется enroll/verify и capture_image.
"""
import pathlib
import select
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "vendor" / "goodix-fp-dump"))

from . import PRODUCT_ID, milang, recover, transport
from .logbook import Logbook

DOCS = pathlib.Path(__file__).resolve().parent.parent / "docs"
PSK = bytes.fromhex((DOCS / "psk.hex").read_text().strip())
PORT = 4433
WIDTH, HEIGHT = 56, 176

FDT_MODE_A = b"\x0d\x01\xb2\xb2\xc2\xc2\xa7\xa7\xb6\xb6\xa6\xa6\xb6\xb6"
FDT_MODE_B = b"\x0d\x01\x80\xb0\x80\xc0\x80\xa4\x80\xb4\x80\xa3\x80\xb4"
FDT_MODE_C = b"\x0d\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
FDT_DOWN_C = b"\x0c\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
FDT_DOWN_D = b"\x0c\x01\x80\xb2\x80\xc2\x80\xa7\x80\xb6\x80\xa6\x80\xb6"


def _read_dec(server, want=20000):
    buf = b""
    deadline = time.time() + 6
    while time.time() < deadline and len(buf) < want:
        r, _, _ = select.select([server.stdout], [], [], 1.0)
        if not r:
            break
        chunk = server.stdout.read1(8192)
        if not chunk:
            break
        buf += chunk
    return buf


class Session:
    """Одна TLS-сессия захвата. Контекстный менеджер."""

    def __init__(self, label="capture"):
        self.log = Logbook(label)
        self.server = None
        self.device = None
        self.client = None

    def __enter__(self):
        import goodix
        import tool
        self.goodix = goodix
        self.tool = tool
        recover.soft_mcu_reset()
        self.device = goodix.Device(PRODUCT_ID,
                                    transport.make_factory("classic", self.log, allow_writes=True))
        self.server = subprocess.Popen(
            ["openssl", "s_server", "-nocert", "-psk", PSK.hex(), "-port", str(PORT),
             "-tls1_2", "-cipher", "PSK@SECLEVEL=0", "-quiet"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(1.0)
        d = self.device
        d.nop()
        try:
            d.enable_chip(True); d.nop()
        except Exception:  # noqa: BLE001
            pass
        d.reset(True, False, 20)
        otp = bytes(d.read_otp())            # конфиг генерится под ЭТО устройство
        d.reset(True, False, 20)
        config = milang.build_config(otp)    # DAC из OTP этого сенсора
        if not d.upload_config_mcu(config):
            raise RuntimeError("устройство отвергло milanG-конфиг")
        d.set_powerdown_scan_frequency(100)
        self.client = socket.create_connection(("localhost", PORT), timeout=5)
        self.client.settimeout(6)
        self.tool.connect_device(d, self.client)
        d.tls_successfully_established()
        d.nop()
        d.query_mcu_state(b"\x55", True)
        d.mcu_switch_to_fdt_mode(FDT_MODE_A, True)
        d.nav()
        d.mcu_switch_to_fdt_mode(FDT_MODE_B, True)
        d.read_sensor_register(0x0082, 2)
        return self

    def grab(self):
        """Снять один кадр -> сырые расшифрованные байты."""
        enc = self.device.mcu_get_image(b"\x01\x00",
                                        self.goodix.FLAGS_TRANSPORT_LAYER_SECURITY)
        self.client.sendall(enc)
        return _read_dec(self.server)

    def arm_finger(self):
        """Перевести в режим ожидания пальца (fdt down)."""
        self.device.mcu_switch_to_fdt_mode(FDT_MODE_C, True)
        self.device.mcu_switch_to_fdt_down(FDT_DOWN_C, True)
        self.device.nop()
        self.device.query_mcu_state(b"\x55", True)

    def grab_finger(self):
        self.device.mcu_switch_to_fdt_down(FDT_DOWN_D, True)
        return self.grab()

    def __exit__(self, *a):
        if self.server:
            self.server.terminate()
            try:
                self.server.communicate(timeout=2)
            except Exception:  # noqa: BLE001
                self.server.kill()
        try:
            self.device.protocol.close()
        except Exception:  # noqa: BLE001
            pass
        self.log.close()
