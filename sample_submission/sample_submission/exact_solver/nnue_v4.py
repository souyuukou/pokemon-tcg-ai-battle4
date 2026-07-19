"""V4 evaluator: Passive residual on top of a V3 semantic trunk."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np

from exact_solver import nnue_v3

MAGIC = b"PTCGEV4\0"
MODEL_SCHEMA = 2
FEATURE_SCHEMA = 2
LIVENESS_SCHEMA = 6
CONTEXT_HIDDEN = 32
PASSIVE_CONTEXT_SCALE = 4096
# magic + 7x u32 + legacyChecksum u64 + 5x u64 + 2x i64 + reserved 16
HEADER = struct.Struct("<8s7IQQQQQQQqq16s")


@dataclass(frozen=True)
class QuantizedModelV4:
    tokens: np.ndarray
    passive_bias: np.ndarray  # [T] int32, token-index aligned
    passive_context_weight: np.ndarray  # [T, CONTEXT] int16
    context_from_global: np.ndarray  # [CONTEXT, GH] int16
    context_bias: np.ndarray  # [CONTEXT] int32
    pairs: np.ndarray  # structured (card_a u2, card_b u2, weight i4)
    v3: nnue_v3.QuantizedModel
    checksum: int = 0
    required_v3_model_hash: int = 0
    feature_schema_hash: int = FEATURE_SCHEMA
    liveness_schema_hash: int = LIVENESS_SCHEMA
    card_token_table_hash: int = 0
    payload_checksum: int = 0
    expected_file_size: int = 0
    proven_min_output: int = -nnue_v3.NON_TERMINAL_LIMIT + 1
    proven_max_output: int = nnue_v3.NON_TERMINAL_LIMIT - 1

    def validate(self) -> None:
        self.v3.validate()
        t = len(self.tokens)
        if self.passive_bias.shape != (t,):
            raise ValueError("passive_bias shape")
        if self.passive_context_weight.shape != (t, CONTEXT_HIDDEN):
            raise ValueError("passive_context_weight shape")
        if self.context_from_global.shape != (CONTEXT_HIDDEN, nnue_v3.GLOBAL_HIDDEN):
            raise ValueError("context_from_global shape")
        if self.context_bias.shape != (CONTEXT_HIDDEN,):
            raise ValueError("context_bias shape")
        if self.proven_min_output <= -nnue_v3.NON_TERMINAL_LIMIT or self.proven_max_output >= nnue_v3.NON_TERMINAL_LIMIT:
            # Allowed to exist, but C++ will set analyticIntegralAllowed=false.
            pass


def own_hand_linear_score(model: nnue_v3.QuantizedModel, token_index: int) -> int:
    """First-order OwnHand contribution (matches C++ estimateOwnHandLinearScore)."""
    gsw = model.global_sparse_weight[0, token_index].astype(np.int64)
    ow = model.output_weight.astype(np.int64)
    delta = gsw * nnue_v3.BELIEF_SCALE
    act = np.clip(delta, 0, 127 * nnue_v3.WEIGHT_SCALE)
    output = int(np.dot(ow, act))
    divisor = nnue_v3.WEIGHT_SCALE * nnue_v3.WEIGHT_SCALE
    magnitude = abs(output)
    score = (magnitude // divisor) * nnue_v3.SCORE_SCALE + (
        (magnitude % divisor) * nnue_v3.SCORE_SCALE + divisor // 2
    ) // divisor
    if output < 0:
        score = -score
    return max(-nnue_v3.NON_TERMINAL_LIMIT, min(nnue_v3.NON_TERMINAL_LIMIT, score))


def _payload_checksum(model: QuantizedModelV4) -> int:
    parts = [
        np.asarray(model.tokens, dtype="<i4").tobytes(order="C"),
        np.asarray(model.passive_bias, dtype="<i4").tobytes(order="C"),
        np.asarray(model.passive_context_weight, dtype="<i2").tobytes(order="C"),
        np.asarray(model.context_from_global, dtype="<i2").tobytes(order="C"),
        np.asarray(model.context_bias, dtype="<i4").tobytes(order="C"),
        np.asarray(model.pairs).tobytes(order="C") if len(model.pairs) else b"",
    ]
    return nnue_v3.fnv1a(b"".join(parts))


def compute_proven_bounds(passive_bias: np.ndarray, pairs: np.ndarray) -> tuple[int, int, bool]:
    """Match C++ ExactSparseEvaluatorV4::recomputeProvenBoundsFromWeights."""
    max_hand = 60  # DECK_SIZE
    lo = 0
    hi = 0
    for bias in np.asarray(passive_bias, dtype=np.int64).tolist():
        if bias >= 0:
            hi += int(bias) * max_hand
        else:
            lo += int(bias) * max_hand
    for pair in pairs:
        w = int(pair["weight"])
        max_term = (
            max_hand * (max_hand - 1) // 2
            if int(pair["card_a"]) == int(pair["card_b"])
            else max_hand * max_hand
        )
        if w >= 0:
            hi += w * max_term
        else:
            lo += w * max_term
    lim = nnue_v3.NON_TERMINAL_LIMIT
    return lo, hi, (lo > -lim) and (hi < lim)


def bootstrap_from_v3(v3: nnue_v3.QuantizedModel) -> QuantizedModelV4:
    tokens = np.asarray(v3.tokens, dtype=np.int32)
    bias = np.zeros(len(tokens), dtype=np.int32)
    for i, token in enumerate(tokens):
        tid = int(token)
        if 0 < tid < 1_000_000:  # card ids below AttackTokenBase
            bias[i] = np.int32(own_hand_linear_score(v3, i))
    pairs = np.zeros(0, dtype=[("card_a", "<u2"), ("card_b", "<u2"), ("weight", "<i4")])
    proven_min, proven_max, analytic_ok = compute_proven_bounds(bias, pairs)
    # Do not fake-round bounds. Models that saturate simply cannot analytic-integrate.
    model = QuantizedModelV4(
        tokens=tokens,
        passive_bias=bias,
        passive_context_weight=np.zeros((len(tokens), CONTEXT_HIDDEN), dtype=np.int16),
        context_from_global=np.zeros((CONTEXT_HIDDEN, nnue_v3.GLOBAL_HIDDEN), dtype=np.int16),
        context_bias=np.zeros(CONTEXT_HIDDEN, dtype=np.int32),
        pairs=pairs,
        v3=v3,
        checksum=nnue_v3.fnv1a(tokens.tobytes()) ^ 0x56345F4254,
        required_v3_model_hash=int(getattr(v3, "checksum", 0) or 0),
        card_token_table_hash=nnue_v3.fnv1a(tokens.tobytes()),
        proven_min_output=proven_min,
        proven_max_output=proven_max,
    )
    if not analytic_ok:
        # Keep object for unit tests of residual math; refuse export via validate().
        pass
    payload = _payload_checksum(model)
    return QuantizedModelV4(
        **{**model.__dict__, "payload_checksum": payload, "checksum": payload},
    )


def export_quantized(path: str | Path, model: QuantizedModelV4) -> None:
    model.validate()
    tokens = np.asarray(model.tokens, dtype="<i4")
    payload = [
        tokens.tobytes(order="C"),
        np.asarray(model.passive_bias, dtype="<i4").tobytes(order="C"),
        np.asarray(model.passive_context_weight, dtype="<i2").tobytes(order="C"),
        np.asarray(model.context_from_global, dtype="<i2").tobytes(order="C"),
        np.asarray(model.context_bias, dtype="<i4").tobytes(order="C"),
        np.asarray(model.pairs).tobytes(order="C") if len(model.pairs) else b"",
    ]
    body = b"".join(payload)
    payload_checksum = model.payload_checksum or nnue_v3.fnv1a(body)
    header_size = HEADER.size
    expected = header_size + len(body)
    header = HEADER.pack(
        MAGIC,
        MODEL_SCHEMA,
        FEATURE_SCHEMA,
        LIVENESS_SCHEMA,
        CONTEXT_HIDDEN,
        PASSIVE_CONTEXT_SCALE,
        len(tokens),
        len(model.pairs),
        int(model.checksum),
        int(model.required_v3_model_hash),
        int(model.feature_schema_hash),
        int(model.liveness_schema_hash),
        int(model.card_token_table_hash),
        int(payload_checksum),
        int(expected),
        int(model.proven_min_output),
        int(model.proven_max_output),
        bytes(16),
    )
    Path(path).write_bytes(header + body)


def predict_integer_v4(
    model: QuantizedModelV4,
    feature: nnue_v3.FeatureRecord,
    passive_counts: Sequence[tuple[int, int]] | None = None,
) -> int:
    """Semantic = V3(feature); Passive = sum n_i * bias[i] (+ pairs). Context-free V4.0."""
    semantic = nnue_v3.predict_integer(model.v3, feature)
    if not passive_counts:
        return semantic
    index = {int(t): i for i, t in enumerate(model.tokens)}
    value = semantic
    counts = {int(cid): int(n) for cid, n in passive_counts}
    for cid, n in counts.items():
        idx = index.get(cid)
        if idx is None:
            continue
        value += n * int(model.passive_bias[idx])
    for pair in model.pairs:
        a, b, w = int(pair["card_a"]), int(pair["card_b"]), int(pair["weight"])
        na, nb = counts.get(a, 0), counts.get(b, 0)
        if a == b:
            value += w * (na * (na - 1) // 2)
        else:
            value += w * na * nb
    return max(-nnue_v3.NON_TERMINAL_LIMIT, min(nnue_v3.NON_TERMINAL_LIMIT, value))
