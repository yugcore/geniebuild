#!/usr/bin/env python3
"""Generate synthetic N-TU C++ benchmark projects for GenieBuild scale testing."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate N-TU C++ scale benchmark project")
    parser.add_argument("--out", default="benchmark_project", help="Output directory")
    parser.add_argument("--tus", type=int, default=50000, help="Number of .cpp translation units")
    parser.add_argument("--headers", type=int, default=2000, help="Number of shared headers")
    parser.add_argument("--fanout", type=int, default=4, help="Headers included per TU")
    args = parser.parse_args()

    root = Path(args.out)
    inc_dir = root / "include"
    src_dir = root / "src"
    gb_dir = root / ".geniebuild"
    inc_dir.mkdir(parents=True, exist_ok=True)
    src_dir.mkdir(parents=True, exist_ok=True)
    gb_dir.mkdir(parents=True, exist_ok=True)

    for h in range(args.headers):
        bucket = inc_dir / f"h{h // 500:02d}"
        bucket.mkdir(parents=True, exist_ok=True)
        (bucket / f"hdr_{h:04d}.h").write_text(
            f"#pragma once\ninline int gb_val_{h}() {{ return {h}; }}\n",
            encoding="utf-8",
        )

    for i in range(args.tus):
        bucket = src_dir / f"s{i // 500:03d}"
        bucket.mkdir(parents=True, exist_ok=True)
        incs = []
        calls = []
        for k in range(max(1, args.fanout)):
            hid = (i * max(1, args.fanout) + k) % max(1, args.headers)
            incs.append(f'#include "h{hid // 500:02d}/hdr_{hid:04d}.h"')
            calls.append(f"gb_val_{hid}()")
        body = "\n".join(incs) + f"\nint tu_fn_{i}() {{ return {' + '.join(calls)}; }}\n"
        (bucket / f"tu_{i:05d}.cpp").write_text(body, encoding="utf-8")

    (src_dir / "main.cpp").write_text(
        "int tu_fn_0();\nint main() { return tu_fn_0() >= 0 ? 0 : 1; }\n",
        encoding="utf-8",
    )

    proj = {
        "version": 2,
        "startupTarget": "bench_app",
        "cxxStd": "c++17",
        "warnings": "-Wall",
        "includeDirs": ["include"],
        "targets": [
            {
                "name": "bench_app",
                "kind": "executable",
                "sources": [{"include": "src/**/*.cpp"}],
            }
        ],
    }
    (gb_dir / "project.json").write_text(json.dumps(proj, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {args.tus} TUs + {args.headers} headers at {root}")


if __name__ == "__main__":
    main()
