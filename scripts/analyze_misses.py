#!/usr/bin/env python3
"""Attribute WHY value-add AI offers (inserted/promoted) go UNSELECTED.

Reuses analyze_predictions' parser, then for every value-add offer bound to a
pre-commit window it reconstructs the tuple (window, pinyin prompt, AI display
text, the user's actual committed delta) and classifies the failure. The point
is to separate ENGINEERING-fixable misses from MODEL-quality misses, since we
can only move the former without retraining.

Failure taxonomy (checked in order; first match wins):
  latin_residue   AI text still contains [A-Za-z] -- the prompt was a half-typed
                  syllable ("qizh", "wul") or an English word ("feature",
                  "memory") and the model echoed the untypable part back in caps.
                  ENGINEERING: such a candidate is never valid Chinese; suppress.
  single_letter   prompt is exactly one letter. Too little signal; the model can
                  only guess the next character. ENGINEERING: gate these.
  ai_prefix       AI text is a Hanzi prefix of what the user typed (AI stopped
                  short). Borderline -- more tokens might have helped.
  ai_overshoot    AI text Hanzi-extends past what the user committed.
  homophone       same Hanzi length, different characters -- a same/near-sound
                  miss. MODEL: needs a better model, not engineering.
  semantic        everything else -- the model went somewhere unrelated. MODEL.

Run with no args to print the full attribution table + suppression projection.
Pass --dump to also print example misses per category.
"""
from __future__ import annotations

import re
import sys
from collections import Counter

import analyze_predictions as ap

_HAN_RE = re.compile(r"[一-鿿]")
_LATIN_RE = re.compile(r"[A-Za-z]")

# Categories we can fix purely in the plugin (no model change).
ENGINEERING_CATS = {"latin_residue", "single_letter"}


def _han_only(s: str) -> str:
    return "".join(_HAN_RE.findall(s))


def collect_offer_outcomes(events):
    """Replay the event stream; for every value-add offer bound to a pre-commit
    window, emit a record of what was offered vs what the user actually
    committed (the next window delta)."""
    records = []
    cur_window = ""
    offer_by_window: dict[str, dict] = {}
    last_prompt_for_window: dict[str, str] = {}

    for ev in events:
        if ev.kind == "ctor":
            cur_window = ""
            offer_by_window.clear()
            last_prompt_for_window.clear()
            continue
        if ev.kind == "filter":
            offer_by_window[cur_window] = {
                "window": cur_window,
                "text": ev.text,
                "action": ev.action,
                "prompt": last_prompt_for_window.get(cur_window, ""),
            }
            continue
        if ev.kind == "built":
            new_window = ev.window
            last_prompt_for_window[new_window] = ev.prompt
            if new_window != cur_window:
                delta = ap.committed_delta(cur_window, new_window)
                if delta is not None:
                    offer = offer_by_window.get(cur_window)
                    if offer and offer.get("action"):
                        records.append({
                            "window": cur_window,
                            "prompt": offer.get("prompt", ""),
                            "ai_text": offer["text"],
                            "delta": delta,
                            "action": offer["action"],
                            "selected": offer["text"] == delta,
                        })
                cur_window = new_window
            continue
    return records


def classify(rec) -> str:
    ai, delta, py = rec["ai_text"], rec["delta"], rec["prompt"]
    if _LATIN_RE.search(ai):
        return "latin_residue"
    if len(py) == 1:
        return "single_letter"
    ai_h, delta_h = _han_only(ai), _han_only(delta)
    if delta_h.startswith(ai_h) and len(ai_h) < len(delta_h):
        return "ai_prefix"
    if ai_h.startswith(delta_h) and len(ai_h) > len(delta_h):
        return "ai_overshoot"
    if ai_h and delta_h and len(ai_h) == len(delta_h):
        return "homophone"
    return "semantic"


def main():
    argv = sys.argv[1:]
    dump = "--dump" in argv
    files = ap.discover_logs(_Args([a for a in argv if not a.startswith("-")]))
    if not files:
        print("no logs", file=sys.stderr)
        return 1
    events = ap.parse_files(files)
    recs = collect_offer_outcomes(events)
    va = [r for r in recs if r["action"] in ("inserted", "promoted")]
    if not va:
        print("no value-add offers with a following commit", file=sys.stderr)
        return 0

    sel = sum(1 for r in va if r["selected"])
    misses = [r for r in va if not r["selected"]]
    for r in va:
        r["cat"] = classify(r)

    print(f"value-add offers (with a following commit): {len(va)}")
    print(f"  selected:   {sel}   ({sel/len(va)*100:.1f}%)")
    print(f"  unselected: {len(misses)}")
    print()
    print("── miss attribution ─────────────────────────────────")
    cats = Counter(r["cat"] for r in misses)
    for cat, n in cats.most_common():
        tag = "ENG" if cat in ENGINEERING_CATS else "model"
        print(f"  {cat:14s} {n:4d}  {n/len(misses)*100:5.1f}%   [{tag}]")
    print()

    # Projection: suppress all offers whose category is engineering-fixable, and
    # confirm we lose no real selections.
    def fixable(r):
        return classify(r) in ENGINEERING_CATS
    kept = [r for r in va if not fixable(r)]
    kept_sel = sum(1 for r in kept if r["selected"])
    lost_sel = sel - kept_sel
    print("── if we suppress engineering-fixable offers ─────────")
    print(f"  before: {sel}/{len(va)} = {sel/len(va)*100:.1f}%")
    print(f"  after : {kept_sel}/{len(kept)} = "
          f"{kept_sel/len(kept)*100:.1f}%" if kept else "  after : (none kept)")
    print(f"  suppressed offers: {len(va)-len(kept)}   "
          f"real selections lost: {lost_sel}  (must be 0 to be free)")

    if dump:
        for cat in cats:
            sub = [r for r in misses if r["cat"] == cat]
            print(f"\n===== {cat} ({len(sub)}) =====")
            for r in sub[:40]:
                print(f"  py='{r['prompt']}'  AI='{r['ai_text']}'  "
                      f"用户='{r['delta']}'  win='{r['window'][-12:]}'")
    return 0


class _Args:
    """Minimal shim so we can reuse ap.discover_logs."""
    def __init__(self, files):
        self.files = files
        self.log_dir = None


if __name__ == "__main__":
    sys.exit(main())
