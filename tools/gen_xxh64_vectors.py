#!/usr/bin/env python3
"""Regenerate the XXH64 reference vectors in tests/test_xxh64.cpp.

Uses the official xxHash bindings (python-xxhash) as ground truth.
"""
import xxhash

PAT = bytes((i * 167 + 13) % 251 for i in range(1024))

CASES = [
    (b"", 0),
    (b"", 0x9E3779B97F4A7C15),
    (b"a", 0),
    (b"abc", 0),
    (b"abc", 42),
    (b"Hello, world!", 0),
    (PAT[:31], 0),
    (PAT[:32], 0),
    (PAT[:33], 0),
    (PAT[:63], 7),
    (PAT[:64], 7),
    (PAT, 0),
    (PAT, 0xDEADBEEF),
]


def main() -> None:
    for data, seed in CASES:
        digest = xxhash.xxh64(data, seed=seed).intdigest()
        print(f"len={len(data):4d} seed=0x{seed:X} -> 0x{digest:016X}ULL")


if __name__ == "__main__":
    main()
