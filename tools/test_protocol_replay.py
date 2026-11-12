import struct
import unittest
from unittest import mock

from tools.export_protocol_replay import (
    MAGIC,
    MAX_PAYLOAD_SIZE,
    Direction,
    ReplayRecord,
    decode_replay,
    encode_replay,
    export_s2c_stream,
    parse_commands,
)


class ReplayFormatTests(unittest.TestCase):
    def setUp(self):
        self.records = [
            ReplayRecord(0, 0x303F, Direction.S2C, b"chat"),
            ReplayRecord(20, 0x301E, Direction.S2C, b"object"),
        ]

    def test_two_record_round_trip_and_golden_bytes(self):
        encoded = encode_replay(self.records)
        self.assertEqual(
            encoded,
            b"DHR1"
            + b"\x00\x00\x00\x00\x3f\x30\x00\x00\x04\x00\x00\x00chat"
            + b"\x14\x00\x00\x00\x1e\x30\x00\x00\x06\x00\x00\x00object",
        )
        self.assertEqual(decode_replay(encoded), self.records)

    def test_empty_replay_and_equal_timestamps_are_valid(self):
        self.assertEqual(decode_replay(MAGIC), [])
        records = [
            ReplayRecord(0, 0x303F, Direction.S2C, b""),
            ReplayRecord(0, 0x301E, Direction.C2S, b"binary\x00\xff"),
        ]
        self.assertEqual(decode_replay(encode_replay(records)), records)

    def test_login_records_are_rejected_before_encoding_or_decoding(self):
        with self.assertRaisesRegex(ValueError, "record 0.*0x4004"):
            encode_replay([ReplayRecord(0, 0x4004, Direction.C2S, b"secret")])
        raw = bytearray(encode_replay(self.records[:1]))
        struct.pack_into("<H", raw, 8, 0x4004)
        with self.assertRaisesRegex(ValueError, "record 0.*0x4004"):
            decode_replay(raw)

    def test_excessive_payload_is_rejected(self):
        maximum = ReplayRecord(0, 0x303F, Direction.S2C, b"x" * MAX_PAYLOAD_SIZE)
        self.assertEqual(decode_replay(encode_replay([maximum])), [maximum])
        with self.assertRaisesRegex(ValueError, "record 0.*payload"):
            encode_replay(
                [ReplayRecord(0, 0x303F, Direction.S2C, b"x" * (MAX_PAYLOAD_SIZE + 1))]
            )
        raw = bytearray(MAGIC + struct.pack("<I H B B I", 0, 0x303F, 0, 0, 8001))
        with self.assertRaisesRegex(ValueError, "record 0.*payload"):
            decode_replay(raw)

    def test_truncated_header_payload_and_trailing_bytes_are_rejected(self):
        for raw in (MAGIC + b"\x00" * 11, encode_replay(self.records[:1]) + b"\x00"):
            with self.subTest(raw=raw):
                with self.assertRaisesRegex(ValueError, "record [01].*truncated"):
                    decode_replay(raw)
        raw = bytearray(encode_replay(self.records[:1]))
        raw.pop()
        with self.assertRaisesRegex(ValueError, "record 0.*truncated"):
            decode_replay(raw)

    def test_non_monotonic_time_is_rejected(self):
        raw = bytearray(encode_replay(self.records))
        struct.pack_into("<I", raw, 4, 20)
        struct.pack_into("<I", raw, 20, 19)
        with self.assertRaisesRegex(ValueError, "record 1.*timestamp"):
            decode_replay(raw)
        with self.assertRaisesRegex(ValueError, "record 1.*timestamp"):
            encode_replay([self.records[1], self.records[0]])

    def test_reserved_flags_and_invalid_direction_are_rejected(self):
        for offset, value, description in ((11, 1, "flags"), (10, 2, "direction")):
            with self.subTest(description=description):
                raw = bytearray(encode_replay(self.records[:1]))
                raw[offset] = value
                with self.assertRaisesRegex(ValueError, f"record 0.*{description}"):
                    decode_replay(raw)

    def test_bad_magic_is_rejected(self):
        for raw in (b"", b"DHR", b"DHR0"):
            with self.subTest(raw=raw):
                with self.assertRaisesRegex(ValueError, "magic"):
                    decode_replay(raw)


class StreamExportTests(unittest.TestCase):
    @staticmethod
    def plain(command, marker):
        return struct.pack("<H", command) + bytes([marker]) * 16

    @staticmethod
    def wire_frame(plain):
        return struct.pack("<H", len(plain)) + plain

    def export(self, stream, commands, max_records=100):
        with mock.patch("tools.export_protocol_replay.load_key", return_value=b"test key"), mock.patch(
            "tools.export_protocol_replay.decrypt_payload",
            side_effect=lambda payload, key: payload,
        ) as decrypt:
            records = export_s2c_stream(stream, commands, max_records=max_records)
        self.assertTrue(decrypt.called)
        return records

    def test_selected_frames_export_in_order_with_unknown_time_set_to_zero(self):
        chat = self.plain(0x303F, 0x11)
        ignored = self.plain(0x300D, 0x22)
        npc = self.plain(0x301E, 0x33)
        stream = self.wire_frame(chat) + self.wire_frame(ignored) + self.wire_frame(npc)
        records = self.export(stream, {0x303F, 0x301E})
        self.assertEqual(
            records,
            [
                ReplayRecord(0, 0x303F, Direction.S2C, chat[2:]),
                ReplayRecord(0, 0x301E, Direction.S2C, npc[2:]),
            ],
        )

    def test_login_frame_is_rejected_even_when_not_selected(self):
        stream = self.wire_frame(self.plain(0x4004, 0x44))
        with self.assertRaisesRegex(ValueError, "frame 0.*0x4004"):
            self.export(stream, {0x303F})

    def test_short_or_truncated_input_frames_are_rejected(self):
        valid = self.wire_frame(self.plain(0x303F, 0x55))
        for stream in (b"\x01\x00x", valid[:-1], b"\x02\x00\x3f\x30"):
            with self.subTest(stream=stream):
                with self.assertRaisesRegex(ValueError, "frame 0"):
                    self.export(stream, {0x303F})

    def test_record_limit_fails_closed_instead_of_silent_truncation(self):
        frame = self.wire_frame(self.plain(0x303F, 0x66))
        with self.assertRaisesRegex(ValueError, "max-records"):
            self.export(frame + frame, {0x303F}, max_records=1)

    def test_explicit_command_parser_rejects_wildcards_and_login(self):
        self.assertEqual(parse_commands("0x303f,12318,0x303F"), {0x303F, 0x301E})
        for text in ("", "all", "0x4004", "0x10000", "0x303f,"):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    parse_commands(text)


class CommandLineTests(unittest.TestCase):
    def test_direct_script_entry_point(self):
        import subprocess
        import sys
        from pathlib import Path

        root = Path(__file__).resolve().parent.parent
        result = subprocess.run(
            [sys.executable, str(root / "tools" / "export_protocol_replay.py"), "--help"],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--commands", result.stdout)


if __name__ == "__main__":
    unittest.main()
