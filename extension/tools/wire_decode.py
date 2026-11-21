#!/usr/bin/env python3
"""An independent WIRE decoder, written from WIRE.md and from nothing else.

wire_decode.py --spec wire.spec.json --record CommandHeader --hex 0a1b2c
wire_decode.py --self-test                          # the decoder alone
wire_decode.py --self-test --spec reports/native/wire.spec.json
wire_decode.py --capture run.netwcap --verbose      # WIRE.md 21
"""

import argparse
import json
import struct
import sys

MAX_VARINT_BYTES = 10


class Poisoned(Exception):
    """A read the stream cannot answer. Never a wrong value, always a refusal."""


def bits_required(span):
    """Bits needed to hold `span`. Zero for a span of zero."""
    width = 0
    while span > 0:
        width += 1
        span >>= 1
    return width


class ReadStream:
    """Little-endian bit reader. Within a byte the first bit read is bit 0."""

    def __init__(self, data):
        self.data = bytes(data)
        self.read = 0

    def remaining(self):
        return len(self.data) * 8 - self.read

    def bits(self, width):
        if width < 0 or width > 64:
            raise Poisoned("bit width %d is out of range" % width)
        if width == 0:
            return 0
        if self.remaining() < width:
            raise Poisoned("wanted %d bits, %d remain" % (width, self.remaining()))
        value = 0
        taken = 0
        while taken < width:
            byte_at = (self.read + taken) >> 3
            within = (self.read + taken) & 7
            available = 8 - within
            wanted = width - taken
            taking = wanted if wanted < available else available
            chunk = (self.data[byte_at] >> within) & ((1 << taking) - 1)
            value |= chunk << taken
            taken += taking
        self.read += width
        return value

    def bool1(self):
        return self.bits(1) != 0

    def int_range(self, low, high):
        if low > high:
            raise Poisoned("range bounds %d..%d are inverted" % (low, high))
        return low + self.bits(bits_required(high - low))

    def align_verify(self):
        pad = (8 - (self.read % 8)) % 8
        if pad and self.bits(pad) != 0:
            raise Poisoned("alignment padding is not zero")

    def varuint(self, max_bytes=MAX_VARINT_BYTES):
        if max_bytes < 1 or max_bytes > MAX_VARINT_BYTES:
            raise Poisoned("varint byte limit %d is out of range" % max_bytes)
        self.align_verify()
        staged = 0
        for at in range(max_bytes):
            byte = self.bits(8)
            if at >= 9 and (byte & 0x7E) != 0:
                raise Poisoned("varint does not fit 64 bits")
            staged |= (byte & 0x7F) << (at * 7)
            if not byte & 0x80:
                if at > 0 and (byte & 0x7F) == 0:
                    raise Poisoned("varint is not canonical")
                return staged
        raise Poisoned("varint exceeds its %d-byte limit" % max_bytes)

    def svarint(self, max_bytes=MAX_VARINT_BYTES):
        staged = self.varuint(max_bytes)
        return (staged >> 1) ^ -(staged & 1)

    def bytes_capped(self, cap):
        if cap < 0:
            raise Poisoned("blob cap %d is negative" % cap)
        length = self.int_range(0, cap)
        self.align_verify()
        if self.remaining() < length * 8:
            raise Poisoned("blob wants %d bytes, %d remain" % (length, self.remaining() // 8))
        at = self.read // 8
        self.read += length * 8
        return self.data[at : at + length]


def decode_field(stream, field):
    kind = field["kind"]
    if kind == "bits":
        return stream.bits(int(field["width"]))
    if kind == "bool1":
        return stream.bool1()
    if kind == "int_range":
        return stream.int_range(int(field["low"]), int(field["high"]))
    if kind == "varuint":
        return stream.varuint(int(field["max_bytes"]))
    if kind == "svarint":
        return stream.svarint(int(field["max_bytes"]))
    if kind == "bytes_capped":
        return stream.bytes_capped(int(field["cap"]))
    if kind == "string":
        return stream.bytes_capped(int(field["cap"])).decode("utf-8")
    raise Poisoned("unknown field kind: %s" % kind)


def decode_into(fields, stream):
    """Decode one record from a live stream, leaving whatever follows it."""
    out = {}
    for field in fields:
        out[field["name"]] = decode_field(stream, field)
    return out


def decode(fields, data, exhaustive=True):
    """Decode one record. Residue is a failure."""
    stream = ReadStream(data)
    out = decode_into(fields, stream)
    if exhaustive:
        stream.align_verify()
        if stream.remaining():
            raise Poisoned("%d bits of residue" % stream.remaining())
    return out


HAND_BUILT = {
    "name": "the v9 reference frame",
    "fields": [
        {"name": "flags", "kind": "bool1"},
        {"name": "pos_x", "kind": "int_range", "low": 0, "high": 2047},
        {"name": "pos_y", "kind": "int_range", "low": 0, "high": 2047},
    ],
    "bytes": bytes([0x01, 0x08, 0x20]),
    "values": {"flags": True, "pos_x": 1024, "pos_y": 512},
}


MASKED_ROW = {
    "name": "the v10 masked row over a three column I16 plan",
    "fields": [
        {"name": "life", "kind": "bits", "width": 4},
        {"name": "mask", "kind": "bits", "width": 3},
        {"name": "has_ack", "kind": "bool1"},
        {"name": "has_tick", "kind": "bool1"},
        {"name": "ack_delta", "kind": "svarint", "max_bytes": 5},
        {"name": "tick_delta", "kind": "svarint", "max_bytes": 3},
        {"name": "x", "kind": "bits", "width": 16},
        {"name": "y", "kind": "bits", "width": 16},
        {"name": "z", "kind": "bits", "width": 16},
    ],
    "bytes": bytes([0xF5, 0x01, 0x05, 0x02, 0x0B, 0x00, 0x16, 0x00, 0x21, 0x00]),
    "values": {
        "life": 5,
        "mask": 7,
        "has_ack": True,
        "has_tick": True,
        "ack_delta": -3,
        "tick_delta": 1,
        "x": 11,
        "y": 22,
        "z": 33,
    },
}


LADDER_ROW = {
    "name": "the v10 laddered row over a two column I16 plan",
    "fields": [
        {"name": "life", "kind": "bits", "width": 4},
        {"name": "mask", "kind": "bits", "width": 2},
        {"name": "has_ack", "kind": "bool1"},
        {"name": "has_tick", "kind": "bool1"},
        {"name": "has_base", "kind": "bool1"},
        {"name": "base_low", "kind": "bits", "width": 8},
        {"name": "sel_x", "kind": "bits", "width": 2},
        {"name": "zz_x", "kind": "bits", "width": 4},
        {"name": "sel_y", "kind": "bits", "width": 2},
        {"name": "zz_y", "kind": "bits", "width": 4},
    ],
    "bytes": bytes([0x35, 0x55, 0xB2, 0x12]),
    "values": {
        "life": 5,
        "mask": 3,
        "has_ack": False,
        "has_tick": False,
        "has_base": True,
        "base_low": 42,
        "sel_x": 1,
        "zz_x": 6,
        "sel_y": 1,
        "zz_y": 9,
    },
}


SNAPSHOT_HEAD = [
    {"name": "token", "kind": "varuint", "max_bytes": 10},
    {"name": "revision", "kind": "varuint", "max_bytes": 10},
    {"name": "distance", "kind": "bits", "width": 6},
    {"name": "has_ack", "kind": "bool1"},
    {"name": "has_tick", "kind": "bool1"},
]


SNAPSHOT_FULL = {
    "name": "WIRE.md 8 full row over a three column I16 plan",
    "fields": SNAPSHOT_HEAD
    + [
        {"name": "x", "kind": "bits", "width": 16},
        {"name": "y", "kind": "bits", "width": 16},
        {"name": "z", "kind": "bits", "width": 16},
    ],
    "bytes": bytes([0x07, 0x01, 0x00, 0x0B, 0x00, 0x16, 0x00, 0x21, 0x00]),
    "values": {
        "token": 7,
        "revision": 1,
        "distance": 0,
        "has_ack": False,
        "has_tick": False,
        "x": 11,
        "y": 22,
        "z": 33,
    },
}


SNAPSHOT_DELTA = {
    "name": "WIRE.md 8 delta at distance one, mask 0b101",
    "fields": SNAPSHOT_HEAD
    + [
        {"name": "mask", "kind": "bits", "width": 3},
        {"name": "tick_delta", "kind": "svarint", "max_bytes": 3},
        {"name": "x", "kind": "bits", "width": 16},
        {"name": "z", "kind": "bits", "width": 16},
    ],
    "bytes": bytes([0x07, 0x04, 0x81, 0x05, 0x02, 0x0B, 0x00, 0x21, 0x00]),
    "values": {
        "token": 7,
        "revision": 4,
        "distance": 1,
        "has_ack": False,
        "has_tick": True,
        "mask": 5,
        "tick_delta": 1,
        "x": 11,
        "z": 33,
    },
}


SNAPSHOT_ZERO_MASK = {
    "name": "WIRE.md 8 zero mask delta at distance two",
    "fields": SNAPSHOT_HEAD + [{"name": "mask", "kind": "bits", "width": 3}],
    "bytes": bytes([0x07, 0x03, 0x02, 0x00]),
    "values": {
        "token": 7,
        "revision": 3,
        "distance": 2,
        "has_ack": False,
        "has_tick": False,
        "mask": 0,
    },
}


SNAPSHOT_WINDOW = {
    "name": "WIRE.md 9 window of two samples over one I16 column",
    "fields": [
        {"name": "token", "kind": "varuint", "max_bytes": 10},
        {"name": "revision", "kind": "varuint", "max_bytes": 10},
        {"name": "count", "kind": "int_range", "low": 1, "high": 255},
        {"name": "has_ack", "kind": "bool1"},
        {"name": "tick_delta", "kind": "svarint", "max_bytes": 3},
        {"name": "age_older", "kind": "varuint", "max_bytes": 3},
        {"name": "older", "kind": "bits", "width": 16},
        {"name": "age_newer", "kind": "varuint", "max_bytes": 3},
        {"name": "newer", "kind": "bits", "width": 16},
    ],
    "bytes": bytes([0x07, 0x02, 0x01, 0x00, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x16, 0x00]),
    "values": {
        "token": 7,
        "revision": 2,
        "count": 2,
        "has_ack": False,
        "tick_delta": 0,
        "age_older": 1,
        "older": 11,
        "age_newer": 0,
        "newer": 22,
    },
}


CONTROL_OPEN = {
    "name": "WIRE.md 10 OPEN for route 300 ordinal 2",
    "fields": [
        {"name": "tag", "kind": "bits", "width": 8},
        {"name": "request_step", "kind": "svarint", "max_bytes": 10},
        {"name": "route", "kind": "varuint", "max_bytes": 5},
        {"name": "family", "kind": "bits", "width": 2},
        {"name": "has_ordinal", "kind": "bool1"},
        {"name": "has_epoch", "kind": "bool1"},
        {"name": "has_tenure", "kind": "bool1"},
        {"name": "has_anchor", "kind": "bool1"},
        {"name": "has_anchor_author", "kind": "bool1"},
        {"name": "has_schema", "kind": "bool1"},
        {"name": "ordinal", "kind": "bits", "width": 8},
        {"name": "epoch", "kind": "varuint", "max_bytes": 3},
        {"name": "tenure", "kind": "varuint", "max_bytes": 5},
        {"name": "anchor", "kind": "varuint", "max_bytes": 5},
        {"name": "anchor_author", "kind": "varuint", "max_bytes": 5},
        {"name": "schema", "kind": "bits", "width": 32},
    ],
    "bytes": bytes(
        [
            0x00,
            0x02,
            0xAC,
            0x02,
            0xFC,
            0x02,
            0x05,
            0x03,
            0x04,
            0x06,
            0xEF,
            0xBE,
            0xAD,
            0xDE,
        ]
    ),
    "values": {
        "tag": 0,
        "request_step": 1,
        "route": 300,
        "family": 0,
        "has_ordinal": True,
        "has_epoch": True,
        "has_tenure": True,
        "has_anchor": True,
        "has_anchor_author": True,
        "has_schema": True,
        "ordinal": 2,
        "epoch": 5,
        "tenure": 3,
        "anchor": 4,
        "anchor_author": 6,
        "schema": 0xDEADBEEF,
    },
}


CONTROL_READY = {
    "name": "WIRE.md 10 READY for request 300",
    "fields": [
        {"name": "tag", "kind": "bits", "width": 8},
        {"name": "request_step", "kind": "svarint", "max_bytes": 10},
        {"name": "token_step", "kind": "svarint", "max_bytes": 10},
    ],
    "bytes": bytes([0x01, 0xD8, 0x04, 0x0E]),
    "values": {"tag": 1, "request_step": 300, "token_step": 7},
}


CONTROL_ACCEPT = {
    "name": "WIRE.md 10 ACCEPT of two entries",
    "fields": [
        {"name": "tag", "kind": "bits", "width": 8},
        {"name": "count", "kind": "int_range", "low": 1, "high": 32},
        {"name": "token_step_a", "kind": "svarint", "max_bytes": 10},
        {"name": "revision_a", "kind": "varuint", "max_bytes": 10},
        {"name": "token_step_b", "kind": "svarint", "max_bytes": 10},
        {"name": "revision_b", "kind": "varuint", "max_bytes": 10},
    ],
    "bytes": bytes([0x02, 0x01, 0x0E, 0x03, 0x04, 0x01]),
    "values": {
        "tag": 2,
        "count": 2,
        "token_step_a": 7,
        "revision_a": 3,
        "token_step_b": 2,
        "revision_b": 1,
    },
}


V11_HAND_BUILT = [
    SNAPSHOT_FULL,
    SNAPSHOT_DELTA,
    SNAPSHOT_ZERO_MASK,
    SNAPSHOT_WINDOW,
    CONTROL_OPEN,
    CONTROL_READY,
    CONTROL_ACCEPT,
]


def unzigzag(coded):
    return (coded >> 1) ^ -(coded & 1)


AGAINST_SPEC = [
    {
        "record": "DatagramReliable",
        "bytes": bytes([0x57, 0x01]),
        "values": {"magic": 0x57, "base_tick": 1},
        "why": "WIRE.md 2, the reliable magic then tick zero as base_tick one",
    },
    {
        "record": "DatagramUnreliable",
        "bytes": bytes([0x77, 0x09, 0x00, 0x00]),
        "values": {"magic": 0x77, "seq": 9, "base_tick": 0},
        "why": "WIRE.md 2, seq 9 little endian, then an unconfigured clock as 0",
    },
    {
        "record": "DatagramAcked",
        "bytes": bytes([0x97, 0x09, 0x00, 0x0C, 0x00, 0xA5, 0xA5, 0xA5, 0xA5, 0xAD, 0x02]),
        "values": {"magic": 0x97, "seq": 9, "ack": 12, "history": 0xA5A5A5A5, "base_tick": 301},
        "why": "WIRE.md 2, the echo of seq 12 with its 32 bit history, at tick 300",
    },
    {
        "record": "AckHeader",
        "bytes": bytes([0x03, 0xAC, 0x02, 0x02]),
        "values": {"epoch": 3, "base": 300, "count": 2},
        "why": "u8 epoch, varuint base, u8 count",
    },
    {
        "record": "CommandHeader",
        "bytes": bytes([0x15, 0x07, 0x05, 0x02]),
        "values": {"acked": 21, "epoch": 7, "base": 5, "count": 2},
        "why": "varuint acked is ack_of_acks 4 plus one, times four, with "
               "asks_detail set and no hold, then u8, varuint, u8",
    },
    {
        "record": "FrameHeader",
        "bytes": bytes([0xAC, 0x02, 0x03, 0x13, 0x00]),
        "values": {"route": 300, "comp": 3, "channel": 19, "length": 0},
        "why": "WIRE.md 3, route varuint 5, comp u8, channel u8, len varuint 3",
    },
    {
        "record": "SpawnVerbHead",
        "bytes": bytes([0xAC, 0x02, 0x02]),
        "values": {"route": 300, "epoch": 2},
        "why": "WIRE.md 11, route varuint 5 then epoch varuint 3",
    },
    {
        "record": "ClockRate",
        "bytes": bytes([0x3C]),
        "values": {"tickrate": 60},
        "why": "WIRE.md 12, a sixty hertz rate in one varuint group",
    },
    {
        "record": "ClockPing",
        "bytes": bytes([0x78, 0x56, 0x34, 0x12]),
        "values": {"origin": 0x12345678},
        "why": "WIRE.md 12, the low 32 bits of the pinger's own wall clock",
    },
    {
        "record": "ClockPong",
        "bytes": bytes([0x78, 0x56, 0x34, 0x12, 0x2A, 0x00, 0x00, 0x00, 0x80, 0xA0, 0x00]),
        "values": {"origin": 0x12345678, "tick": 42, "phase": 128, "wait": 160},
        "why": "WIRE.md 12, the ping's origin echoed, tick 42 at half phase, held 16 ms",
    },
    {
        "record": "ActionRequest",
        "bytes": bytes([0x04, 0x00]) + b"fire" + bytes([0x12, 0x00, 0x00]),
        "values": {"method": "fire", "view_tick": 9, "data": b""},
        "why": "WIRE.md 18, a remote action request at view tick 9",
    },
    {
        "record": "SessionReason",
        "bytes": bytes([0x04, 0x00]) + b"rude",
        "values": {"reason": "rude"},
        "why": "WIRE.md 16, a reason a game wrote and a player reads",
    },
    {
        "record": "KickRequest",
        "bytes": bytes([0x09, 0x03, 0x00]) + b"afk",
        "values": {"player": 9, "reason": "afk"},
        "why": "WIRE.md 16, player 9, then the reason",
    },
    {
        "record": "SessionLeave",
        "bytes": bytes([0x01]),
        "values": {"returning": True},
        "why": "WIRE.md 16, a client stepping away, padded to the byte",
    },
    {
        "record": "PlayerLeft",
        "bytes": bytes([0x09, 0x01]),
        "values": {"player_id": 9, "reason": 1},
        "why": "WIRE.md 16, player 9 left stepping away",
    },
    {
        "record": "ResyncRoute",
        "bytes": bytes([0x07, 0x00, 0x00, 0x00, 0x01, 0x01]),
        "values": {
            "route_delta": 7,
            "anchor": 0,
            "anchor_author": 0,
            "control_revision": 0,
            "epoch": 1,
            "gone": True,
        },
        "why": "WIRE.md 16, the former server reports route 7 gone",
    },
    {
        "record": "SceneReleased",
        "bytes": bytes([0x07]),
        "values": {"route": 7},
        "why": "WIRE.md 16, the route of the scene the server released",
    },
    {
        "record": "SpawnFailed",
        "bytes": bytes([0x07, 0x10, 0x00]) + b"returned no Node",
        "values": {"route": 7, "reason": "returned no Node"},
        "why": "WIRE.md 16, a client that could not build route 7",
    },
    {
        "record": "SceneViewersHead",
        "bytes": bytes([0x07, 0x01, 0x02, 0x03, 0x02]),
        "values": {"route": 7, "epoch": 1, "generation": 2, "revision": 3, "count": 2},
        "why": "WIRE.md 16, revision 3 of scene 7's roster, two viewers following",
    },
    {
        "record": "SceneViewerRow",
        "bytes": bytes([0x09]),
        "values": {"player_id": 9},
        "why": "WIRE.md 16, one viewer named by the membership the accept issued",
    },
    {
        "record": "AcceptFrame",
        "bytes": (
            bytes([0x0E, 0x03, 0x00])
            + b"ana"
            + bytes([0x01])
            + bytes([0x06, 0x00])
            + b"steam:"
            + bytes([0x00, 0x00])
            + bytes([0x11, 0x00])
            + b"203.0.113.4:21253"
            + bytes([0x08])
        ),
        "values": {
            "peer_id": 7,
            "username": "ana",
            "player_id": 1,
            "account": "steam:",
            "account_name": "",
            "join_address": "203.0.113.4:21253",
            "max_players": 8,
        },
        "why": "WIRE.md 16, the accept peer 7 itself receives, as membership 1 "
        "under an account and no display name, with the address the server "
        "advertises and its limit of 8 players",
    },
    {
        "record": "JoinFrame",
        "bytes": (
            bytes([0x03, 0x00]) + b"ana" + bytes([0x00, 0x00, 0x00, 0x00, 0x00]) + bytes(8) + bytes([0x08]) + bytes(16)
        ),
        "values": {
            "username": "ana",
            "args": b"",
            "arg_types": b"",
            "proof": b"",
            "schema_hash": 0,
            "peer_id": 4,
            "app_tag": 0,
            "wire_identity": 0,
        },
        "why": "WIRE.md 16, a join whose two identity halves are both zero",
    },
    {
        "record": "TableFrameHead",
        "bytes": bytes([0x03, 0xEF, 0xBE, 0xAC, 0x02, 0x01, 0x02]),
        "values": {"table_id": 3, "schema_hash": 0xBEEF, "tick": 300, "flags": 1, "rows": 2},
        "why": "WIRE.md 17, a snapshot of two rows of table 3 at tick 300",
    },
    {
        "record": "AwarenessEdge",
        "bytes": bytes([0x07, 0x04, 0x00]) + b"zone" + bytes([0x01]),
        "values": {
            "route": 7,
            "layer_id": "zone",
            "entered": True,
        },
        "why": "WIRE.md 15, the receiving peer starts seeing route 7 inside zone",
    },
    {
        "record": "ControlRequest",
        "bytes": bytes([0x01, 0x03, 0xAC, 0x02, 0x00, 0x00, 0x02, 0x00]),
        "values": {
            "op": 1,
            "observed_revision": 3,
            "issued_tick": 300,
            "source_route": 0,
            "successor": 0,
            "kind": 0,
            "hold": 1,
            "final_state": b"",
        },
        "why": "WIRE.md 14, op 1 asking for a yieldable hold at tick 300",
    },
    {
        "record": "ControlApply",
        "bytes": bytes(
            [0x02, 0x03, 0x00, 0x25, 0x00, 0x00, 0x09, 0x04, 0x00, 0x00, 0x00]
        ),
        "values": {
            "controller": 2,
            "revision": 3,
            "op": 0,
            "tenure_changed": True,
            "hold": 2,
            "outcome": 0,
            "has_owner": True,
            "kept": False,
            "final_state": b"",
            "peer": 9,
            "player_id": 4,
            "code": 0,
            "reason": "",
        },
        "why": "WIRE.md 14, revision 3 opens peer 2's tenure with an exclusive "
        "hold and hands the entity to peer 9 as player 4, with no refusal",
    },
    {
        "record": "DenyKey",
        "bytes": bytes([0x04, 0x00]) + b"shot" + bytes([0x2C, 0x02, 0x00]) + b"no",
        "values": {"key": "shot", "code": 44, "reason": "no"},
        "why": "WIRE.md 13, a denial with ERR_BUSY and a two byte reason",
    },
    {
        "record": "ConfirmKey",
        "bytes": bytes([0x04, 0x00]) + b"shot",
        "values": {"key": "shot"},
        "why": "WIRE.md 13, the same layout as a denial",
    },
    {
        "record": "RouteLease",
        "bytes": bytes([0x11, 0x10]),
        "values": {"base": 17, "count": 16},
        "why": "WIRE.md 16, the sixteen routes from 17 a joiner mints from",
    },
    {
        "record": "RowControlOpenHead",
        "bytes": bytes([0x00, 0x02, 0xAC, 0x02, 0x00]),
        "values": {
            "tag": 0,
            "request_step": 1,
            "route": 300,
            "family": 0,
            "has_ordinal": False,
            "has_epoch": False,
            "has_tenure": False,
            "has_anchor": False,
            "has_anchor_author": False,
            "has_schema": False,
        },
        "why": "WIRE.md 10, the head of an OPEN whose fields all match its basis",
    },
    {
        "record": "RowControlReady",
        "bytes": CONTROL_READY["bytes"],
        "values": CONTROL_READY["values"],
        "why": "WIRE.md 10, the receiver answering request 300 with token 7",
    },
    {
        "record": "RowControlReset",
        "bytes": bytes([0x03, 0xD8, 0x04, 0x07]),
        "values": {"tag": 3, "request_step": 300, "token": 7},
        "why": "WIRE.md 10, a RESET stepping its request and naming its token",
    },
    {
        "record": "RowControlClose",
        "bytes": bytes([0x04, 0x0E]),
        "values": {"tag": 4, "token_step": 7},
        "why": "WIRE.md 10, a writer closing token 7 as the frame's first",
    },
    {
        "record": "RowControlAcceptHead",
        "bytes": bytes([0x02, 0x01]),
        "values": {"tag": 2, "count": 2},
        "why": "WIRE.md 10, two entries written as 1 in the range 1..32",
    },
    {
        "record": "RowControlAcceptStep",
        "bytes": bytes([0x0E, 0x03]),
        "values": {"token_step": 7, "revision": 3},
        "why": "WIRE.md 10, token 7 accepted at revision 3",
    },
    {
        "record": "CaptureRecord",
        "bytes": bytes([0x01, 0x02, 0xAC, 0x02, 0x2B, 0x02, 0x00, 0x57, 0x01]),
        "values": {
            "dir": 1,
            "peer": 2,
            "wall_ms": 300,
            "tick": 43,
            "bytes": bytes([0x57, 0x01]),
        },
        "why": "WIRE.md 21, a datagram peer 2 read 300ms in, at session tick 42",
    },
]


# A capture names its own format, and a format owns its datagram magics.
# Every other byte of the carrier is described by the header's own record sheet,
# so this table and REPLY_FAILED_BIT_SINCE are the two things a format bump
# moves that the sheet cannot say.
# A capture whose format is absent here is refused rather than guessed at.
# Reading a magic against the wrong table names a record of the wrong shape and
# then decodes plausible garbage out of every frame behind it.
CAPTURE_MAGICS_BY_FORMAT = {
    10: {
        0x57: "DatagramReliable",
        0x77: "DatagramUnreliable",
        0x97: "DatagramAcked",
    },
    11: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    12: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    13: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    14: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    15: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    16: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    17: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
    },
    18: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    19: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    20: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    21: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    22: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    23: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
    24: {
        0x58: "DatagramReliable",
        0x78: "DatagramUnreliable",
        0x98: "DatagramAcked",
        0xB8: "DatagramAckedHeld",
    },
}

REPLY_FAILED_BIT_SINCE = 12
STRING_CAP = 1023


VALUE_SINCE = 24
VALUE_CAP = 65535
MAX_VALUE_DEPTH = 16
MAX_VALUE_ELEMENTS = 65536
VALUE_NAMES = (
    "NIL FALSE TRUE INT FLOAT FLOAT32 STRING STRING_NAME NODE_PATH VECTOR2 "
    "VECTOR2I VECTOR3 VECTOR3I VECTOR4 VECTOR4I QUATERNION COLOR RECT2 RECT2I "
    "PLANE AABB BASIS TRANSFORM2D TRANSFORM3D PROJECTION ARRAY TYPED_ARRAY "
    "DICTIONARY TYPED_DICTIONARY BYTES PACKED_INT32 PACKED_INT64 PACKED_FLOAT32 "
    "PACKED_FLOAT64 PACKED_STRING PACKED_VECTOR2 PACKED_VECTOR3 PACKED_COLOR "
    "PACKED_VECTOR4"
).split()
VALUE_REALS = {9: 2, 11: 3, 13: 4, 15: 4, 16: 4, 17: 4, 19: 4, 20: 6, 21: 9, 22: 6, 23: 12, 24: 16}
VALUE_INTS = {10: 2, 12: 3, 14: 4, 18: 4}
VALUE_PACKED = {30: "<i", 31: "<q", 32: "<f", 33: "<d", 35: "<2f", 36: "<3f", 37: "<4f", 38: "<4f"}
VALUE_KIND = {2: 1, 5: 4, 26: 25, 28: 27}


class ValueReader:
    """One value of WIRE.md 23, refused on every spelling the writer never makes."""

    def __init__(self, data, at=0, end=None):
        self.data = bytes(data)
        self.at = at
        self.end = len(self.data) if end is None else end
        self.budget = MAX_VALUE_ELEMENTS

    def span(self, size):
        if size < 0 or self.at + size > self.end:
            raise Poisoned("a value reads past its end")
        start = self.at
        self.at += size
        return self.data[start : self.at]

    def varuint(self):
        staged = 0
        for group in range(MAX_VARINT_BYTES):
            byte = self.span(1)[0]
            if group == MAX_VARINT_BYTES - 1 and (byte & 0x7F) > 1:
                raise Poisoned("a value varuint is past 64 bits")
            staged |= (byte & 0x7F) << (group * 7)
            if not byte & 0x80:
                if group > 0 and (byte & 0x7F) == 0:
                    raise Poisoned("a value varuint is not canonical")
                return staged
        raise Poisoned("a value varuint is past 64 bits")

    def svarint(self):
        staged = self.varuint()
        return (staged >> 1) ^ -(staged & 1)

    def tag(self):
        byte = self.span(1)[0]
        if byte >= len(VALUE_NAMES):
            raise Poisoned("value tag %d is past the last" % byte)
        return byte

    def kind(self, tag):
        return VALUE_KIND.get(tag, tag)

    def header(self):
        tag = self.tag()
        if self.kind(tag) != tag:
            raise Poisoned("a typed header names %s, which is no kind" % VALUE_NAMES[tag])
        return tag

    def text(self):
        raw = self.span(self.varuint())
        if b"\x00" in raw or raw.startswith(b"\xef\xbb\xbf"):
            raise Poisoned("value text holds a NUL or a byte order mark")
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raise Poisoned("value text is not UTF-8")

    def count(self, least):
        count = self.varuint()
        if count * least > self.end - self.at:
            raise Poisoned("a value count is larger than its bytes")
        return count

    def value(self, depth=0, want=0):
        if depth > MAX_VALUE_DEPTH:
            raise Poisoned("a value nests past depth %d" % MAX_VALUE_DEPTH)
        self.budget -= 1
        if self.budget < 0:
            raise Poisoned("a value holds more than %d values" % MAX_VALUE_ELEMENTS)
        tag = self.tag()
        if want and self.kind(tag) != want:
            raise Poisoned("a typed element is %s" % VALUE_NAMES[tag])
        if tag <= 2:
            return (None, False, True)[tag]
        if tag == 3:
            return self.svarint()
        if tag == 4:
            held = struct.unpack("<d", self.span(8))[0]
            try:
                narrowed = struct.pack("<d", struct.unpack("<f", struct.pack("<f", held))[0])
            except OverflowError:
                narrowed = None
            if narrowed == struct.pack("<d", held):
                raise Poisoned("a FLOAT that a FLOAT32 holds exactly")
            return held
        if tag == 5:
            return struct.unpack("<f", self.span(4))[0]
        if tag in (6, 7):
            return self.text()
        if tag == 8:
            path = self.text()
            names, _, subnames = path.partition(":")
            names = names[1:] if names.startswith("/") else names
            parts = (names.split("/") if names else []) + (subnames.split(":") if ":" in path else [])
            if any(part == "" for part in parts):
                raise Poisoned("a NODE_PATH Godot would read as another path")
            return path
        if tag in VALUE_REALS:
            return (VALUE_NAMES[tag], struct.unpack("<%df" % VALUE_REALS[tag], self.span(4 * VALUE_REALS[tag])))
        if tag in VALUE_INTS:
            parts = tuple(self.svarint() for _ in range(VALUE_INTS[tag]))
            if any(part < -(1 << 31) or part >= 1 << 31 for part in parts):
                raise Poisoned("a %s component is past 32 bits" % VALUE_NAMES[tag])
            return (VALUE_NAMES[tag], parts)
        if tag in (25, 26):
            element = self.header() if tag == 26 else 0
            if tag == 26 and element == 0:
                raise Poisoned("a TYPED_ARRAY of NIL")
            return [self.value(depth + 1, element) for _ in range(self.count(1))]
        if tag in (27, 28):
            key_kind = self.header() if tag == 28 else 0
            value_kind = self.header() if tag == 28 else 0
            if tag == 28 and key_kind == 0 and value_kind == 0:
                raise Poisoned("a TYPED_DICTIONARY of no type")
            pairs = []
            keys = set()
            for _ in range(self.count(2)):
                start = self.at
                key = self.value(depth + 1, key_kind)
                if self.data[start : self.at] in keys:
                    raise Poisoned("a DICTIONARY holds one key twice")
                keys.add(self.data[start : self.at])
                pairs.append((key, self.value(depth + 1, value_kind)))
            return pairs
        if tag == 29:
            return self.span(self.varuint())
        if tag == 34:
            return [self.text() for _ in range(self.count(1))]
        layout = VALUE_PACKED[tag]
        width = struct.calcsize(layout)
        count = self.count(width)
        return [struct.unpack(layout, self.span(width)) for _ in range(count)]


def read_value(data, at=0, end=None):
    """One value and the offset just past it."""
    reader = ValueReader(data, at, end)
    held = reader.value()
    return held, reader.at


def read_cell(cell, text):
    """One sized column body of WIRE.md 8, a STRING or a VARIANT cell."""
    if text:
        if b"\x00" in cell or cell.startswith(b"\xef\xbb\xbf"):
            raise Poisoned("cell text holds a NUL or a byte order mark")
        try:
            return bytes(cell).decode("utf-8")
        except UnicodeDecodeError:
            raise Poisoned("cell text is not UTF-8")
    held, end = read_value(cell)
    if end != len(cell):
        raise Poisoned("a VARIANT cell carries %d bytes past its value" % (len(cell) - end))
    return held


def read_slots(stream):
    """A section 18 slot list. A quantized slot needs its declaration."""
    slots = []
    for _ in range(stream.varuint(2)):
        kind = stream.int_range(0, 2)
        if kind == 1:
            raise Poisoned("a quantized slot reads only against its declaration")
        if kind == 2:
            route = stream.varuint(5)
            comp = stream.bits(8)
            path = stream.bytes_capped(STRING_CAP).decode("utf-8") if comp == 255 else None
            slots.append(("NODE_REF", route, comp, path))
            continue
        stream.align_verify()
        at = stream.read // 8
        held, end = read_value(stream.data, at, min(len(stream.data), at + VALUE_CAP))
        stream.read = end * 8
        slots.append(held)
    return slots


def read_token(stream):
    if stream.bool1():
        return stream.bits(8)
    return stream.bytes_capped(STRING_CAP).decode("utf-8")


def read_call(payload):
    """A CALL, SIGNAL or PROPERTY_SYNC payload of WIRE.md 18, with its values."""
    stream = ReadStream(payload)
    flags = stream.bits(8)
    out = {"flags": flags}
    if flags & 4:
        out["txn"] = stream.varuint(5)
    if flags & 3 == 2:
        out["target"] = stream.bits(32)
    out["token"] = read_token(stream)
    out["slots"] = read_slots(stream)
    stream.align_verify()
    if stream.remaining():
        raise Poisoned("a call carries %d bits past its slots" % stream.remaining())
    return out


def read_sync(payload, delta):
    """A SYNC or SYNC_DELTA payload of WIRE.md 19, with its values."""
    stream = ReadStream(payload)
    out = {"ordinal": stream.varuint(5)}
    out["mask" if delta else "flags"] = stream.varuint(10) if delta else stream.bits(8)
    out["slots"] = read_slots(stream)
    stream.align_verify()
    if stream.remaining():
        raise Poisoned("a sync carries %d bits past its slots" % stream.remaining())
    return out


def read_member(payload):
    stream = ReadStream(payload)
    out = {"token": read_token(stream), "slots": read_slots(stream)}
    stream.align_verify()
    if stream.remaining():
        raise Poisoned("a frame carries %d bits past its slots" % stream.remaining())
    return out


def read_reply(payload, fmt):
    """One REPLY payload, from WIRE.md 18.

    From format 12 a reply carries a failed bit. A failed reply carries an
    Error code and the receiver's reason. From format 24 a successful reply's
    value slots decode, and before it they are counted.
    """
    stream = ReadStream(payload)
    out = {"txn": stream.varuint(5)}
    if int(fmt) < REPLY_FAILED_BIT_SINCE or not stream.bool1():
        out["failed"] = False
        if int(fmt) >= VALUE_SINCE:
            out["slots"] = read_slots(stream)
            stream.align_verify()
        out["value_bits"] = stream.remaining()
        return out
    out["failed"] = True
    out["code"] = stream.varuint(2)
    out["detail"] = stream.bytes_capped(STRING_CAP).decode("utf-8")
    stream.align_verify()
    if stream.remaining():
        raise Poisoned("a failed reply carries %d bits past its detail" % stream.remaining())
    return out


def datagram_magics(fmt):
    """The magic table one capture format speaks, or a refusal."""
    try:
        key = int(fmt)
    except (TypeError, ValueError):
        raise Poisoned("the capture names no format version")
    if key not in CAPTURE_MAGICS_BY_FORMAT:
        raise Poisoned(
            "format %s is no capture format this decoder speaks, and it speaks %s"
            % (key, ", ".join(str(k) for k in sorted(CAPTURE_MAGICS_BY_FORMAT)))
        )
    return CAPTURE_MAGICS_BY_FORMAT[key]


def read_capture(path):
    """Split a .netwcap into its header and record list. Reject trailing data."""
    with open(path, "rb") as handle:
        raw = handle.read()
    split = raw.find(b"\n")
    if split < 0:
        raise Poisoned("the capture carries no header line")
    header = json.loads(raw[:split].decode("utf-8"))
    records = header.get("records", {})
    if "CaptureRecord" not in records:
        raise Poisoned("the capture header describes no CaptureRecord")
    stream = ReadStream(raw[split + 1 :])
    rows = []
    while stream.remaining() >= 8:
        rows.append(decode_into(records["CaptureRecord"], stream))
    if stream.remaining():
        raise Poisoned("%d bits of residue past the last record" % stream.remaining())
    return header, rows


def walk_datagram(records, channels, payload, magics):
    """Every frame in one captured datagram, or the refusal that stopped it."""
    stream = ReadStream(payload)
    magic = payload[0] if payload else -1
    if magic not in magics:
        raise Poisoned("0x%02x is no datagram magic this format speaks" % magic)
    head = decode_into(records[magics[magic]], stream)
    stream.align_verify()
    frames = []
    while stream.remaining() >= 8:
        frame = decode_into(records["FrameHeader"], stream)
        length = frame["length"]
        if stream.remaining() < length * 8:
            raise Poisoned(
                "route %d channel %d claims %d bytes, %d remain"
                % (frame["route"], frame["channel"], length, stream.remaining() // 8)
            )
        at = stream.read // 8
        stream.read += length * 8
        frame["payload"] = stream.data[at : at + length]
        frame["channel_name"] = channels.get(frame["channel"], "?")
        frames.append(frame)
    return head, frames


def capture(path, verbose):
    """Decode a capture end to end, and answer non-zero on any refusal."""
    header, rows = read_capture(path)
    records = header.get("records", {})
    channels = {int(row["id"]): row["name"] for row in header.get("channels", [])}
    magics = datagram_magics(header.get("format"))
    print(
        "CAPTURE %s format=%s identity=%s peer=%s records=%d schemas=%d"
        % (
            path,
            header.get("format"),
            header.get("identity"),
            header.get("peer"),
            len(rows),
            len(header.get("schemas", [])),
        )
    )

    errors = 0
    frames = 0
    per_channel = {0: {}, 1: {}}
    per_direction = {0: 0, 1: 0}
    datagram_bytes = {0: 0, 1: 0}
    raw = 0
    for index, row in enumerate(rows):
        direction = row["dir"]
        per_direction[direction] = per_direction.get(direction, 0) + 1
        datagram_bytes[direction] = datagram_bytes.get(direction, 0) + len(row["bytes"])
        if row["bytes"] and row["bytes"][0] not in magics:
            raw += 1
            if verbose:
                print("  raw record %d: bytes=%d" % (index, len(row["bytes"])))
            continue
        try:
            head, walked = walk_datagram(records, channels, row["bytes"], magics)
        except Poisoned as stopped:
            errors += 1
            print("ERROR record %d: %s" % (index, stopped))
            continue
        frames += len(walked)
        lane = per_channel.setdefault(direction, {})
        for frame in walked:
            key = "%d %s" % (frame["channel"], frame["channel_name"])
            lane[key] = lane.get(key, 0) + len(frame["payload"])
            name = frame["channel_name"]
            valued = int(header.get("format")) >= VALUE_SINCE
            readers = {"REPLY": lambda p: read_reply(p, header.get("format"))}
            if valued:
                readers.update(
                    {
                        "CALL": read_call,
                        "SIGNAL": read_member,
                        "PROPERTY_SYNC": read_member,
                        "SYNC": lambda p: read_sync(p, False),
                        "SYNC_DELTA": lambda p: read_sync(p, True),
                    }
                )
            if name not in readers:
                continue
            try:
                frame["reply"] = readers[name](frame["payload"])
            except Poisoned as stopped:
                errors += 1
                print("ERROR record %d: a %s %s" % (index, name, stopped))
        if verbose:
            print(
                "  %s peer=%d wall=%dms tick=%s bytes=%d frames=%d"
                % (
                    "out" if row["dir"] == 0 else "in ",
                    row["peer"],
                    row["wall_ms"],
                    row["tick"] - 1 if row["tick"] else "none",
                    len(row["bytes"]),
                    len(walked),
                )
            )
            for frame in walked:
                print(
                    "      route=%d comp=%d channel=%d %s payload=%d"
                    % (
                        frame["route"],
                        frame["comp"],
                        frame["channel"],
                        frame["channel_name"],
                        len(frame["payload"]),
                    )
                )
                if "reply" in frame:
                    print("        %s" % frame["reply"])

    for direction, label in ((0, "OUT"), (1, "IN ")):
        lane = per_channel.get(direction, {})
        payload = sum(lane.values())
        print(
            "  %s datagrams=%d datagram_bytes=%d frame_payload=%d residual=%d"
            % (
                label,
                per_direction.get(direction, 0),
                datagram_bytes.get(direction, 0),
                payload,
                datagram_bytes.get(direction, 0) - payload,
            )
        )
        for key in sorted(lane, key=lambda k: -lane[k]):
            print("      %-28s %9d" % (key, lane[key]))
    print("CAPTURE %d records %d frames %d errors" % (len(rows), frames, errors))
    if raw:
        print("CAPTURE %d raw records that are not netwkd datagrams" % raw)
    return 1 if errors else 0


def self_test(spec_path=None):
    """Proves the decoder red before it is trusted green."""
    wrong = 0

    def check(name, got, want):
        nonlocal wrong
        ok = got == want
        wrong += 0 if ok else 1
        print("%s %-52s %s" % ("ok  " if ok else "FAIL", name, "" if ok else "got %r want %r" % (got, want)))

    def rejects(name, thunk):
        nonlocal wrong
        try:
            thunk()
        except Poisoned:
            print("ok   %-52s" % name)
            return
        wrong += 1
        print("FAIL %-52s accepted invalid input" % name)

    check(
        "the 23 Vector2 decodes to its value",
        read_value(bytes.fromhex("090000c03f000000c0")),
        (("VECTOR2", (1.5, -2.0)), 9),
    )
    check(
        "the 23 Array decodes to its value",
        read_value(bytes.fromhex("1902030e06026f6b")),
        ([7, "ok"], 8),
    )
    check("a STRING cell holds UTF-8 with no tag", read_cell(b"caf\xc3\xa9", True), "café")
    for name, hexed in (
        ("a tag past the last", "27"),
        ("an INT in more bytes than it needs", "038100"),
        ("a FLOAT a FLOAT32 holds", "04000000000000e03f"),
        ("text holding a NUL", "060100"),
        ("a TYPED_ARRAY of INT holding a STRING", "2603010600"),
        ("a DICTIONARY holding one key twice", "1b020302000302 01".replace(" ", "")),
        ("18 nested arrays", "1901" * 17 + "1900"),
        ("a NODE_PATH with an empty subname", "0804613a3a62"),
        ("a TYPED_DICTIONARY of NIL and NIL", "1c000000"),
    ):
        rejects("23 refuses %s" % name, lambda h=hexed: read_value(bytes.fromhex(h)))
    check("17 nested arrays are the deepest held", read_value(bytes.fromhex("1901" * 16 + "1900"))[1], 34)
    rejects("a VARIANT cell with residue is refused", lambda: read_cell(bytes.fromhex("0000"), False))

    got = decode(HAND_BUILT["fields"], HAND_BUILT["bytes"])
    check("the 8.3 reference frame decodes to its values", got, HAND_BUILT["values"])

    layout = 0
    for bit in (0, 1 + 10, 12 + 9):
        layout |= 1 << bit
    check(
        "the 8.3 bit layout packs to the 8.3 bytes",
        bytes([(layout >> (8 * i)) & 0xFF for i in range(3)]),
        HAND_BUILT["bytes"],
    )

    check(
        "the 9.4 masked row decodes to its values",
        decode(MASKED_ROW["fields"], MASKED_ROW["bytes"]),
        MASKED_ROW["values"],
    )
    rejects(
        "the 9.4 masked row rejects a trailing byte",
        lambda: decode(MASKED_ROW["fields"], MASKED_ROW["bytes"] + b"\xff"),
    )

    laddered = decode(LADDER_ROW["fields"], LADDER_ROW["bytes"])
    check("the 9.5 laddered row decodes to its values", laddered, LADDER_ROW["values"])
    rejects(
        "the 9.5 laddered row rejects a trailing byte",
        lambda: decode(LADDER_ROW["fields"], LADDER_ROW["bytes"] + b"\xff"),
    )
    check(
        "the 9.5 laddered row steps 1000 and 2000 to the prose's codes",
        (1000 + unzigzag(laddered["zz_x"]), 2000 + unzigzag(laddered["zz_y"])),
        (1003, 1995),
    )

    for frame in V11_HAND_BUILT:
        check(
            "%s decodes to its values" % frame["name"],
            decode(frame["fields"], frame["bytes"]),
            frame["values"],
        )
        rejects(
            "%s rejects a trailing byte" % frame["name"],
            lambda f=frame: decode(f["fields"], f["bytes"] + b"\xff"),
        )
        rejects(
            "%s rejects a truncation" % frame["name"],
            lambda f=frame: decode(f["fields"], f["bytes"][:-1]),
        )

    check(
        "a 10 frame of two CLOSEs steps 9000 to 9002",
        read_control(bytes([0x04, 0xD0, 0x8C, 0x01, 0x04, 0x04])),
        [("CLOSE", {"token": 9000}), ("CLOSE", {"token": 9002})],
    )
    check(
        "a 10 OPEN that moved its tenure carries only the tenure and a check",
        read_control(bytes([0x00, 0x04, 0xAC, 0x02, 0x14, 0x02, 0x08, 0x10])),
        [
            (
                "OPEN",
                {"request": 2, "route": 300, "family": 0, "ordinal": 2, "tenure": 8, "check": 0x10},
            )
        ],
    )
    check(
        "an 18 v12 failed REPLY carries its code and reason",
        read_reply(bytes([0x07, 0x01, 0x2C, 0x02, 0x00, 0x6E, 0x6F]), 12),
        {"txn": 7, "failed": True, "code": 44, "detail": "no"},
    )
    check(
        "an 18 v12 answered REPLY counts its value slots",
        read_reply(bytes([0x07, 0x00, 0x01]), 12),
        {"txn": 7, "failed": False, "value_bits": 15},
    )
    check(
        "an 18 v11 REPLY has no failed bit",
        read_reply(bytes([0x07, 0x01, 0x00]), 11),
        {"txn": 7, "failed": False, "value_bits": 16},
    )
    rejects(
        "an 18 v12 failed REPLY refuses a trailing byte",
        lambda: read_reply(bytes([0x07, 0x01, 0x2C, 0x02, 0x00, 0x6E, 0x6F, 0x00]), 12),
    )
    rejects(
        "an 18 v12 failed REPLY refuses a reason cut short",
        lambda: read_reply(bytes([0x07, 0x01, 0x2C, 0x02, 0x00, 0x6E]), 12),
    )
    rejects("a 10 frame refuses an empty payload", lambda: read_control(b""))
    rejects(
        "a 10 frame refuses a record cut short",
        lambda: read_control(bytes([0x04, 0xD0, 0x8C, 0x01, 0x04])),
    )

    check(
        "the 10.2 delta names revision 4 minus distance 1 as its baseline",
        SNAPSHOT_DELTA["values"]["revision"] - SNAPSHOT_DELTA["values"]["distance"],
        3,
    )
    check(
        "the 10.2 full row writes no mask, so it is six bytes of columns",
        len(SNAPSHOT_FULL["bytes"]) - 3,
        6,
    )

    check(
        "a single-value range costs no bits",
        decode([{"name": "only", "kind": "int_range", "low": 7, "high": 7}], b""),
        {"only": 7},
    )

    check(
        "a two-group varuint reads its value",
        decode([{"name": "n", "kind": "varuint", "max_bytes": 5}], bytes([0xAC, 0x02]))["n"],
        300,
    )
    rejects(
        "a varuint with a redundant final group",
        lambda: decode([{"name": "n", "kind": "varuint", "max_bytes": 5}], bytes([0xAC, 0x82, 0x00])),
    )
    rejects(
        "a varuint past its byte limit",
        lambda: decode([{"name": "n", "kind": "varuint", "max_bytes": 1}], bytes([0x80, 0x01])),
    )

    for value, encoded in ((0, 0x00), (-1, 0x01), (1, 0x02), (-2, 0x03)):
        check(
            "svarint reads %d" % value,
            decode([{"name": "n", "kind": "svarint", "max_bytes": 5}], bytes([encoded]))["n"],
            value,
        )

    check(
        "bytes_capped spends a range on its length",
        decode([{"name": "b", "kind": "bytes_capped", "cap": 3}], bytes([0x02, 0xAA, 0xBB]))["b"],
        bytes([0xAA, 0xBB]),
    )

    rejects("a frame with residue", lambda: decode(HAND_BUILT["fields"], HAND_BUILT["bytes"] + b"\xff"))
    rejects("a frame that ends early", lambda: decode(HAND_BUILT["fields"], HAND_BUILT["bytes"][:2]))
    rejects(
        "alignment padding that is not zero",
        lambda: decode(
            [{"name": "one", "kind": "bool1"}, {"name": "b", "kind": "bytes_capped", "cap": 3}], bytes([0x83, 0xAA])
        ),
    )

    if spec_path is None:
        print(
            "EXCLUDED no --spec given, so nothing was held to the C++ "
            "descriptors -- this run proves only the decoder"
        )
    else:
        with open(spec_path) as handle:
            spec = json.load(handle)
        records = spec.get("records", {})

        # The spec names the format this build speaks, and a capture it writes
        # opens with one of that format's three magics. Every fixture below
        # decodes a record the caller already named, so none of them reads a
        # magic at all, and a format bump that moves the magics leaves them all
        # green while no live capture decodes. This is the case that reads the
        # magic the way a capture does, through the same table `capture()` uses.
        spec_format = spec.get("format")
        try:
            magics = datagram_magics(spec_format)
        except Poisoned as refused:
            wrong += 1
            magics = {}
            print("FAIL %-52s %s" % ("the spec's format has a magic table", refused))
        for magic, record in sorted(magics.items()):
            if record not in records:
                wrong += 1
                print("FAIL %-52s the spec holds no such record" % record)
                continue
            # The shortest datagram this format can carry, built from the spec's
            # own field list so the case needs no transcribed layout of its own.
            # Every field past the magic reads zero, and nothing follows the
            # carrier header, so a walk yields the header and no frame.
            width = 0
            for field in records[record]:
                width += field["width"] if field["kind"] == "bits" else 8
            body = bytes([magic]) + bytes((width - 8 + 7) // 8)
            walked = None
            try:
                walked, _frames = walk_datagram(records, {}, body, magics)
            except Poisoned as refused:
                print("FAIL %-52s %s" % (record, refused))
                wrong += 1
                continue
            check(
                "format %s walks 0x%02x as %s" % (spec_format, magic, record),
                walked.get("magic"),
                magic,
            )
        if magics:
            stranger = (max(magics) + 1) & 0xFF
            rejects(
                "format %s refuses a datagram magic it does not speak" % spec_format,
                lambda m=stranger: walk_datagram(records, {}, bytes([m]) + b"\x01", magics),
            )

        for frame in AGAINST_SPEC:
            name = frame["record"]
            if name not in records:
                wrong += 1
                print("FAIL %-52s the spec holds no such record" % name)
                continue
            try:
                got = decode(records[name], frame["bytes"])
            except Poisoned as refused:
                wrong += 1
                print("FAIL %-52s %s" % (name, refused))
                continue
            check("%s decodes as its prose reads (%s)" % (name, frame["why"]), got, frame["values"])
            rejects(
                "%s rejects a trailing byte" % name, lambda f=frame, r=records[name]: decode(r, f["bytes"] + b"\xff")
            )

    print("SELFTEST %d wrong" % wrong)
    return 1 if wrong else 0


ROW_CHANNELS = {39: "SYNC_ROW", 40: "SYNC_ROW_DELTA", 41: "SYNC_ROW_WINDOW"}
CONTROL_CHANNEL = 43
CONTROL_TAGS = {0: "OPEN", 1: "READY", 2: "ACCEPT", 3: "RESET", 4: "CLOSE"}


def read_row_head(payload):
    """A v11 state row's stream name, from WIRE.md 8.

    The token, the revision and the distance precede the mask, and none of the
    three depends on the schema, so a row names its stream in a capture whose
    header carries no compiled column list.
    """
    stream = ReadStream(payload)
    token = stream.varuint(10)
    token_bits = stream.read
    revision = stream.varuint(10)
    revision_bits = stream.read - token_bits
    distance = stream.bits(6)
    return token, revision, distance, token_bits, revision_bits


OPEN_FIELDS = (
    ("epoch", "varuint", 3),
    ("tenure", "varuint", 5),
    ("anchor", "varuint", 5),
    ("anchor_author", "varuint", 5),
    ("schema", "bits", 32),
)


def read_control(payload):
    """Every record of one ROW_CONTROL frame, from WIRE.md 10.

    Ids are resolved from their steps, which never leave the frame. An OPEN
    field its writer left to the basis is absent from the body, because the
    basis lives in the two peers and a capture of one direction cannot rebuild
    it.
    """
    if not payload:
        raise Poisoned("a control frame carries no record")
    stream = ReadStream(payload)
    cursor = {"opened": 0, "answered": 0, "minted": 0, "closed": 0}

    def step(kind):
        cursor[kind] = (cursor[kind] + stream.svarint(10)) & ((1 << 64) - 1)
        return cursor[kind]

    out = []
    while stream.remaining():
        tag = stream.bits(8)
        if tag not in CONTROL_TAGS:
            raise Poisoned("control tag %d is above 4" % tag)
        name = CONTROL_TAGS[tag]
        body = {}
        if name == "OPEN":
            body["request"] = step("opened")
            body["route"] = stream.varuint(5)
            body["family"] = stream.bits(2)
            if body["family"] > 2:
                raise Poisoned("stream family %d is above 2" % body["family"])
            has_ordinal = stream.bool1()
            present = [stream.bool1() for _ in OPEN_FIELDS]
            body["ordinal"] = stream.bits(8) if has_ordinal else 0
            if has_ordinal and body["ordinal"] == 0:
                raise Poisoned("a present ordinal is 0")
            for (field, kind, width), carried in zip(OPEN_FIELDS, present):
                if carried:
                    body[field] = stream.varuint(width) if kind == "varuint" else stream.bits(width)
            if not all(present):
                body["check"] = stream.bits(8)
        elif name == "READY":
            body["request"] = step("answered")
            body["token"] = step("minted")
        elif name == "RESET":
            body["request"] = step("answered")
            body["token"] = stream.varuint(10)
        elif name == "CLOSE":
            body["token"] = step("closed")
        else:
            count = stream.int_range(1, 32)
            entries = []
            token = 0
            for _ in range(count):
                token = (token + stream.svarint(10)) & ((1 << 64) - 1)
                revision = stream.varuint(10)
                if token == 0 or revision == 0:
                    raise Poisoned("an ACCEPT entry names token or revision 0")
                entries.append((token, revision))
            body["entries"] = entries
        stream.align_verify()
        out.append((name, body))
    return out


def percentile(values, share):
    if not values:
        return 0
    ordered = sorted(values)
    at = min(len(ordered) - 1, int(share * (len(ordered) - 1) + 0.5))
    return ordered[at]


def account(path, spec_path=None):
    """The byte and latency account one peer's capture can answer alone.

    A row names its stream and a receipt names the same stream, both without a
    schema, so a capture joins the two by (token, revision) and answers what a
    receipt cost in time and what a lane owed while it waited.
    """
    header, rows = read_capture(path)
    records = header.get("records", {})
    if spec_path:
        with open(spec_path) as handle:
            records = json.load(handle).get("records", records)
    channels = {int(row["id"]): row["name"] for row in header.get("channels", [])}
    magics = datagram_magics(header.get("format"))

    payload_bytes = {0: 0, 1: 0}
    datagram_bytes = {0: 0, 1: 0}
    control_bytes = {0: 0, 1: 0}
    row_bytes = {0: 0, 1: 0}
    control_records = {}
    rows_by_distance = {"full": 0, "delta": 0}
    naming_bits = {0: 0, 1: 0}
    naming_rows = {0: 0, 1: 0}

    exposed_at = {}
    confirmed = {}
    exposed_high = {}
    receipt_latency = []
    pending_series = []
    ready_at = {}
    first_full_in = {}
    errors = 0

    for index, row in enumerate(rows):
        direction = row["dir"]
        wall = row["wall_ms"]
        datagram_bytes[direction] += len(row["bytes"])
        try:
            _head, frames = walk_datagram(records, channels, row["bytes"], magics)
        except Poisoned:
            errors += 1
            continue
        for frame in frames:
            size = len(frame["payload"])
            payload_bytes[direction] += size
            channel = frame["channel"]
            if channel in ROW_CHANNELS:
                row_bytes[direction] += size
                if channel == 41:
                    continue
                try:
                    token, revision, distance, token_bits, revision_bits = read_row_head(frame["payload"])
                except Poisoned:
                    errors += 1
                    continue
                rows_by_distance["full" if distance == 0 else "delta"] += 1
                naming_bits[direction] += token_bits + revision_bits
                naming_rows[direction] += 1
                if direction == 0:
                    exposed_at.setdefault((token, revision), wall)
                    if revision > exposed_high.get(token, 0):
                        exposed_high[token] = revision
                elif distance == 0 and token not in first_full_in:
                    first_full_in[token] = wall
            elif channel == CONTROL_CHANNEL:
                control_bytes[direction] += size
                try:
                    carried = read_control(frame["payload"])
                except Poisoned:
                    errors += 1
                    continue
                for name, body in carried:
                    control_records[name] = control_records.get(name, 0) + 1
                    if name == "READY":
                        ready_at.setdefault(body["token"], wall)
                    if name == "ACCEPT" and direction == 1:
                        for token, revision in body["entries"]:
                            if revision > confirmed.get(token, 0):
                                confirmed[token] = revision
                            sent = exposed_at.get((token, revision))
                            if sent is not None:
                                receipt_latency.append(wall - sent)
        owing = sum(1 for token, high in exposed_high.items() if high > confirmed.get(token, 0))
        pending_series.append(owing)

    print("ACCOUNT %s format=%s" % (path, header.get("format")))
    for direction, label in ((0, "OUT"), (1, "IN ")):
        carrier = datagram_bytes[direction] - payload_bytes[direction]
        total = datagram_bytes[direction] or 1
        print(
            "  %s datagram=%d payload=%d carrier=%d (%.1f%%) "
            "rows=%d (%.1f%%) control=%d (%.1f%%)"
            % (
                label,
                datagram_bytes[direction],
                payload_bytes[direction],
                carrier,
                100.0 * carrier / total,
                row_bytes[direction],
                100.0 * row_bytes[direction] / total,
                control_bytes[direction],
                100.0 * control_bytes[direction] / total,
            )
        )
    print("  control records %s" % control_records)
    print("  rows full=%d delta=%d" % (rows_by_distance["full"], rows_by_distance["delta"]))
    for direction, label in ((0, "OUT"), (1, "IN ")):
        if not naming_rows[direction]:
            continue
        print(
            "  %s row naming rows=%d token+revision bits/row=%.2f bytes=%d "
            "(%.1f%% of row bytes)"
            % (
                label,
                naming_rows[direction],
                naming_bits[direction] / naming_rows[direction],
                naming_bits[direction] // 8,
                100.0 * (naming_bits[direction] / 8.0) / (row_bytes[direction] or 1),
            )
        )
    print(
        "  receipt latency ms n=%d p50=%d p90=%d max=%d"
        % (
            len(receipt_latency),
            percentile(receipt_latency, 0.50),
            percentile(receipt_latency, 0.90),
            max(receipt_latency) if receipt_latency else 0,
        )
    )
    print(
        "  pending lanes max=%d p90=%d streams=%d"
        % (
            max(pending_series) if pending_series else 0,
            percentile(pending_series, 0.90),
            len(exposed_high) or len(first_full_in),
        )
    )
    usable = [
        first_full_in[token] - ready_at[token]
        for token in first_full_in
        if token in ready_at and first_full_in[token] >= ready_at[token]
    ]
    print(
        "  first usable row ms after READY n=%d p50=%d max=%d"
        % (len(usable), percentile(usable, 0.50), max(usable) if usable else 0)
    )
    if errors:
        print("  UNDECODED %d" % errors)
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec")
    parser.add_argument("--record")
    parser.add_argument("--hex")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--capture")
    parser.add_argument("--account")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test(args.spec)
    if args.account:
        try:
            return account(args.account, args.spec)
        except Poisoned as refused:
            sys.exit("ERROR %s" % refused)
    if args.capture:
        try:
            return capture(args.capture, args.verbose)
        except Poisoned as refused:
            sys.exit("ERROR %s" % refused)
    if not (args.spec and args.record and args.hex):
        parser.error("--spec, --record and --hex are required without --self-test")

    with open(args.spec) as handle:
        spec = json.load(handle)
    records = spec.get("records", {})
    if args.record not in records:
        sys.exit("spec holds no record named %s" % args.record)
    try:
        decoded = decode(records[args.record], bytes.fromhex(args.hex))
    except Poisoned as refused:
        sys.exit("ERROR %s" % refused)
    for name, value in decoded.items():
        print("%s = %r" % (name, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
