#!/usr/bin/env python3
"""Export selected, decrypted s2c frames to the bounded DHR1 replay format.

DHR1 layout (little-endian):
    char magic[4] = "DHR1"
    repeated records:
        u32 elapsed_ms, u16 command, u8 direction, u8 reserved_flags,
        u32 payload_size, byte payload[payload_size]

The payload excludes the two-byte command. Reserved flags must be zero and
payloads are limited to 8000 bytes. Login command 0x4004 is always rejected.

A raw reassembled stream has no packet timestamps. Exported records therefore
use elapsed_ms=0 to mean unknown timing, preserving frame order without
inventing delays.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.kimi_msg_crypto import decrypt_payload, load_key

MAGIC = b"DHR1"
MAX_PAYLOAD_SIZE = 8000
LOGIN_COMMAND = 0x4004
RECORD_HEADER = struct.Struct("<I H B B I")
WIRE_HEADER_SIZE = 2
MIN_S2C_PLAINTEXT_SIZE = 0x12


class Direction(IntEnum):
    S2C = 0
    C2S = 1


@dataclass(frozen=True)
class ReplayRecord:
    elapsed_ms: int
    command: int
    direction: Direction
    payload: bytes


def _validate_record(record: ReplayRecord, index: int) -> None:
    if not 0 <= record.elapsed_ms <= 0xFFFFFFFF:
        raise ValueError(f"record {index}: timestamp is outside u32 range")
    if not 0 <= record.command <= 0xFFFF:
        raise ValueError(f"record {index}: command is outside u16 range")
    if record.command == LOGIN_COMMAND:
        raise ValueError(f"record {index}: login command 0x4004 is forbidden")
    try:
        Direction(record.direction)
    except ValueError as error:
        raise ValueError(f"record {index}: invalid direction") from error
    if len(record.payload) > MAX_PAYLOAD_SIZE:
        raise ValueError(f"record {index}: payload exceeds {MAX_PAYLOAD_SIZE} bytes")


def encode_replay(records) -> bytes:
    output = bytearray(MAGIC)
    previous_elapsed = 0
    for index, record in enumerate(records):
        _validate_record(record, index)
        if index and record.elapsed_ms < previous_elapsed:
            raise ValueError(f"record {index}: timestamp is not monotonic")
        output += RECORD_HEADER.pack(
            record.elapsed_ms,
            record.command,
            int(record.direction),
            0,
            len(record.payload),
        )
        output += record.payload
        previous_elapsed = record.elapsed_ms
    return bytes(output)


def decode_replay(data: bytes) -> list[ReplayRecord]:
    if len(data) < len(MAGIC) or data[: len(MAGIC)] != MAGIC:
        raise ValueError("invalid DHR1 magic")

    records = []
    offset = len(MAGIC)
    previous_elapsed = 0
    while offset < len(data):
        index = len(records)
        if len(data) - offset < RECORD_HEADER.size:
            raise ValueError(f"record {index}: truncated record header")
        elapsed_ms, command, direction_value, flags, payload_size = RECORD_HEADER.unpack_from(
            data, offset
        )
        offset += RECORD_HEADER.size
        if command == LOGIN_COMMAND:
            raise ValueError(f"record {index}: login command 0x4004 is forbidden")
        if payload_size > MAX_PAYLOAD_SIZE:
            raise ValueError(f"record {index}: payload exceeds {MAX_PAYLOAD_SIZE} bytes")
        if payload_size > len(data) - offset:
            raise ValueError(f"record {index}: truncated payload")
        if flags != 0:
            raise ValueError(f"record {index}: nonzero reserved flags")
        try:
            direction = Direction(direction_value)
        except ValueError as error:
            raise ValueError(f"record {index}: invalid direction {direction_value}") from error
        if records and elapsed_ms < previous_elapsed:
            raise ValueError(f"record {index}: timestamp is not monotonic")
        payload = data[offset : offset + payload_size]
        offset += payload_size
        records.append(ReplayRecord(elapsed_ms, command, direction, payload))
        previous_elapsed = elapsed_ms
    return records


def parse_commands(text: str) -> set[int]:
    commands = set()
    for token in text.split(","):
        token = token.strip()
        if not token:
            raise ValueError("commands must be a comma-separated list without empty items")
        try:
            command = int(token, 0)
        except ValueError as error:
            raise ValueError(f"invalid command: {token!r}") from error
        if not 0 <= command <= 0xFFFF:
            raise ValueError(f"command outside u16 range: {token!r}")
        if command == LOGIN_COMMAND:
            raise ValueError("login command 0x4004 cannot be exported")
        commands.add(command)
    if not commands:
        raise ValueError("at least one explicit command is required")
    return commands


def export_s2c_stream(
    stream: bytes,
    commands: set[int],
    *,
    max_records: int = 1000,
    key: bytes | None = None,
) -> list[ReplayRecord]:
    """Strictly decode a complete s2c stream; malformed input fails the export."""
    if not commands or LOGIN_COMMAND in commands:
        raise ValueError("commands must be explicit and cannot contain 0x4004")
    if max_records <= 0:
        raise ValueError("max-records must be positive")
    if key is None:
        key = load_key()

    records = []
    offset = 0
    frame_index = 0
    while offset < len(stream):
        if len(stream) - offset < 4:
            raise ValueError(f"frame {frame_index}: truncated wire header at offset {offset:#x}")
        wire_size = struct.unpack_from("<H", stream, offset)[0]
        if wire_size < MIN_S2C_PLAINTEXT_SIZE:
            raise ValueError(
                f"frame {frame_index}: wire payload too short for s2c decryption at offset {offset:#x}"
            )
        if wire_size - WIRE_HEADER_SIZE > MAX_PAYLOAD_SIZE:
            raise ValueError(f"frame {frame_index}: payload exceeds {MAX_PAYLOAD_SIZE} bytes")
        frame_end = offset + WIRE_HEADER_SIZE + wire_size
        if frame_end > len(stream):
            raise ValueError(f"frame {frame_index}: truncated wire payload at offset {offset:#x}")

        plaintext = decrypt_payload(stream[offset + WIRE_HEADER_SIZE : frame_end], key)
        if len(plaintext) != wire_size:
            raise ValueError(f"frame {frame_index}: decrypted size changed")
        command = struct.unpack_from("<H", plaintext, 0)[0]
        if command == LOGIN_COMMAND:
            raise ValueError(f"frame {frame_index}: login command 0x4004 is forbidden")
        if command in commands:
            if len(records) >= max_records:
                raise ValueError(
                    f"frame {frame_index}: matching records exceed max-records={max_records}"
                )
            records.append(ReplayRecord(0, command, Direction.S2C, plaintext[2:]))

        offset = frame_end
        frame_index += 1
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stream", required=True, help="complete reassembled s2c TCP stream")
    parser.add_argument("--out", required=True, help="output DHR1 path")
    parser.add_argument(
        "--commands",
        required=True,
        help="explicit comma-separated commands, for example 0x303f,0x301e; no wildcard",
    )
    parser.add_argument(
        "--max-records",
        type=int,
        default=1000,
        help="positive selected-record limit; excess matches fail instead of truncating (default: 1000)",
    )
    args = parser.parse_args()
    try:
        commands = parse_commands(args.commands)
        records = export_s2c_stream(
            Path(args.stream).read_bytes(), commands, max_records=args.max_records
        )
        Path(args.out).write_bytes(encode_replay(records))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"exported {len(records)} records to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
