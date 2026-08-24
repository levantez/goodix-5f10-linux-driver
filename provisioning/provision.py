#!/usr/bin/env python3
"""Провижининг Goodix 5f10: записать наш white-box (PSK=0) и проверить TLS.

Что делает:
  1. soft-reset MCU в чистое состояние;
  2. baseline: preset_psk_read(0xbb020003) — текущий хеш PSK;
  3. preset_psk_write(0xbb010003, whitebox_psk0) — ЕДИНСТВЕННАЯ запись во флеш
     (guard пропускает её только с allow_psk_write и только флаг 0xbb010003);
  4. preset_psk_read(0xbb020003) — новый хеш, сверка со старым;
  5. TLS-PSK хендшейк с psk=0 через openssl s_server — проверка приёма ключа.

Запись делается ТОЛЬКО с флагом --write (явное согласие пользователя).
Без --write скрипт лишь читает хеш и печатает, что бы он записал (dry-run).

Прошивочные команды (erase/write_firmware/option-byte) guard блокирует всегда.
"""
import argparse
import pathlib
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from gx5f10 import PRODUCT_ID, recover, transport
from gx5f10.logbook import Logbook

DOCS = pathlib.Path(__file__).resolve().parent.parent / "docs"
WB_FILE = DOCS / "whitebox.hex"
PSK_FILE = DOCS / "psk.hex"
PSK = bytes.fromhex(PSK_FILE.read_text().strip())
PORT = 4433


def write_raw_e0(device, goodix, blob):
    """Отправить сырой 0xe0 (PRESET_PSK_WRITE) с готовым payload = production-blob."""
    device.protocol.write(goodix.encode_message_pack(
        goodix.encode_message_protocol(blob, 0xE0)))
    goodix.check_ack(
        goodix.check_message_protocol(
            goodix.check_message_pack(device.protocol.read()), goodix.COMMAND_ACK), 0xE0)
    reply = goodix.check_message_protocol(
        goodix.check_message_pack(device.protocol.read()), 0xE0)
    return len(reply) >= 1 and reply[0] == 0x00


def read_hash(device, label):
    ok, flags, data = device.preset_psk_read(0xBB020003)
    if not ok:
        print(f"  [{label}] preset_psk_read(0xbb020003) -> не поддержано")
        return None
    print(f"  [{label}] PMK hash = {data.hex()}  (flags {flags:#x})")
    return data


def tls_check(device, log):
    """Вернёт True, если TLS-PSK с psk=0 поднялся."""
    import goodix

    server = subprocess.Popen(
        ["openssl", "s_server", "-nocert", "-psk", PSK.hex(), "-port", str(PORT),
         "-tls1_2", "-cipher", "PSK@SECLEVEL=0", "-quiet"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    try:
        device.reset(True, False, 20)
        hello = device.request_tls_connection()
        client = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        client.settimeout(5)
        client.sendall(hello)
        device.protocol.write(goodix.encode_message_pack(
            client.recv(4096), goodix.FLAGS_TRANSPORT_LAYER_SECURITY))
        for _ in range(3):
            chunk = goodix.check_message_pack(device.protocol.read(),
                                              goodix.FLAGS_TRANSPORT_LAYER_SECURITY)
            client.sendall(chunk)
        reply = client.recv(4096)
        if not reply:
            return False, "openssl закрыл соединение"
        if reply[0] == 0x14:
            return True, "ChangeCipherSpec — TLS принят"
        if reply[0] == 0x15 and len(reply) >= 7:
            return False, f"Alert level={reply[5]} desc={reply[6]}"
        return False, f"неожиданная запись типа {reply[0]:#04x}"
    except Exception as error:  # noqa: BLE001
        return False, f"{type(error).__name__}: {str(error)[:80]}"
    finally:
        server.terminate()
        try:
            server.communicate(timeout=3)
        except Exception:  # noqa: BLE001
            server.kill()


def main() -> int:
    import goodix

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true",
                        help="выполнить запись PSK во флеш (без него — dry-run)")
    parser.add_argument("--check", action="store_true",
                        help="только проверка состояния: полный сброс + чтение хеша")
    parser.add_argument("--combined", action="store_true",
                        help="писать полный production-blob (docs/provision_blob.hex) "
                             "вместо одиночного TLV")
    args = parser.parse_args()

    whitebox = bytes.fromhex(WB_FILE.read_text().strip())
    print(f"white-box (psk=0): {len(whitebox)} байт, {whitebox[:16].hex()}…\n")

    recover.soft_mcu_reset()
    log = Logbook("provision")
    factory = transport.make_factory("classic", log, allow_writes=True,
                                     allow_psk_write=args.write)
    device = goodix.Device(PRODUCT_ID, factory)

    try:
        device.nop()
        try:
            device.enable_chip(True)
            device.nop()
        except Exception:  # noqa: BLE001
            pass
        print(f"firmware: {device.firmware_version()}")

        if args.check:
            print("\n[check] полный reset(0xa2) затем чтение хеша:")
            try:
                success, number = device.reset(True, False, 20)
                print(f"  reset: success={success} number={number}")
            except Exception as error:  # noqa: BLE001
                print(f"  reset: {type(error).__name__}: {error}")
            h = read_hash(device, "check")
            print("  здоров" if h is not None else "  psk-чтение всё ещё в ошибке")
            return 0 if h is not None else 8

        print("\n1. хеш ДО:")
        before = read_hash(device, "до")

        if not args.write:
            print("\n[dry-run] записал бы preset_psk_write(0xbb010003, white-box).")
            print("Повтори с --write, чтобы выполнить запись во флеш.")
            return 0

        if args.combined:
            blob = bytes.fromhex((DOCS / "provision_blob.hex").read_text().strip())
            print(f"\n2. ЗАПИСЬ полного production-blob ({len(blob)} байт, 0xe0) …")
            ok = write_raw_e0(device, goodix, blob)
            print(f"   устройство ответило: {'OK' if ok else 'ОТКАЗ'} (raw={ok})")
        else:
            print("\n2. ЗАПИСЬ preset_psk_write(0xbb010003, white-box) …")
            ok = device.preset_psk_write(0xBB010003, whitebox)
            print(f"   устройство ответило: {'OK' if ok else 'ОТКАЗ'} (raw={ok})")

        print("\n3. хеш ПОСЛЕ:")
        after = read_hash(device, "после")
        if before is not None and after is not None:
            print(f"   хеш {'СМЕНИЛСЯ' if before != after else 'НЕ изменился'}")

        print("\n4. TLS-PSK хендшейк с psk=0:")
        success, why = tls_check(device, log)
        print(f"   {'*** УСПЕХ: ' if success else 'не прошёл: '}{why}")
        return 0 if success else 7
    finally:
        try:
            device.protocol.close()
        except Exception:  # noqa: BLE001
            pass
        log.close()
        print(f"\nлог: {log.path}")


if __name__ == "__main__":
    sys.exit(main())
