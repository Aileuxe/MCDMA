#!/usr/bin/env python3
"""Check the upstream revision and protocol files before building the RPC proxy peers."""

import argparse
import hashlib
import subprocess
from pathlib import Path

from mcdma_rpc_proxy import LLAMA_COMMIT


HASHES = {
    "ggml/src/ggml-rpc/ggml-rpc.cpp": "812494f207b78e6af7454bd8042ce582850b1c91733db4144e6297ba940b66bd",
    "ggml/src/ggml-rpc/transport.h": "fec7abf4e6cebec0d20e3350c01f2f47d495f90a79c6d829870e09d8a7ef2219",
    "ggml/include/ggml-rpc.h": "196d2a7434f0abdb01f13c5360e0b118b9f2d66c6452e3a7a844fbecc890205c",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
    if revision != LLAMA_COMMIT:
        parser.error(f"expected llama.cpp revision {LLAMA_COMMIT}")
    for relative, expected in HASHES.items():
        actual = hashlib.sha256((args.source / relative).read_bytes()).hexdigest()
        if actual != expected:
            parser.error(f"protocol source differs from the pin: {relative}")
    print(f"verified llama.cpp protocol source at {LLAMA_COMMIT}; build provenance remains the caller's responsibility")


if __name__ == "__main__":
    main()
