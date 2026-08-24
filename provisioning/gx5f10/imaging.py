"""Обработка сырого кадра сенсора milanG -> чистое изображение отпечатка.

decode_image (4px/6байт, 12-бит) -> 9856 значений -> 56x176. Вычитаем фоновый кадр
(без пальца), нормализуем, CLAHE-усиление контраста гребней.
"""
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "vendor" / "goodix-fp-dump"))
import tool  # noqa: E402

WIDTH, HEIGHT = 56, 176


def raw_to_array(raw: bytes) -> np.ndarray | None:
    """Сырые расшифрованные байты (с TLS-обёрткой) -> 2D float-массив 176x56."""
    if len(raw) < 14:
        return None
    body = raw[8:-5]
    px = tool.decode_image(body)
    if len(px) < WIDTH * HEIGHT:
        return None
    return np.array(px[:WIDTH * HEIGHT], dtype=np.float32).reshape(HEIGHT, WIDTH)


def process(finger_raw: bytes, clear_raw: bytes | None = None) -> np.ndarray | None:
    """Вернуть 8-битное изображение отпечатка (uint8), готовое для SIFT."""
    import cv2

    f = raw_to_array(finger_raw)
    if f is None:
        return None
    if clear_raw is not None:
        c = raw_to_array(clear_raw)
        if c is not None:
            f = c - f  # фон минус палец: гребни становятся светлыми
    # нормализация в 0..255
    lo, hi = np.percentile(f, 2), np.percentile(f, 98)
    if hi <= lo:
        return None
    img = np.clip((f - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8)
    # усиление контраста гребней
    img = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(4, 8)).apply(img)
    # апскейл (сенсор узкий) для устойчивости SIFT
    img = cv2.resize(img, (WIDTH * 4, HEIGHT * 4), interpolation=cv2.INTER_CUBIC)
    img = cv2.GaussianBlur(img, (3, 3), 0)
    return img
