#!/usr/bin/env python3
"""
Test corpus fetcher for compression benchmarking.
Downloads and unpacks standard compression corpora.
"""

import argparse
import json
import os
import sys
import hashlib
import zipfile
import tempfile
from pathlib import Path
from urllib.request import urlopen
from urllib.error import URLError

# Corpus definitions
CORPORA = {
    "silesia": {
        "urls": [
            "http://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip",
            "https://mattmahoney.net/dc/silesia.zip",
        ],
        "output_dir": "silesia",
    },
    "cantrbry": {
        "urls": ["https://corpus.canterbury.ac.nz/resources/cantrbry.zip"],
        "output_dir": "cantrbry",
    },
    "enwik8": {
        "urls": ["https://mattmahoney.net/dc/enwik8.zip"],
        "output_dir": "enwik8",
    },
}


def get_corpus_dir():
    """Get the corpus data directory."""
    return Path(__file__).parent.parent / "corpus" / "data"


def get_manifest_path():
    """Get the manifest.json path."""
    return Path(__file__).parent.parent / "corpus" / "manifest.json"


def ensure_corpus_dir():
    """Ensure the corpus data directory exists."""
    corpus_dir = get_corpus_dir()
    corpus_dir.mkdir(parents=True, exist_ok=True)
    return corpus_dir


def load_manifest():
    """Load the existing manifest or return empty list."""
    manifest_path = get_manifest_path()
    if manifest_path.exists():
        try:
            with open(manifest_path, "r") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            return []
    return []


def save_manifest(manifest):
    """Save manifest to manifest.json with deterministic formatting."""
    manifest_path = get_manifest_path()
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    # Sort by file path for deterministic output
    sorted_manifest = sorted(manifest, key=lambda x: x["file"])

    with open(manifest_path, "w") as f:
        json.dump(sorted_manifest, f, indent=2)
        f.write("\n")  # Add trailing newline


def sha256_file(filepath):
    """Compute SHA256 hash of a file."""
    sha256 = hashlib.sha256()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            sha256.update(chunk)
    return sha256.hexdigest()


def download_corpus(corpus_name, urls):
    """Download a corpus from one of its URLs."""
    for url in urls:
        try:
            print(f"Downloading {corpus_name} from {url}...", file=sys.stderr)
            with urlopen(url, timeout=60) as response:
                data = response.read()
                print(f"Downloaded {len(data)} bytes", file=sys.stderr)
                return data
        except (URLError, OSError) as e:
            print(f"Failed to download from {url}: {e}", file=sys.stderr)
            continue

    raise RuntimeError(f"Failed to download {corpus_name} from all URLs")


def extract_zip_corpus(corpus_name, output_dir, archive_data):
    """Extract a ZIP corpus to the appropriate directory."""
    # Write to temp file
    with tempfile.NamedTemporaryFile(delete=False, suffix=".zip") as tmp:
        tmp.write(archive_data)
        tmp_path = tmp.name

    try:
        with zipfile.ZipFile(tmp_path, "r") as zf:
            zf.extractall(output_dir)
        print(f"Extracted {corpus_name} to {output_dir}", file=sys.stderr)
    finally:
        os.unlink(tmp_path)

    # Check if we need to flatten (single top-level directory)
    items = list(output_dir.iterdir())
    if len(items) == 1 and items[0].is_dir():
        # Check if the single directory is the corpus itself (not a common case, but handle it)
        # For most corpora, we want to keep the structure as-is
        # Only flatten if the single dir has the corpus name
        if items[0].name == corpus_name:
            print(f"Flattening single top-level directory", file=sys.stderr)
            # Move contents up one level
            subdir = items[0]
            for item in subdir.iterdir():
                if item.is_file():
                    item.rename(output_dir / item.name)
                else:
                    item.rename(output_dir / item.name)
            subdir.rmdir()


def compute_corpus_checksums(corpus_name):
    """Compute checksums for all files in an extracted corpus."""
    corpus_dir = get_corpus_dir()
    output_dir = corpus_dir / CORPORA[corpus_name]["output_dir"]

    entries = []

    if not output_dir.exists():
        return entries

    # Walk through all files and compute checksums
    for root, dirs, files in os.walk(output_dir):
        for filename in sorted(files):
            filepath = Path(root) / filename
            # Get relative path from corpus/data/
            rel_path = filepath.relative_to(corpus_dir)

            sha256_hash = sha256_file(filepath)
            file_size = filepath.stat().st_size

            entries.append({
                "corpus": corpus_name,
                "file": str(rel_path),
                "bytes": file_size,
                "sha256": sha256_hash,
            })

    return entries


def is_corpus_present(corpus_name):
    """Check if a corpus is already extracted."""
    corpus_dir = get_corpus_dir()
    output_dir = corpus_dir / CORPORA[corpus_name]["output_dir"]
    return output_dir.exists() and any(output_dir.iterdir())


def get_corpus_size(corpus_name):
    """Get the total size of an extracted corpus in bytes."""
    corpus_dir = get_corpus_dir()
    output_dir = corpus_dir / CORPORA[corpus_name]["output_dir"]

    total = 0
    if output_dir.exists():
        for root, dirs, files in os.walk(output_dir):
            for filename in files:
                filepath = Path(root) / filename
                total += filepath.stat().st_size

    return total


def fetch_corpus(corpus_name):
    """Fetch and extract a single corpus."""
    corpus_info = CORPORA[corpus_name]

    if is_corpus_present(corpus_name):
        print(f"{corpus_name}: already present, skipping download", file=sys.stderr)
        return compute_corpus_checksums(corpus_name)

    # Ensure output directory exists
    corpus_dir = ensure_corpus_dir()
    output_dir = corpus_dir / corpus_info["output_dir"]
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Fetching {corpus_name}...", file=sys.stderr)

    try:
        archive_data = download_corpus(corpus_name, corpus_info["urls"])
        extract_zip_corpus(corpus_name, output_dir, archive_data)
        checksums = compute_corpus_checksums(corpus_name)
        print(f"Computed checksums for {len(checksums)} files in {corpus_name}", file=sys.stderr)
        return checksums
    except Exception as e:
        print(f"Error fetching {corpus_name}: {e}", file=sys.stderr)
        # Clean up partial extraction
        if output_dir.exists():
            try:
                for item in output_dir.iterdir():
                    if item.is_file():
                        item.unlink()
                    elif item.is_dir():
                        import shutil
                        shutil.rmtree(item)
            except Exception:
                pass
        raise


def format_bytes(n):
    """Format bytes to human-readable size."""
    for unit in ["B", "KB", "MB", "GB"]:
        if n < 1024:
            if n == int(n):
                return f"{int(n)}{unit}"
            return f"{n:.1f}{unit}"
        n /= 1024
    if n == int(n):
        return f"{int(n)}TB"
    return f"{n:.1f}TB"


def list_corpora(selected_corpora=None):
    """List status of corpora."""
    if selected_corpora is None:
        selected_corpora = CORPORA

    for corpus_name in sorted(selected_corpora.keys()):
        if is_corpus_present(corpus_name):
            size = get_corpus_size(corpus_name)
            file_count = 0
            output_dir = get_corpus_dir() / CORPORA[corpus_name]["output_dir"]
            for root, dirs, files in os.walk(output_dir):
                file_count += len(files)
            print(f"{corpus_name}: present ({file_count} files, {format_bytes(size)})")
        else:
            print(f"{corpus_name}: missing")


def main():
    parser = argparse.ArgumentParser(
        description="Fetch and extract standard compression corpora"
    )
    parser.add_argument(
        "--only",
        help="Comma-separated list of corpora to fetch (e.g., silesia,enwik8)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List status of corpora without downloading",
    )

    args = parser.parse_args()

    # Determine which corpora to process
    if args.only:
        corpus_names = [c.strip() for c in args.only.split(",")]
        selected_corpora = {}
        for name in corpus_names:
            if name not in CORPORA:
                print(f"Error: Unknown corpus: {name}", file=sys.stderr)
                sys.exit(1)
            selected_corpora[name] = CORPORA[name]
    else:
        selected_corpora = CORPORA

    if args.list:
        list_corpora(selected_corpora)
        return 0

    # Fetch corpora and refresh their manifest entries, preserving entries
    # for corpora not selected this run.
    all_checksums = [
        e for e in load_manifest() if e.get("corpus") not in selected_corpora
    ]

    for corpus_name in sorted(selected_corpora.keys()):
        try:
            checksums = fetch_corpus(corpus_name)
            all_checksums.extend(checksums)
        except Exception as e:
            print(f"Failed to fetch {corpus_name}: {e}", file=sys.stderr)
            sys.exit(1)

    # Save manifest
    save_manifest(all_checksums)
    print(f"Manifest saved to {get_manifest_path()}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
