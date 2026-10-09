"""Hybrid handoffs for Qwen3.8 Flash Next: whole-prefix state of its QSA, Gated DeltaNet and PLE layers.

A hybrid model's recurrent state is only defined for a whole prefix, so a hybrid handoff always
covers prompt tokens [0, T) and exports each layer's state at T as named entries in a fixed layout,
whatever vLLM's own layout is (docs/kv-handoff.md, "Hybrid models"):

    qsa  k, v        [T, kv_heads, head_dim]           post-RoPE, as cached
    qsa  index_keys  [T, indexer_head_dim]             raw: pre-norm, pre-RoPE
    gdn  conv        [conv_kernel - 1, conv_dim]       oldest tap first; channels q | k | v
    gdn  ssm         [v_heads, v_dim, k_dim]           float32
    ple  conv        [(kernel - 1) * dilation, hc_dim] oldest tap first

vLLM keeps no raw index keys beyond the last four tokens, so `install_index_key_hook` wraps the
QSA layer's run and copies each prefill chunk's raw keys out by position. That needs one request
in flight at a time: run the producer with --max-num-seqs 1. Every entry is snapshotted into
private tensors on the step that finishes the prefill, so serving never reads live caches.
"""

from __future__ import annotations

import logging
import re
import threading
from dataclasses import dataclass
from typing import Any

import torch

from .export import LayerPages

logger = logging.getLogger(__name__)
_QSA = re.compile(r"layers\.(\d+)\.self_attn\.attn$")
_GDN = re.compile(r"layers\.(\d+)\.linear_attn$")
_PLE = re.compile(r"layers\.(\d+)\.ple$")
_DIMS = {
    ("qsa", "k"): ["block", "head", "head_dim"],
    ("qsa", "v"): ["block", "head", "head_dim"],
    ("qsa", "index_keys"): ["block", "index_dim"],
    ("gdn", "conv"): ["block", "channel"],
    ("gdn", "ssm"): ["block", "v_dim", "k_dim"],
    ("ple", "conv"): ["block", "channel"],
}


def is_hybrid(specs: list[Any]) -> bool:
    return any(type(spec).__name__ == "MambaSpec" for spec in specs)


@dataclass
class HybridPages(LayerPages):
    """One canonical entry: a private tensor whose first dimension is the row axis frames cut."""

    kind: str = ""
    layer: int = -1
    name: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {"index": self.index, "kind": self.kind, "layer": self.layer, "name": self.name,
                "shape": self.shape, "dims": self.dims, "dtype": self.dtype}


class IndexKeyRecorder:
    """Raw QSA index keys by position, per layer, for the one request being prefilled."""

    def __init__(self, max_tokens: int) -> None:
        self.max_tokens = max_tokens
        self._buffers: dict[int, torch.Tensor] = {}
        self._layers: dict[int, int] = {}  # data_ptr of a layer's KV cache -> model layer
        self._lock = threading.Lock()

    def bind(self, kv_caches: dict[str, torch.Tensor]) -> None:
        for name, tensor in kv_caches.items():
            found = _QSA.search(name)
            if found:
                self._layers[tensor.data_ptr()] = int(found.group(1))

    def record(self, module: Any, positions: torch.Tensor, raw_keys: torch.Tensor, rows: int) -> None:
        if rows <= 0 or torch.cuda.is_current_stream_capturing():
            return
        cache = module.kv_cache[0] if isinstance(module.kv_cache, (list, tuple)) else module.kv_cache
        layer = self._layers.get(cache.data_ptr()) if cache is not None and cache.numel() else None
        if layer is None:
            return
        buffer = self._buffers.get(layer)
        if buffer is None:
            buffer = torch.zeros(self.max_tokens, raw_keys.shape[-1], dtype=raw_keys.dtype,
                                 device=raw_keys.device)
            self._buffers[layer] = buffer
        logical = positions[:rows] if positions.dim() == 1 else positions[0, :rows]
        buffer.index_copy_(0, logical.to(torch.long), raw_keys[:rows])

    def keys(self, layer: int, tokens: int) -> torch.Tensor:
        buffer = self._buffers.get(layer)
        if buffer is None:
            raise ValueError(f"no raw index keys were recorded for layer {layer}")
        return buffer[:tokens].clone()


def install_index_key_hook(recorder: IndexKeyRecorder) -> None:
    """Wrap the b12x QSA run so every prefill chunk's raw index keys reach `recorder`."""
    from vllm.models.qwen4_exp.nvidia import b12x_qsa

    cls = b12x_qsa.Qwen4ExpQSAAttention
    original = cls._run_b12x_qsa
    if getattr(original, "_mcdma_recorder", None) is not None:
        original._mcdma_recorder = recorder
        return

    def run(self, *, positions, raw_index_key, rows, **kwargs):
        run._mcdma_recorder.record(self, positions, raw_index_key, int(rows))
        return original(self, positions=positions, raw_index_key=raw_index_key, rows=rows, **kwargs)

    run._mcdma_recorder = recorder
    cls._run_b12x_qsa = run
    logger.info("MCDMA handoff: recording raw QSA index keys for hybrid exports")


def _state_parts(tensor: torch.Tensor, slot: int, spec: Any) -> list[torch.Tensor]:
    """A Mamba-type state page split into its states, as vLLM's bind_kv_cache lays them out."""
    page = tensor[slot].reshape(-1).view(torch.uint8)
    parts, offset = [], 0
    for shape, dtype in zip(spec.shapes, spec.dtypes):
        count = 1
        for size in shape:
            count *= int(size)
        nbytes = count * torch.empty((), dtype=dtype).element_size()
        parts.append(page[offset:offset + nbytes].view(dtype).reshape(tuple(int(s) for s in shape)))
        offset += nbytes
    return parts


def _channel_major(state: torch.Tensor, taps: int) -> torch.Tensor:
    """A conv state as [taps, channels], whichever of vLLM's two layouts holds it."""
    if state.shape[-1] == taps and state.shape[0] != taps:
        return state.transpose(0, 1)
    return state


def export_entries(caches: dict[str, torch.Tensor], groups: dict[str, int], specs: list[Any],
                   block_ids: tuple[tuple[int, ...], ...], tokens: int, recorder: IndexKeyRecorder,
                   config: Any) -> list[HybridPages]:
    """Snapshot every canonical entry of one request's prefix [0, tokens)."""
    conv_taps = int(config.linear_conv_kernel_dim) - 1
    ple_taps = (int(config.ple_conv_kernel_size) - 1) * int(config.ngram_size)
    found: list[tuple[int, str, str, torch.Tensor]] = []
    for name, tensor in caches.items():
        group = groups.get(name)
        if group is None:
            continue
        spec, ids = specs[group], block_ids[group]
        if (match := _QSA.search(name)) is not None:
            layer = int(match.group(1))
            heads, head_size = int(spec.num_kv_heads), int(spec.head_size)
            # [num_blocks, 2 (K, V), block_size, heads * head_size]
            blocks = torch.tensor(ids[: -(-tokens // spec.block_size)], device=tensor.device)
            pages = tensor.index_select(0, blocks)
            for part, label in ((0, "k"), (1, "v")):
                rows = pages[:, part].reshape(-1, heads, head_size)[:tokens]
                found.append((layer, "qsa", label, rows.clone()))
            found.append((layer, "qsa", "index_keys", recorder.keys(layer, tokens)))
        elif (match := _GDN.search(name)) is not None:
            conv, ssm = _state_parts(tensor, ids[0], spec)[:2]
            found.append((int(match.group(1)), "gdn", "conv", _channel_major(conv, conv_taps).clone()))
            found.append((int(match.group(1)), "gdn", "ssm", ssm.to(torch.float32).clone()))
        elif (match := _PLE.search(name)) is not None:
            conv = _state_parts(tensor, ids[0], spec)[0]
            found.append((int(match.group(1)), "ple", "conv", _channel_major(conv, ple_taps).clone()))
    entries = []
    for position, (layer, kind, label, tensor) in enumerate(sorted(found, key=lambda e: (e[0], e[1], e[2]))):
        entries.append(HybridPages(
            index=position, tensor=tensor.contiguous(), block_dim=0, rows=list(range(tensor.shape[0])),
            dims=_DIMS[(kind, label)], dtype=str(tensor.dtype).removeprefix("torch."),
            kind=kind, layer=layer, name=label))
    return entries
