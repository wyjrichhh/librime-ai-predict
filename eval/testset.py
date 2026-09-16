#!/usr/bin/env python3
"""Extract a real test set of (window_text, pinyin prompt, committed hanzi)
triples from the plugin's inference logs.

Ground truth: when the window grows (the user committed a segment), the appended
delta is what the user actually wanted for the prompt they had just typed. So a
triple (window_before, final_prompt_before, delta) is a labelled example:
  model(window_before, prompt) SHOULD produce delta.

Reuses scripts/analyze_predictions.py's parser. Output is a gitignored JSONL
(it contains real input data -- see the repo's "eval scripts/data not open
sourced" decision).
"""
from __future__ import annotations

import argparse
import json
import os
import sys

_SCRIPTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts")
sys.path.insert(0, _SCRIPTS)

import analyze_predictions as ap  # noqa: E402


def extract(events: list[ap.Event]) -> list[dict]:
    """Replay events; for every window transition (commit) record the triple.

    prompt is the LAST pinyin seen for the pre-commit window (the most complete
    fragment before the user committed)."""
    triples: list[dict] = []
    cur_window = ""
    last_prompt_for_window: dict[str, str] = {}

    for ev in events:
        if ev.kind == "ctor":
            cur_window = ""
            last_prompt_for_window.clear()
            continue
        if ev.kind == "built":
            new_window = ev.window
            last_prompt_for_window[new_window] = ev.prompt
            if new_window != cur_window:
                delta = ap.committed_delta(cur_window, new_window)
                if delta is not None and delta:
                    triples.append({
                        "window": cur_window,
                        "prompt": last_prompt_for_window.get(cur_window, ""),
                        "commit": delta,
                    })
                cur_window = new_window
            continue
    return triples


def _is_pinyin(s: str) -> bool:
    return bool(s) and all(c.islower() and "a" <= c <= "z" or c == "'" for c in s)


def main(argv: list[str]) -> int:
    ap_ = argparse.ArgumentParser()
    ap_.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "testset.jsonl"))
    ap_.add_argument("--min-prompt", type=int, default=1, help="min pinyin letters")
    ap_.add_argument("--max-prompt", type=int, default=0, help="max pinyin letters (0=no cap); use to focus short words")
    ap_.add_argument("--require-context", action="store_true", help="only windowed (non-empty window)")
    ap_.add_argument("files", nargs="*")
    args = ap_.parse_args(argv)

    class Args:  # shim for ap.discover_logs
        files = args.files
        log_dir = None
    files = ap.discover_logs(Args)
    if not files:
        print("no logs found", file=sys.stderr)
        return 1

    events = ap.parse_files(files)
    triples = extract(events)

    def keep(t):
        if not _is_pinyin(t["prompt"]):
            return False
        n = len([c for c in t["prompt"] if "a" <= c <= "z"])
        if n < args.min_prompt:
            return False
        if args.max_prompt and n > args.max_prompt:
            return False
        if args.require_context and not t["window"]:
            return False
        return True

    kept = [t for t in triples if keep(t)]
    with open(args.out, "w", encoding="utf-8") as fh:
        for t in kept:
            fh.write(json.dumps(t, ensure_ascii=False) + "\n")

    # brief distribution report
    by_len: dict[int, int] = {}
    for t in kept:
        n = len([c for c in t["prompt"] if "a" <= c <= "z"])
        by_len[n] = by_len.get(n, 0) + 1
    print(f"triples total={len(triples)} kept={len(kept)} -> {args.out}")
    print("prompt-length distribution (letters):")
    for n in sorted(by_len):
        print(f"  {n:3d} letters : {by_len[n]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
