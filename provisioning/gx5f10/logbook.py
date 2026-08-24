"""Лог всех USB-обменов. Каждая сессия -> отдельный файл в docs/log/.

Это первичный материал для docs/protocol.md, поэтому пишем сырой hex без интерпретации.
"""
import datetime
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOGDIR = ROOT / "docs" / "log"


class Logbook:

    def __init__(self, name: str):
        LOGDIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.path = LOGDIR / f"{stamp}-{name}.log"
        self._fh = self.path.open("w", encoding="utf-8")
        self.note(f"session {name} started at {stamp}")

    def _write(self, line: str) -> None:
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._fh.write(f"{ts} {line}\n")
        self._fh.flush()

    def note(self, text: str) -> None:
        self._write(f"##  {text}")

    def out(self, data: bytes, comment: str = "") -> None:
        self._write(f">>  {data.hex(' ')}{'   # ' + comment if comment else ''}")

    def inp(self, data: bytes, comment: str = "") -> None:
        self._write(f"<<  {data.hex(' ')}{'   # ' + comment if comment else ''}")

    def close(self) -> None:
        if self._fh.closed:      # close() может прийти и из transport, и из вызывающего кода
            return
        self.note("session closed")
        self._fh.close()
