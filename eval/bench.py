#!/usr/bin/env python3
"""Benchmark decoding strategies on the extracted (window, prompt, commit) test
set. Primary metric: top-1 exact hit -- the model's best displayable prediction
equals what the user actually committed. Also reports top-K containment.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from infer import Infer, InferenceOptions, byte_level_decode, _is_punct_codepoint  # noqa: E402


def load_testset(path: str) -> list[dict]:
    return [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]


def build_punct_suppress(model_dir: Path) -> list[list[str]]:
    """Suppress every vocab token whose byte-level decode is pure punctuation."""
    vocab = json.load(open(model_dir / "shared_vocabulary.json"))
    return [[t] for t in vocab
            if not (t.startswith("<") and t.endswith(">"))
            and (d := byte_level_decode(t))
            and all(_is_punct_codepoint(ord(c)) for c in d)]


def _han_only(s: str) -> str:
    return "".join(c for c in s if "一" <= c <= "鿿")


def distinct_displayable(pred) -> list[str]:
    """Dedupe hypotheses by display text, dropping non-displayable (Latin/control)."""
    seen: list[str] = []
    for h in pred.hypotheses:
        d = h.display
        if not d or any(("a" <= c <= "z") or ("A" <= c <= "Z") for c in d):
            continue
        if d not in seen:
            seen.append(d)
    return seen


def run(inf: Infer, tests: list[dict], opt: InferenceOptions,
        suppress: list[list[str]] | None, top_k: int) -> dict:
    n = len(tests)
    hit1 = hitk = 0
    t0 = time.time()
    for t in tests:
        src = f"{t['window']}<pinyin_start>{t['prompt']}</pinyin_start>"
        pred = inf.predict(src, opt, suppress_sequences=suppress)
        tops = distinct_displayable(pred)
        target = _han_only(t["commit"])
        if not target:
            continue
        if tops and tops[0] == target:
            hit1 += 1
        if target in tops[:top_k]:
            hitk += 1
    dt = time.time() - t0
    return {
        "n": n, "top1_hit": hit1, f"top{top_k}_hit": hitk,
        "top1_rate": hit1 / n, f"top{top_k}_rate": hitk / n,
        "secs": dt, "ms_per_case": dt / n * 1000,
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--testset", default=str(Path(__file__).resolve().parent / "testset.jsonl"))
    ap.add_argument("--model", default=str(Path.home() / "Library/Rime/predict_models/zh-base-ct2-int8"))
    ap.add_argument("--max-prompt", type=int, default=0)
    ap.add_argument("--min-prompt", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--beam", type=int, default=1)
    ap.add_argument("--num-hyp", type=int, default=1)
    ap.add_argument("--top-k", type=int, default=1)
    ap.add_argument("--suppress-punct", action="store_true")
    ap.add_argument("--length-penalty", type=float, default=1.0)
    ap.add_argument("--repetition-penalty", type=float, default=1.2)
    ap.add_argument("--no-repeat-ngram", type=int, default=0)
    args = ap.parse_args(argv)

    tests = load_testset(args.testset)
    tests = [t for t in tests if t["prompt"] and t["commit"]]
    if args.max_prompt:
        tests = [t for t in tests if len([c for c in t["prompt"] if "a" <= c <= "z"]) <= args.max_prompt]
    if args.min_prompt > 1:
        tests = [t for t in tests if len([c for c in t["prompt"] if "a" <= c <= "z"]) >= args.min_prompt]
    if args.limit:
        tests = tests[: args.limit]

    inf = Infer(Path(args.model))
    suppress = build_punct_suppress(Path(args.model)) if args.suppress_punct else None
    opt = InferenceOptions(
        beam_size=args.beam, num_hypotheses=max(args.num_hyp, args.top_k),
        length_penalty=args.length_penalty, repetition_penalty=args.repetition_penalty,
        no_repeat_ngram_size=args.no_repeat_ngram, sampling_topk=1,
    )
    res = run(inf, tests, opt, suppress, args.top_k)
    print(f"n={res['n']} beam={args.beam} n_hyp={args.num_hyp} top_k={args.top_k} "
          f"suppress_punct={args.suppress_punct}")
    print(f"  top1 exact hit : {res['top1_hit']}/{res['n']} = {res['top1_rate']*100:.1f}%")
    print(f"  top{args.top_k} contains: {res[f'top{args.top_k}_hit']}/{res['n']} = {res[f'top{args.top_k}_rate']*100:.1f}%")
    print(f"  {res['ms_per_case']:.1f} ms/case ({res['secs']:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
