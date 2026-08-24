"""Подключает референсную реализацию семейства (vendor/goodix-fp-dump, MIT) к sys.path."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor" / "goodix-fp-dump"

if not VENDOR.is_dir():
    raise RuntimeError(f"нет референсной реализации: {VENDOR}")

if str(VENDOR) not in sys.path:
    sys.path.insert(0, str(VENDOR))
