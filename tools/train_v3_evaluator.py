"""Train the quantized V3 entity/global network on replay feature records."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset


ROOT = Path(__file__).resolve().parents[1]
SUBMISSION = ROOT / "sample_submission" / "sample_submission"
sys.path.insert(0, str(SUBMISSION))

from exact_solver.nnue_v3 import (  # noqa: E402
    BELIEF_SCALE,
    ENTITY_DENSE,
    ENTITY_HIDDEN,
    ENTITY_RELATIONS,
    GLOBAL_DENSE,
    GLOBAL_HIDDEN,
    GLOBAL_RELATIONS,
    POOLS,
    QuantizedModel,
    WEIGHT_SCALE,
    export_quantized,
    load_quantized,
    predict_integer_many,
    EntityFeatures,
    FeatureRecord,
)


@dataclass
class Example:
    date: str
    replay: str
    dense: list[int]
    sparse: list[list[int]]
    entities: list[dict]
    target: float
    weight: float


def read_examples(path: Path, sample_kind: str | None) -> list[Example]:
    result = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            raw = json.loads(line)
            if sample_kind is not None and raw.get("sampleKind") != sample_kind:
                continue
            if raw.get("overflow"):
                continue
            result.append(Example(
                str(raw["date"]), str(raw["replayId"]),
                raw["globalDense"], raw["globalSparse"], raw["entities"],
                float(raw["reward"]), float(raw["matchWeight"]),
            ))
    if not result:
        raise ValueError("dataset has no non-overflow examples")
    return result


def split_matches(examples: list[Example]):
    matches = sorted({(example.date, example.replay) for example in examples})
    train_end = max(1, int(len(matches) * 0.8))
    validation_end = max(train_end + 1, int(len(matches) * 0.9))
    validation_end = min(validation_end, len(matches))
    membership = {}
    for index, match in enumerate(matches):
        membership[match] = 0 if index < train_end else (1 if index < validation_end else 2)
    split = [[], [], []]
    for example in examples:
        split[membership[(example.date, example.replay)]].append(example)
    return split


class ReplayDataset(Dataset):
    def __init__(self, examples):
        self.examples = examples

    def __len__(self):
        return len(self.examples)

    def __getitem__(self, index):
        return self.examples[index]


def collate(examples: list[Example], token_index: dict[int, int]):
    batch = len(examples)
    global_dense = torch.tensor([item.dense for item in examples], dtype=torch.float32)
    global_indices, global_weights, global_offsets = [], [], [0]
    entity_dense, entity_pool, entity_batch = [], [], []
    entity_indices, entity_weights, entity_offsets = [], [], [0]
    for batch_index, item in enumerate(examples):
        for relation, token, value in item.sparse:
            global_indices.append(int(relation) * len(token_index)
                                  + token_index.get(int(token), 0))
            global_weights.append(float(value))
        global_offsets.append(len(global_indices))
        for entity in item.entities:
            entity_dense.append(entity["dense"])
            entity_pool.append(int(entity["pool"]))
            entity_batch.append(batch_index)
            for relation, token, value in entity["sparse"]:
                entity_indices.append(int(relation) * len(token_index)
                                      + token_index.get(int(token), 0))
                entity_weights.append(float(value))
            entity_offsets.append(len(entity_indices))
    tensors = {
        "global_dense": global_dense,
        "global_indices": torch.tensor(global_indices or [0], dtype=torch.long),
        "global_weights": torch.tensor(global_weights or [0.0], dtype=torch.float32),
        "global_offsets": torch.tensor(global_offsets, dtype=torch.long),
        "entity_dense": torch.tensor(
            entity_dense or [[0] * ENTITY_DENSE], dtype=torch.float32),
        "entity_pool": torch.tensor(entity_pool or [0], dtype=torch.long),
        "entity_batch": torch.tensor(entity_batch or [0], dtype=torch.long),
        "entity_indices": torch.tensor(entity_indices or [0], dtype=torch.long),
        "entity_weights": torch.tensor(entity_weights or [0.0], dtype=torch.float32),
        "entity_offsets": torch.tensor(
            entity_offsets if entity_dense else [0, 1], dtype=torch.long),
        "real_entities": len(entity_dense),
        "target": torch.tensor([item.target for item in examples], dtype=torch.float32),
        "weight": torch.tensor([item.weight for item in examples], dtype=torch.float32),
        "batch": batch,
    }
    return tensors


class V3Network(nn.Module):
    def __init__(self, base: QuantizedModel):
        super().__init__()
        token_count = len(base.tokens)
        self.token_count = token_count
        self.entity_dense = nn.Linear(ENTITY_DENSE, ENTITY_HIDDEN, bias=True)
        self.entity_sparse = nn.EmbeddingBag(
            ENTITY_RELATIONS * token_count, ENTITY_HIDDEN,
            mode="sum", include_last_offset=True)
        self.global_dense = nn.Linear(GLOBAL_DENSE, GLOBAL_HIDDEN, bias=True)
        self.global_sparse = nn.EmbeddingBag(
            GLOBAL_RELATIONS * token_count, GLOBAL_HIDDEN,
            mode="sum", include_last_offset=True)
        self.pool = nn.Parameter(torch.empty(POOLS, ENTITY_HIDDEN, GLOBAL_HIDDEN))
        self.output = nn.Linear(GLOBAL_HIDDEN, 1, bias=True)
        with torch.no_grad():
            self.entity_dense.weight.copy_(
                torch.from_numpy(base.entity_dense_weight.astype(np.float32) / WEIGHT_SCALE))
            self.entity_dense.bias.copy_(
                torch.from_numpy(base.entity_bias.astype(np.float32) / WEIGHT_SCALE))
            self.entity_sparse.weight.copy_(torch.from_numpy(
                base.entity_sparse_weight.reshape(-1, ENTITY_HIDDEN).astype(np.float32)
                / WEIGHT_SCALE))
            self.global_dense.weight.copy_(
                torch.from_numpy(base.global_dense_weight.astype(np.float32) / WEIGHT_SCALE))
            self.global_dense.bias.copy_(
                torch.from_numpy(base.global_bias.astype(np.float32) / WEIGHT_SCALE))
            self.global_sparse.weight.copy_(torch.from_numpy(
                base.global_sparse_weight.reshape(-1, GLOBAL_HIDDEN).astype(np.float32)
                / WEIGHT_SCALE))
            self.pool.copy_(torch.from_numpy(base.pool_weight.astype(np.float32) / WEIGHT_SCALE))
            self.output.weight.copy_(
                torch.from_numpy(base.output_weight.astype(np.float32)[None, :] / WEIGHT_SCALE))
            self.output.bias.fill_(float(base.output_bias) / (WEIGHT_SCALE * WEIGHT_SCALE))

    def forward(self, batch):
        device = self.pool.device
        value = self.global_dense(batch["global_dense"].to(device))
        value = value + self.global_sparse(
            batch["global_indices"].to(device),
            batch["global_offsets"].to(device),
            per_sample_weights=batch["global_weights"].to(device))
        if batch["real_entities"]:
            entity = self.entity_dense(batch["entity_dense"].to(device))
            entity = entity + self.entity_sparse(
                batch["entity_indices"].to(device),
                batch["entity_offsets"].to(device),
                per_sample_weights=batch["entity_weights"].to(device))
            entity = torch.clamp(torch.relu(entity), max=127.0)
            selected_pool = self.pool[batch["entity_pool"].to(device)]
            projection = torch.bmm(entity.unsqueeze(1), selected_pool).squeeze(1)
            pooled = torch.zeros(
                batch["batch"], GLOBAL_HIDDEN, device=device, dtype=value.dtype)
            pooled.index_add_(0, batch["entity_batch"].to(device), projection)
            value = value + pooled
        value = torch.clamp(torch.relu(value), max=127.0)
        # Do not clamp the training output. The boundary model can saturate on
        # previously unseen intermediate states; clamping here would give those
        # examples zero gradient and make transfer learning unable to recover.
        return self.output(value).squeeze(1)


def quantize(network: V3Network, base: QuantizedModel, dataset_hash: bytes):
    def q16(tensor):
        return np.clip(np.rint(tensor.detach().cpu().numpy() * WEIGHT_SCALE),
                       -32768, 32767).astype(np.int16)

    def q_sparse(tensor):
        # Sparse inputs can be as large as Q8 card counts/effect payloads. The
        # audited native int32 bound assumes these embeddings remain tiny.
        return np.clip(np.rint(tensor.detach().cpu().numpy() * WEIGHT_SCALE),
                       -4, 4).astype(np.int16)

    def q32(tensor):
        return np.clip(np.rint(tensor.detach().cpu().numpy() * WEIGHT_SCALE),
                       -(1 << 31), (1 << 31) - 1).astype(np.int32)

    result = QuantizedModel(
        base.tokens.copy(),
        q16(network.entity_dense.weight),
        q_sparse(network.entity_sparse.weight).reshape(
            ENTITY_RELATIONS, len(base.tokens), ENTITY_HIDDEN),
        q32(network.entity_dense.bias),
        q16(network.global_dense.weight),
        q_sparse(network.global_sparse.weight).reshape(
            GLOBAL_RELATIONS, len(base.tokens), GLOBAL_HIDDEN),
        q16(network.pool),
        q32(network.global_dense.bias),
        q16(network.output.weight[0]),
        int(round(float(network.output.bias.detach().cpu()[0])
                  * WEIGHT_SCALE * WEIGHT_SCALE)),
        dataset_hash,
        base.card_checksum,
        base.effect_checksum,
        base.combo_checksum,
    )
    result.validate()
    return result


def metrics(network, loader, device):
    network.eval()
    squared = weighted = correct = count = 0.0
    with torch.no_grad():
        for batch in loader:
            prediction = network(batch)
            target = batch["target"].to(device)
            weight = batch["weight"].to(device)
            squared += float(((prediction - target).square() * weight).sum())
            weighted += float(weight.sum())
            correct += float(((prediction >= 0) == (target >= 0)).sum())
            count += len(target)
    return {"mse": squared / max(weighted, 1e-12),
            "signAccuracy": correct / max(count, 1)}


def to_feature(example: Example) -> FeatureRecord:
    return FeatureRecord(
        example.dense,
        example.sparse,
        [EntityFeatures(item["pool"], item["dense"], item["sparse"])
         for item in example.entities],
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--initial-model", type=Path, required=True)
    parser.add_argument("--boundary-only", action="store_true")
    parser.add_argument("--sample-kind", choices=("boundary", "intermediate"))
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    parser.add_argument("--seed", type=int, default=20260720)
    args = parser.parse_args()
    random.seed(args.seed); np.random.seed(args.seed); torch.manual_seed(args.seed)
    torch.backends.cudnn.deterministic = True
    examples = read_examples(args.dataset, args.sample_kind)
    if args.boundary_only != (args.sample_kind == "boundary"):
        raise ValueError("--boundary-only must be set exactly for boundary samples")
    train, validation, test = split_matches(examples)
    base = load_quantized(args.initial_model)
    token_index = {int(token): index for index, token in enumerate(base.tokens)}
    collator = lambda values: collate(values, token_index)
    loaders = [
        DataLoader(ReplayDataset(values), batch_size=args.batch_size,
                   shuffle=index == 0, collate_fn=collator, num_workers=0)
        for index, values in enumerate((train, validation, test))
    ]
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    network = V3Network(base).to(device)
    optimizer = torch.optim.AdamW(network.parameters(), lr=args.learning_rate,
                                  weight_decay=1e-5)
    best_state = None; best_epoch = 0; best_validation = math.inf; history = []
    for epoch in range(1, args.epochs + 1):
        network.train()
        for batch in loaders[0]:
            optimizer.zero_grad(set_to_none=True)
            prediction = network(batch)
            target = batch["target"].to(device)
            weight = batch["weight"].to(device)
            loss = ((prediction - target).square() * weight).sum() / weight.sum()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(network.parameters(), 5.0)
            optimizer.step()
        validation_metrics = metrics(network, loaders[1], device)
        history.append({"epoch": epoch, **validation_metrics})
        print(f"epoch={epoch} validation={validation_metrics}", flush=True)
        if validation_metrics["mse"] < best_validation:
            best_validation = validation_metrics["mse"]
            best_epoch = epoch
            best_state = {
                key: value.detach().cpu().clone()
                for key, value in network.state_dict().items()
            }
    network.load_state_dict(best_state)
    dataset_hash = hashlib.sha256(args.dataset.read_bytes()).digest()
    model = quantize(network, base, dataset_hash)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    export_quantized(args.output, model, boundary_only=args.boundary_only)
    float_test = metrics(network, loaders[2], device)
    quantized_predictions = np.asarray(predict_integer_many(
        model, [to_feature(example) for example in test]), dtype=np.float64) / 100_000_000
    baseline_predictions = np.asarray(predict_integer_many(
        base, [to_feature(example) for example in test]), dtype=np.float64) / 100_000_000
    targets = np.asarray([example.target for example in test])
    weights = np.asarray([example.weight for example in test])
    quantized_mse = float(np.sum((quantized_predictions - targets) ** 2 * weights)
                          / np.sum(weights))
    quantized_sign = float(np.mean((quantized_predictions >= 0) == (targets >= 0)))
    baseline_mse = float(np.sum((baseline_predictions - targets) ** 2 * weights)
                         / np.sum(weights))
    baseline_sign = float(np.mean((baseline_predictions >= 0) == (targets >= 0)))
    report = {
        "dataset": str(args.dataset.resolve()),
        "datasetSha256": dataset_hash.hex(),
        "initialModel": str(args.initial_model.resolve()),
        "output": str(args.output.resolve()),
        "boundaryOnly": args.boundary_only,
        "informationSetSafe": True,
        "device": str(device),
        "epochs": args.epochs,
        "bestEpoch": best_epoch,
        "examples": len(examples),
        "splitExamples": {
            "train": len(train), "validation": len(validation), "test": len(test)},
        "validationHistory": history,
        "floatTest": float_test,
        "quantizedTest": {
            "mse": quantized_mse, "signAccuracy": quantized_sign},
        "initialModelTest": {
            "mse": baseline_mse, "signAccuracy": baseline_sign},
        "improvesInitialMse": quantized_mse < baseline_mse,
        "zeroBaselineMse": 1.0,
        "modelSha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
        "modelBytes": args.output.stat().st_size,
    }
    report_path = args.output.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
