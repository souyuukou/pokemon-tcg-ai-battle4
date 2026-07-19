"""Stable canonical keys. Hashes accelerate lookup; full bytes decide equality."""

from __future__ import annotations

from dataclasses import asdict, dataclass, is_dataclass
from enum import Enum
import struct
from typing import Any, Mapping


MASK64 = (1 << 64) - 1
SERIAL_FIELDS = {"serial", "serialActive", "serialBench", "serialBefore", "serialAfter", "serialTarget"}
UNORDERED_FIELDS = {"hand", "discard", "energies", "energyCards", "tools", "preEvolution"}
EXCLUDED_OBSERVATION_FIELDS = {"logs", "search_begin_input"}


def _rotl(x: int, b: int) -> int:
    return ((x << b) | (x >> (64 - b))) & MASK64


def siphash24(data: bytes, key: bytes) -> int:
    """Reference SipHash-2-4 with a fixed 128-bit key."""
    if len(key) != 16:
        raise ValueError("SipHash key must be 16 bytes")
    k0, k1 = struct.unpack("<QQ", key)
    v0, v1 = 0x736F6D6570736575 ^ k0, 0x646F72616E646F6D ^ k1
    v2, v3 = 0x6C7967656E657261 ^ k0, 0x7465646279746573 ^ k1

    def rounds(n: int) -> None:
        nonlocal v0, v1, v2, v3
        for _ in range(n):
            v0 = (v0 + v1) & MASK64; v1 = _rotl(v1, 13); v1 ^= v0; v0 = _rotl(v0, 32)
            v2 = (v2 + v3) & MASK64; v3 = _rotl(v3, 16); v3 ^= v2
            v0 = (v0 + v3) & MASK64; v3 = _rotl(v3, 21); v3 ^= v0
            v2 = (v2 + v1) & MASK64; v1 = _rotl(v1, 17); v1 ^= v2; v2 = _rotl(v2, 32)

    end = len(data) - len(data) % 8
    for offset in range(0, end, 8):
        m = struct.unpack_from("<Q", data, offset)[0]
        v3 ^= m; rounds(2); v0 ^= m
    b = len(data) << 56
    for i, value in enumerate(data[end:]):
        b |= value << (8 * i)
    v3 ^= b; rounds(2); v0 ^= b; v2 ^= 0xFF; rounds(4)
    return (v0 ^ v1 ^ v2 ^ v3) & MASK64


@dataclass(frozen=True, slots=True)
class CanonicalKey:
    lo: int
    hi: int
    key_id: int


class KeyMemoryLimit(RuntimeError):
    pass


class KeyArena:
    """Interns immutable full keys. Digest collisions are deliberately harmless."""
    KEY0 = bytes.fromhex("00112233445566778899aabbccddeeff")
    KEY1 = bytes.fromhex("ffeeddccbbaa99887766554433221100")

    def __init__(self, hash_fn=None, max_bytes: int = 700 * 1024 * 1024):
        self._keys: list[bytes] = []
        self._buckets: dict[tuple[int, int], list[int]] = {}
        self._hash_fn = hash_fn
        self.max_bytes = max_bytes
        self.bytes_used = 0

    def intern(self, raw: bytes) -> CanonicalKey:
        lo, hi = self._digest(raw)
        bucket = self._buckets.setdefault((lo, hi), [])
        for key_id in bucket:
            if self._keys[key_id] == raw:
                return CanonicalKey(lo, hi, key_id)
        if self.bytes_used + len(raw) > self.max_bytes:
            raise KeyMemoryLimit("canonical key arena memory limit reached")
        key_id = len(self._keys)
        self._keys.append(raw)
        self.bytes_used += len(raw)
        bucket.append(key_id)
        return CanonicalKey(lo, hi, key_id)

    def bytes_for(self, key: CanonicalKey) -> bytes:
        return self._keys[key.key_id]

    def equal(self, a: CanonicalKey, b: CanonicalKey) -> bool:
        return a.lo == b.lo and a.hi == b.hi and self.bytes_for(a) == self.bytes_for(b)

    def _digest(self, raw: bytes) -> tuple[int, int]:
        if self._hash_fn is not None:
            return self._hash_fn(raw)
        return siphash24(raw, self.KEY0), siphash24(raw, self.KEY1)


def _uvarint(value: int) -> bytes:
    value = value * 2 if value >= 0 else -value * 2 - 1
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80); value >>= 7
    out.append(value)
    return bytes(out)


def canonical_bytes(value: Any, *, unordered_fields=frozenset()) -> bytes:
    """Type-tagged, length-delimited encoding independent of Python repr/order."""
    if is_dataclass(value):
        value = asdict(value)
    if value is None: return b"n"
    if isinstance(value, bool): return b"t" if value else b"f"
    if isinstance(value, Enum): return b"e" + canonical_bytes(value.value)
    if isinstance(value, int): return b"i" + _uvarint(value)
    if isinstance(value, str):
        raw = value.encode("utf-8"); return b"s" + _uvarint(len(raw)) + raw
    if isinstance(value, bytes): return b"b" + _uvarint(len(value)) + value
    if isinstance(value, Mapping):
        parts = []
        for key in sorted(value, key=lambda x: canonical_bytes(x)):
            encoded = canonical_bytes(value[key], unordered_fields=unordered_fields)
            parts.append(canonical_bytes(key) + _uvarint(len(encoded)) + encoded)
        body = b"".join(parts); return b"m" + _uvarint(len(parts)) + body
    if isinstance(value, (list, tuple, set, frozenset)):
        parts = [canonical_bytes(v, unordered_fields=unordered_fields) for v in value]
        if isinstance(value, (set, frozenset)): parts.sort()
        body = b"".join(_uvarint(len(p)) + p for p in parts)
        return b"l" + _uvarint(len(parts)) + body
    raise TypeError(f"unsupported canonical type: {type(value)!r}")


def _plain(value: Any) -> Any:
    if is_dataclass(value): return {k: _plain(v) for k, v in asdict(value).items()}
    if isinstance(value, Enum): return value.value
    if isinstance(value, Mapping): return {k: _plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)): return [_plain(v) for v in value]
    return value


def _strip_and_sort(value: Any, field: str | None = None) -> Any:
    if isinstance(value, dict):
        result = {k: _strip_and_sort(v, k) for k, v in value.items() if k not in EXCLUDED_OBSERVATION_FIELDS}
        return result
    if isinstance(value, list):
        result = [_strip_and_sort(v) for v in value]
        if field in UNORDERED_FIELDS:
            result.sort(key=canonical_bytes)
        if field == "bench":
            result.sort(key=canonical_bytes)
        return result
    return value


def _alpha_rename_serials(value: Any) -> Any:
    """Rename physical serials after structural ordering, preserving references."""
    mapping: dict[int, int] = {}
    def visit(v: Any, field: str | None = None) -> Any:
        if isinstance(v, dict): return {k: visit(x, k) for k, x in v.items()}
        if isinstance(v, list): return [visit(x) for x in v]
        if field in SERIAL_FIELDS and isinstance(v, int):
            if v not in mapping: mapping[v] = len(mapping)
            return mapping[v]
        return v
    return visit(value)


def observation_key(observation: Any, namespace: Mapping[str, Any]) -> bytes:
    """Information-state key: contains only what the acting player observes."""
    plain = _strip_and_sort(_plain(observation))
    plain = _alpha_rename_serials(plain)
    return canonical_bytes({"namespace": dict(namespace), "observation": plain})
