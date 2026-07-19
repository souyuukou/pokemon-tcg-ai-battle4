from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np

MAGIC = b"PTCGEV3\0"
VERSION = FEATURE_SCHEMA = INFORMATION_SCHEMA = 3
EFFECT_SCHEMA = COMBO_SCHEMA = 1
GLOBAL_DENSE = ENTITY_DENSE = 16
ENTITY_HIDDEN = 24
GLOBAL_HIDDEN = 64
POOLS = 4
GLOBAL_RELATIONS = 24
ENTITY_RELATIONS = 16
WEIGHT_SCALE = 4096
BELIEF_SCALE = 256
SCORE_SCALE = 100_000_000
NON_TERMINAL_LIMIT = 90_000_000
HEADER = struct.Struct("<8s16I4Q32s")


def fnv1a(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & ((1 << 64) - 1)
    return value


@dataclass(frozen=True)
class EntityFeatures:
    pool: int
    dense: Sequence[int]
    sparse: Sequence[Sequence[int]]


@dataclass(frozen=True)
class FeatureRecord:
    global_dense: Sequence[int]
    global_sparse: Sequence[Sequence[int]]
    entities: Sequence[EntityFeatures]


@dataclass(frozen=True)
class QuantizedModel:
    tokens: np.ndarray
    entity_dense_weight: np.ndarray
    entity_sparse_weight: np.ndarray
    entity_bias: np.ndarray
    global_dense_weight: np.ndarray
    global_sparse_weight: np.ndarray
    pool_weight: np.ndarray
    global_bias: np.ndarray
    output_weight: np.ndarray
    output_bias: int
    dataset_hash: bytes = bytes(32)
    card_checksum: int = 0
    effect_checksum: int = 0
    combo_checksum: int = 0

    def validate(self) -> None:
        ids = np.asarray(self.tokens, dtype=np.int32)
        if ids.ndim != 1 or not len(ids) or ids[0] != 0 or np.any(ids[1:] <= ids[:-1]):
            raise ValueError("tokens must be unique, sorted, and start with UNK=0")
        expected = (
            (self.entity_dense_weight, (ENTITY_HIDDEN, ENTITY_DENSE)),
            (self.entity_sparse_weight, (ENTITY_RELATIONS, len(ids), ENTITY_HIDDEN)),
            (self.entity_bias, (ENTITY_HIDDEN,)),
            (self.global_dense_weight, (GLOBAL_HIDDEN, GLOBAL_DENSE)),
            (self.global_sparse_weight, (GLOBAL_RELATIONS, len(ids), GLOBAL_HIDDEN)),
            (self.pool_weight, (POOLS, ENTITY_HIDDEN, GLOBAL_HIDDEN)),
            (self.global_bias, (GLOBAL_HIDDEN,)),
            (self.output_weight, (GLOBAL_HIDDEN,)),
        )
        for value, shape in expected:
            if np.asarray(value).shape != shape:
                raise ValueError(f"invalid V3 weight shape: expected {shape}")
        if len(self.dataset_hash) != 32:
            raise ValueError("dataset hash must have 32 bytes")
        entity_dense_max = np.asarray([1,1,10_000,10_000,100,3,100,1,0,6,10_000,32_767,0,1,0,1], dtype=object)
        entity_sparse_max = np.asarray([256,256,256,256,256,15_360,1_024,2_048,32_768,
                                        2_100_000,409_600,4_096,24_576,12_288,409_600,409_600], dtype=object)
        entity_bound = np.abs(np.asarray(self.entity_bias, dtype=object))
        entity_bound += np.abs(np.asarray(self.entity_dense_weight, dtype=object)) @ entity_dense_max
        entity_bound += np.max(np.abs(np.asarray(self.entity_sparse_weight, dtype=np.int64)), axis=1).T.astype(object) @ entity_sparse_max
        if np.any(entity_bound > (1 << 31) - 1):
            raise ValueError("V3 entity accumulator bound exceeds int32")
        global_dense_max = np.asarray([1,1,10_000,60,60,60,60,60,4,32,1,60,0,0,3,1], dtype=object)
        global_sparse_max = np.asarray([15_360,15_360,15_360,256,15_360,15_360,15_360,15_360,15_360,
                                        15_360,15_360,256,256,256,256,256,256,256,256,
                                        2_100_000,2_100_000,2_100_000,15_360,512], dtype=object)
        global_bound = np.abs(np.asarray(self.global_bias, dtype=object))
        global_bound += np.abs(np.asarray(self.global_dense_weight, dtype=object)) @ global_dense_max
        global_bound += np.max(np.abs(np.asarray(self.global_sparse_weight, dtype=np.int64)), axis=1).T.astype(object) @ global_sparse_max
        pool_abs = np.abs(np.asarray(self.pool_weight, dtype=object))
        global_bound += pool_abs[0].sum(axis=0) * 127 + pool_abs[1].sum(axis=0) * 127
        global_bound += pool_abs[2].sum(axis=0) * 127 * 8 + pool_abs[3].sum(axis=0) * 127 * 8
        if np.any(global_bound > (1 << 31) - 1):
            raise ValueError("V3 global accumulator bound exceeds int32")


def export_quantized(path: str | Path, model: QuantizedModel) -> None:
    model.validate()
    tokens = np.asarray(model.tokens, dtype="<i4")
    header = HEADER.pack(
        MAGIC, VERSION, GLOBAL_DENSE, ENTITY_DENSE, ENTITY_HIDDEN, GLOBAL_HIDDEN, POOLS,
        GLOBAL_RELATIONS, ENTITY_RELATIONS, len(tokens), WEIGHT_SCALE, BELIEF_SCALE, SCORE_SCALE,
        FEATURE_SCHEMA, INFORMATION_SCHEMA, EFFECT_SCHEMA, COMBO_SCHEMA,
        fnv1a(tokens.tobytes()), int(model.card_checksum), int(model.effect_checksum), int(model.combo_checksum),
        model.dataset_hash,
    )
    arrays = (
        tokens,
        np.asarray(model.entity_dense_weight, dtype="<i2"),
        np.asarray(model.entity_sparse_weight, dtype="<i2"),
        np.asarray(model.entity_bias, dtype="<i4"),
        np.asarray(model.global_dense_weight, dtype="<i2"),
        np.asarray(model.global_sparse_weight, dtype="<i2"),
        np.asarray(model.pool_weight, dtype="<i2"),
        np.asarray(model.global_bias, dtype="<i4"),
        np.asarray(model.output_weight, dtype="<i2"),
    )
    Path(path).write_bytes(header + b"".join(a.tobytes(order="C") for a in arrays)
                           + struct.pack("<q", int(model.output_bias)))


def load_quantized(path: str | Path) -> QuantizedModel:
    raw = Path(path).read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError("invalid V3 evaluator length")
    fields = HEADER.unpack_from(raw)
    (magic, version, gd, ed, eh, gh, pools, gr, er, token_count,
     weight_scale, belief_scale, score_scale, feature_schema, information_schema,
     effect_schema, combo_schema, token_checksum, card_checksum, effect_checksum,
     combo_checksum, dataset_hash) = fields
    if (magic, version, gd, ed, eh, gh, pools, gr, er, weight_scale, belief_scale,
        score_scale, feature_schema, information_schema, effect_schema, combo_schema) != (
        MAGIC, VERSION, GLOBAL_DENSE, ENTITY_DENSE, ENTITY_HIDDEN, GLOBAL_HIDDEN, POOLS,
        GLOBAL_RELATIONS, ENTITY_RELATIONS, WEIGHT_SCALE, BELIEF_SCALE, SCORE_SCALE,
        FEATURE_SCHEMA, INFORMATION_SCHEMA, EFFECT_SCHEMA, COMBO_SCHEMA):
        raise ValueError("invalid V3 evaluator header")
    offset = HEADER.size

    def take(dtype: str, count: int) -> np.ndarray:
        nonlocal offset
        size = np.dtype(dtype).itemsize * count
        if offset + size > len(raw):
            raise ValueError("truncated V3 evaluator")
        value = np.frombuffer(raw, dtype=dtype, count=count, offset=offset).copy()
        offset += size
        return value

    tokens = take("<i4", token_count)
    if fnv1a(tokens.tobytes()) != token_checksum:
        raise ValueError("V3 token checksum mismatch")
    edw = take("<i2", ENTITY_HIDDEN * ENTITY_DENSE).reshape(ENTITY_HIDDEN, ENTITY_DENSE)
    esw = take("<i2", ENTITY_RELATIONS * token_count * ENTITY_HIDDEN).reshape(ENTITY_RELATIONS, token_count, ENTITY_HIDDEN)
    eb = take("<i4", ENTITY_HIDDEN)
    gdw = take("<i2", GLOBAL_HIDDEN * GLOBAL_DENSE).reshape(GLOBAL_HIDDEN, GLOBAL_DENSE)
    gsw = take("<i2", GLOBAL_RELATIONS * token_count * GLOBAL_HIDDEN).reshape(GLOBAL_RELATIONS, token_count, GLOBAL_HIDDEN)
    pw = take("<i2", POOLS * ENTITY_HIDDEN * GLOBAL_HIDDEN).reshape(POOLS, ENTITY_HIDDEN, GLOBAL_HIDDEN)
    gb = take("<i4", GLOBAL_HIDDEN)
    ow = take("<i2", GLOBAL_HIDDEN)
    if offset + 8 != len(raw):
        raise ValueError("invalid V3 payload length")
    output_bias, = struct.unpack_from("<q", raw, offset)
    result = QuantizedModel(tokens, edw, esw, eb, gdw, gsw, pw, gb, ow, output_bias,
                            dataset_hash, card_checksum, effect_checksum, combo_checksum)
    result.validate()
    return result


def _rounded_divide(value: np.ndarray, divisor: int) -> np.ndarray:
    magnitude = np.abs(value)
    result = (magnitude + divisor // 2) // divisor
    return np.where(value < 0, -result, result)


def _predict_integer_validated(model: QuantizedModel, feature: FeatureRecord,
                               index: dict[int, int]) -> int:
    gd = np.asarray(feature.global_dense, dtype=np.int64)
    if gd.shape != (GLOBAL_DENSE,):
        raise ValueError("invalid global dense shape")
    global_acc = model.global_bias.astype(np.int64) + model.global_dense_weight.astype(np.int64) @ gd
    for relation, token, value in feature.global_sparse:
        global_acc += model.global_sparse_weight[int(relation), index.get(int(token), 0)].astype(np.int64) * int(value)
    for entity in feature.entities:
        if not 0 <= int(entity.pool) < POOLS:
            raise ValueError("invalid entity pool")
        dense = np.asarray(entity.dense, dtype=np.int64)
        if dense.shape != (ENTITY_DENSE,):
            raise ValueError("invalid entity dense shape")
        acc = model.entity_bias.astype(np.int64) + model.entity_dense_weight.astype(np.int64) @ dense
        for relation, token, value in entity.sparse:
            acc += model.entity_sparse_weight[int(relation), index.get(int(token), 0)].astype(np.int64) * int(value)
        activation = np.clip(acc, 0, 127 * WEIGHT_SCALE)
        projection = activation @ model.pool_weight[int(entity.pool)].astype(np.int64)
        global_acc += _rounded_divide(projection, WEIGHT_SCALE)
    activation = np.clip(global_acc, 0, 127 * WEIGHT_SCALE)
    output = int(model.output_bias) + int(np.dot(model.output_weight.astype(object), activation.astype(object)))
    divisor = WEIGHT_SCALE * WEIGHT_SCALE
    magnitude = abs(output)
    score = (magnitude // divisor) * SCORE_SCALE + ((magnitude % divisor) * SCORE_SCALE + divisor // 2) // divisor
    if output < 0:
        score = -score
    return max(-NON_TERMINAL_LIMIT, min(NON_TERMINAL_LIMIT, score))


def predict_integer(model: QuantizedModel, feature: FeatureRecord) -> int:
    model.validate()
    index = {int(token): i for i, token in enumerate(model.tokens)}
    return _predict_integer_validated(model, feature, index)


def predict_integer_many(model: QuantizedModel,
                         features: Sequence[FeatureRecord]) -> list[int]:
    """Evaluate many records while validating the immutable model only once."""
    model.validate()
    index = {int(token): i for i, token in enumerate(model.tokens)}
    return [_predict_integer_validated(model, feature, index) for feature in features]


def manifest_digest(path: str | Path | None) -> bytes:
    return bytes(32) if path is None else hashlib.sha256(Path(path).read_bytes()).digest()
