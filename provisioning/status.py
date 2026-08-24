#!/usr/bin/env python3
"""Состояние сенсора: адрес, драйверы, доступность узла. Ничего не отправляет."""
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from gx5f10 import recover


def main() -> int:
    try:
        dev = recover.find()
    except FileNotFoundError as error:
        print(f"НЕ НАЙДЕН: {error}")
        return 1

    print(f"sysfs      : {dev['sysfs']}")
    print(f"адрес      : шина {dev['busnum']} устройство {dev['devnum']}")
    print(f"узел       : {dev['node']}")
    print(f"product    : {dev['product']}")
    print(f"serial     : {dev['serial']}  bcdDevice: {dev['bcdDevice']}")
    print(f"драйверы   : {recover.drivers()}")

    node = dev["node"]
    readable = os.access(node, os.R_OK)
    writable = os.access(node, os.W_OK)
    print(f"доступ     : read={readable} write={writable}")
    if not writable:
        print("           -> нужно udev-правило (udev/70-goodix-5f10.rules) либо sudo;")
        print("              без права записи не работают ни обмен, ни USBDEVFS_RESET")
    print(f"живой      : {recover.alive()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
