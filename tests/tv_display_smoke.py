#!/usr/bin/env python3
"""Smoke tests for the read-only CRT display's Reality-state reader."""
from __future__ import annotations

import importlib.util
import sqlite3
import socket
import tempfile
import threading
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "TV.py"
SPEC = importlib.util.spec_from_file_location("r2_tv_display", MODULE_PATH)
TV = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(TV)


class TVDisplaySmoke(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.db_path = Path(self.tempdir.name) / "reality.db"
        with sqlite3.connect(self.db_path) as db:
            db.executescript("""
                CREATE TABLE r2_tv_state (
                    id INTEGER PRIMARY KEY, power INTEGER NOT NULL,
                    source_kind TEXT NOT NULL, source_value INTEGER NOT NULL
                );
                CREATE TABLE r2_tv_devices (
                    name TEXT PRIMARY KEY, connected INTEGER NOT NULL,
                    connection_kind TEXT NOT NULL, port INTEGER NOT NULL
                );
            """)
        TV.DB_PATH = self.db_path
        TV.SOCKET_PATH = Path(self.tempdir.name) / "tv-control.sock"

    def set_state(self, power: int, kind: str, value: int) -> None:
        with sqlite3.connect(self.db_path) as db:
            db.execute("INSERT OR REPLACE INTO r2_tv_state VALUES(1,?,?,?)",
                       (power, kind, value))

    def add_device(self, name: str, kind: str, port: int) -> None:
        with sqlite3.connect(self.db_path) as db:
            db.execute("INSERT INTO r2_tv_devices VALUES(?,?,?,?)",
                       (name, 1, kind, port))

    def test_empty_rf_channel_has_no_device(self) -> None:
        self.set_state(1, "rf", 3)
        self.assertEqual(TV.CRTDisplay.read_state(object.__new__(TV.CRTDisplay)),
                         (1, "rf", 3, None))

    def test_connected_rf_device_is_detected(self) -> None:
        self.set_state(1, "rf", 3)
        self.add_device("Atari 2600", "rf", 3)
        self.assertEqual(TV.CRTDisplay.read_state(object.__new__(TV.CRTDisplay)),
                         (1, "rf", 3, "Atari 2600"))

    def test_external_av_device_is_detected(self) -> None:
        self.set_state(1, "input", 2)
        self.add_device("Game console", "input", 2)
        self.assertEqual(TV.CRTDisplay.read_state(object.__new__(TV.CRTDisplay)),
                         (1, "input", 2, "Game console"))

    def test_builtin_vcr_does_not_need_external_device(self) -> None:
        self.set_state(1, "input", 1)
        self.assertEqual(TV.CRTDisplay.read_state(object.__new__(TV.CRTDisplay)),
                         (1, "input", 1, None))

    def test_reader_does_not_mutate_reality_database(self) -> None:
        self.set_state(0, "input", 1)
        before = self.db_path.read_bytes()
        TV.CRTDisplay.read_state(object.__new__(TV.CRTDisplay))
        self.assertEqual(self.db_path.read_bytes(), before)

    def test_control_client_uses_local_socket(self) -> None:
        received = []
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(TV.SOCKET_PATH))
        server.listen(1)

        def serve_once() -> None:
            with server:
                conn, _ = server.accept()
                with conn:
                    received.append(conn.recv(256).decode("utf-8"))
                    conn.sendall(b"OK input selected")

        worker = threading.Thread(target=serve_once)
        worker.start()
        self.assertEqual(TV.send_control_command("input 2"), "OK input selected")
        worker.join(timeout=2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(received, ["input 2\\n"])

    def test_control_client_rejects_unsupported_commands(self) -> None:
        with self.assertRaises(ValueError):
            TV.send_control_command("sql update r2_tv_state")


if __name__ == "__main__":
    unittest.main()
