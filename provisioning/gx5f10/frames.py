"""Сборка кадров обоих фреймингов — без обращения к устройству, чисто арифметика."""
import struct

FLAGS_MESSAGE_PROTOCOL = 0xA0


def classic(command: int, payload: bytes, *, inner_checksum: bool = True) -> bytes:
    """flags(1) len(2) csum(1) | cmd(1) len(2) payload csum(1)"""
    inner = struct.pack("<B", command) + struct.pack("<H", len(payload) + 1) + payload
    inner += struct.pack("<B", 0xAA - sum(inner) & 0xFF if inner_checksum else 0x88)
    head = struct.pack("<B", FLAGS_MESSAGE_PROTOCOL) + struct.pack("<H", len(inner))
    head += struct.pack("<B", sum(head) & 0xFF)
    return head + inner


def wrapless(category: int, command: int, payload: bytes, *, checksum: bool = True) -> bytes:
    """cmd(1) len(2) payload csum(1)"""
    command_byte = category << 4 | command << 1
    data = struct.pack("<B", command_byte) + struct.pack("<H", len(payload) + 1) + payload
    data += struct.pack("<B", 0xAA - sum(data) & 0xFF if checksum else 0x88)
    return data
