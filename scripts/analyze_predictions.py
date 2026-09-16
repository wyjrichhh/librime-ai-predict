#!/usr/bin/env python3
"""Analyze ai_predict inference history to estimate how often the AI candidate
is actually chosen by the user -- i.e. whether the prediction is pulling its
weight.

WHY THIS IS A *PROXY*, NOT A DIRECT MEASUREMENT
-----------------------------------------------
The plugin's glog output records what the AI *offered* (cache HIT + filter
action), but it never logs the user's commit/selection directly. We recover
the selection signal from a side channel:

  Every segment the user commits lands in librime's commit_history, and the
  ContextBuilder concatenates the most-recent commits into `window_text` for
  the NEXT inference. So the time-ordered sequence of `window_text` values is,
  in effect, a transcript of everything the user committed.

  => When the AI offers text T while the window is W, and the very next window
     change appends exactly T (window goes W -> ...W+T), the user committed the
     AI's suggestion. If the appended delta is something else, they ignored it.

This is a heuristic. It can't see: commits into apps that don't round-trip
through a fresh Query, backspace/edit churn, or selections of a segment that is
never followed by more typing. It is, however, stable and comparable run over
run -- which is exactly what we need to judge optimization effects.

OFFER TYPES (from ai_predict_filter) carry different value:
  inserted  -> AI surfaced a NOVEL candidate the menu didn't have   (value-add)
  promoted  -> AI pulled a buried candidate up to the top slot      (value-add)
  dedup     -> AI text already sat at slot #1; IME had it anyway    (no value-add)

The headline metric is the VALUE-ADD selection rate: of the times the AI
offered something the IME wouldn't have put on top (inserted+promoted), how
often did the user take it.

USAGE
-----
  ./analyze_predictions.py                      # auto-find Squirrel logs
  ./analyze_predictions.py --log-dir DIR        # explicit dir
  ./analyze_predictions.py LOGFILE ...          # explicit files
  ./analyze_predictions.py --record --label "baseline"   # append run to history
  ./analyze_predictions.py --json               # machine-readable summary only

Re-run after a code change with a new --label to compare against prior runs;
--show-history prints the recorded runs side by side.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime

_LOG_BASENAME_GLOB = "rime.squirrel.ai_predict.*.log"
# Where the plugin writes its glog files. The frontend normally sets
# RIME_LOG_DIR so logs land in ~/Library/Logs/Squirrel/, but when that var is
# absent the plugin's glog falls back to $TMPDIR (macOS: a per-user
# /var/folders/.../T/rime.squirrel/). We scan both so the eval never silently
# reads stale data just because the host stopped exporting RIME_LOG_DIR.
def _default_log_dirs() -> list[str]:
    dirs = [os.path.expanduser("~/Library/Logs/Squirrel")]
    tmp = os.environ.get("TMPDIR") or "/tmp"
    dirs.append(os.path.join(tmp, "rime.squirrel"))
    return dirs
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HISTORY = os.path.join(_SCRIPT_DIR, "prediction_eval_history.jsonl")
# Immutable "before" reference, written once by --freeze-baseline. Distinct from
# the rolling history JSONL above.
FROZEN_BASELINE = os.path.join(_SCRIPT_DIR, "baseline_frozen.json")
# Clean A/B boundary: the moment the new binary went live. Events at/after this
# timestamp are attributed to the new binary; everything before is the old one.
BOUNDARY_FILE = os.path.join(_SCRIPT_DIR, ".eval_boundary")
# Human-readable comparison log the user reviews periodically.
COMPARISON_LOG = os.path.join(_SCRIPT_DIR, "eval_comparison.log")

# Minimum AI-offered commits in the after-window before a comparison is trusted.
MIN_AFTER_SAMPLE = 30

# glog line:  I20260617 13:05:35.763424 0x1f3649e80 predict_translator.cc:80] msg
LINE_RE = re.compile(
    r"^([IWEF])(\d{8}) (\d{2}:\d{2}:\d{2}\.\d+) (0x[0-9a-f]+) "
    r"([^:\]]+):(\d+)\] (.*)$"
)

RE_CTOR = re.compile(r"ai_predict_translator: ctor")
RE_BUILT = re.compile(
    r"ai_predict_translator: built context .*window_text='(.*?)' "
    r"effective_prompt='(.*?)' cache_key="
)
RE_HIT = re.compile(r"ai_predict_translator: cache HIT display='(.*?)'")
RE_MISS = re.compile(r"ai_predict_translator: cache MISS")
RE_NOCTX = re.compile(r"ai_predict_translator: Query no context built")
RE_QUERY = re.compile(r"ai_predict_translator: Query input='(.*?)' segment=")
RE_INVOKE = re.compile(r"PredictionEngine: invoking backend")
RE_RETURN = re.compile(r"PredictionEngine: backend returned raw='(.*)' \(took (\d+) ms\)")
RE_INFER_FAIL = re.compile(r"PredictionEngine: inference failed or empty")
RE_INSERTED = re.compile(r"ai_predict_filter: inserted new AI candidate '(.*?)' at slot #(\d+)")
RE_PROMOTED = re.compile(r"ai_predict_filter: promoted existing candidate '(.*?)' .*to slot #(\d+)")
RE_DEDUP = re.compile(r"ai_predict_filter: dedup -- AI text '(.*?)' already at slot #(\d+)")


@dataclass
class Event:
    kind: str          # built|hit|filter|miss|noctx|query|invoke|return|fail|ctor
    ts: str
    file_idx: int
    line_no: int
    window: str = ""
    prompt: str = ""
    text: str = ""     # ai display text / committed text
    action: str = ""   # inserted|promoted|dedup
    slot: int = -1
    latency_ms: int = -1


def discover_logs(args) -> list[str]:
    if args.files:
        return args.files
    dirs = [args.log_dir] if args.log_dir else _default_log_dirs()
    files: list[str] = []
    seen: set[str] = set()
    for d in dirs:
        for f in glob.glob(os.path.join(d, _LOG_BASENAME_GLOB)):
            if os.path.islink(f):
                continue
            real = os.path.realpath(f)
            if real in seen:
                continue
            seen.add(real)
            files.append(f)
    # Chronological by filename timestamp (glog encodes it), fall back to mtime.
    files.sort(key=lambda f: (os.path.basename(f), os.path.getmtime(f)))
    return files


def parse_files(files: list[str]) -> list[Event]:
    events: list[Event] = []
    for idx, path in enumerate(files):
        try:
            fh = open(path, "r", encoding="utf-8", errors="replace")
        except OSError as e:
            print(f"warning: cannot open {path}: {e}", file=sys.stderr)
            continue
        with fh:
            for ln, raw in enumerate(fh, 1):
                m = LINE_RE.match(raw.rstrip("\n"))
                if not m:
                    continue
                # Sortable full timestamp "YYYYMMDD HH:MM:SS.ffffff": date is
                # group(2), time-of-day is group(3). Combining them lets events
                # be partitioned by an absolute A/B boundary (--since/--until).
                ts, msg = f"{m.group(2)} {m.group(3)}", m.group(7)
                ev = _classify(msg, ts, idx, ln)
                if ev:
                    events.append(ev)
    return events


def filter_events(events: list[Event], since: str | None,
                  until: str | None) -> list[Event]:
    """Keep only events whose timestamp lies in [since, until].

    Bounds are compared lexicographically against Event.ts
    ("YYYYMMDD HH:MM:SS.ffffff"), which sorts chronologically. A bound may be
    given with or without the date half; we compare the common prefix length so
    a "YYYYMMDD HH:MM:SS" bound matches the dotted-microsecond timestamps.
    """
    if not since and not until:
        return events
    out = []
    for ev in events:
        if since and ev.ts < since:
            continue
        if until and ev.ts > until:
            continue
        out.append(ev)
    return out


def _now_ts() -> str:
    """Current time in the same sortable format as Event.ts."""
    return datetime.now().strftime("%Y%m%d %H:%M:%S.%f")


def compute(files: list[str], since: str | None = None,
            until: str | None = None, label: str | None = None) -> dict:
    """Parse → filter → analyze → summary, in one shot."""
    events = filter_events(parse_files(files), since, until)
    st = analyze(events)
    st.files = files
    return build_summary(st, label)


def _classify(msg: str, ts: str, idx: int, ln: int) -> Event | None:
    if RE_CTOR.search(msg):
        return Event("ctor", ts, idx, ln)
    if m := RE_BUILT.search(msg):
        return Event("built", ts, idx, ln, window=m.group(1), prompt=m.group(2))
    if m := RE_HIT.search(msg):
        return Event("hit", ts, idx, ln, text=m.group(1))
    if m := RE_INSERTED.search(msg):
        return Event("filter", ts, idx, ln, text=m.group(1), action="inserted", slot=int(m.group(2)))
    if m := RE_PROMOTED.search(msg):
        return Event("filter", ts, idx, ln, text=m.group(1), action="promoted", slot=int(m.group(2)))
    if m := RE_DEDUP.search(msg):
        return Event("filter", ts, idx, ln, text=m.group(1), action="dedup", slot=int(m.group(2)))
    if RE_MISS.search(msg):
        return Event("miss", ts, idx, ln)
    if RE_NOCTX.search(msg):
        return Event("noctx", ts, idx, ln)
    if m := RE_RETURN.search(msg):
        return Event("return", ts, idx, ln, latency_ms=int(m.group(2)))
    if RE_INFER_FAIL.search(msg):
        return Event("fail", ts, idx, ln)
    if RE_INVOKE.search(msg):
        return Event("invoke", ts, idx, ln)
    if m := RE_QUERY.search(msg):
        return Event("query", ts, idx, ln, prompt=m.group(1))
    return None


def committed_delta(prev_w: str, cur_w: str) -> str | None:
    """What the user committed, inferred from window_text growth.

    Commits append at the end of the window and trim from the front, so
    cur_w == (some suffix of prev_w) + delta. Recover delta as the tail of
    cur_w after the longest prefix of cur_w that is a suffix of prev_w.

    Returns None when the windows don't overlap and prev_w was non-empty -- a
    context reset (BackSpace to empty, app switch, new sentence), not a commit.
    """
    if cur_w == prev_w:
        return None
    if prev_w == "":
        return cur_w or None
    maxj = min(len(prev_w), len(cur_w))
    for j in range(maxj, 0, -1):
        if prev_w.endswith(cur_w[:j]):
            return cur_w[j:] or None
    # No shared boundary: not an append. Treat as reset unless cur extends prev
    # outright (defensive).
    if cur_w.startswith(prev_w):
        return cur_w[len(prev_w):] or None
    return None


@dataclass
class Stats:
    files: list[str] = field(default_factory=list)
    lines_parsed: int = 0
    sessions: int = 0                 # translator ctor count (process starts)
    queries: int = 0
    queries_no_context: int = 0
    cache_hits: int = 0
    cache_misses: int = 0
    inferences: int = 0               # backend invocations
    inference_failed: int = 0
    latencies: list[int] = field(default_factory=list)

    offers: Counter = field(default_factory=Counter)        # by action
    # commit accounting
    commits_total: int = 0            # window transitions classified as commits
    commits_with_ai_offer: int = 0    # an AI offer existed for the pre-commit window
    selected: Counter = field(default_factory=Counter)      # by action, ai_text == delta
    offered_not_selected: Counter = field(default_factory=Counter)

    def latency_pcts(self) -> dict:
        if not self.latencies:
            return {}
        s = sorted(self.latencies)
        def pct(p):
            return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]
        return {"p50": pct(50), "p90": pct(90), "p99": pct(99), "max": s[-1]}


def analyze(events: list[Event]) -> Stats:
    st = Stats()
    cur_window = ""
    # window -> last AI offer seen while that window was active. Selection is
    # decided against the offer bound to the PRE-commit window (see below).
    offer_by_window: dict[str, dict] = {}

    for ev in events:
        if ev.kind == "ctor":
            st.sessions += 1
            cur_window = ""
            offer_by_window.clear()
            continue
        if ev.kind == "query":
            st.queries += 1
            continue
        if ev.kind == "noctx":
            st.queries_no_context += 1
            continue
        if ev.kind == "miss":
            st.cache_misses += 1
            continue
        if ev.kind == "hit":
            st.cache_hits += 1
            continue
        if ev.kind == "invoke":
            st.inferences += 1
            continue
        if ev.kind == "fail":
            st.inference_failed += 1
            continue
        if ev.kind == "return":
            if ev.latency_ms >= 0:
                st.latencies.append(ev.latency_ms)
            continue
        if ev.kind == "filter":
            st.offers[ev.action] += 1
            # Bind this offer to the window that is active right now; selection
            # is later decided against the offer for the PRE-commit window.
            offer_by_window[cur_window] = {
                "window": cur_window, "text": ev.text, "action": ev.action
            }
            continue
        if ev.kind == "built":
            new_window = ev.window
            # A change in window_text since the last built/hit cycle means the
            # user committed a segment. Decide selection against the AI offer
            # that was active for the PRE-commit window.
            if new_window != cur_window:
                delta = committed_delta(cur_window, new_window)
                if delta is not None:
                    st.commits_total += 1
                    offer = offer_by_window.get(cur_window)
                    if offer and offer.get("action"):
                        st.commits_with_ai_offer += 1
                        if offer["text"] == delta:
                            st.selected[offer["action"]] += 1
                        else:
                            st.offered_not_selected[offer["action"]] += 1
                # advance window; clear stale per-window offers for cleanliness
                cur_window = new_window
            continue

    return st


def _rate(num: int, den: int) -> float:
    return (num / den) if den else 0.0


def build_summary(st: Stats, label: str | None) -> dict:
    value_offered = st.offers["inserted"] + st.offers["promoted"]
    value_selected = st.selected["inserted"] + st.selected["promoted"]
    all_selected = sum(st.selected.values())
    all_offer_commits = st.commits_with_ai_offer
    return {
        "label": label,
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "files": [os.path.basename(f) for f in st.files],
        "sessions": st.sessions,
        "queries": st.queries,
        "queries_no_context": st.queries_no_context,
        "cache_hits": st.cache_hits,
        "cache_misses": st.cache_misses,
        "inferences": st.inferences,
        "inference_failed": st.inference_failed,
        "latency_ms": st.latency_pcts(),
        "offers": dict(st.offers),
        "offers_total": sum(st.offers.values()),
        "offers_value_add": value_offered,
        "commits_total": st.commits_total,
        "commits_with_ai_offer": st.commits_with_ai_offer,
        "selected": dict(st.selected),
        "selected_total": all_selected,
        "selected_value_add": value_selected,
        # headline rates
        "selection_rate_overall": round(_rate(all_selected, all_offer_commits), 4),
        "selection_rate_value_add": round(
            _rate(value_selected,
                  st.selected["inserted"] + st.selected["promoted"]
                  + st.offered_not_selected["inserted"] + st.offered_not_selected["promoted"]),
            4,
        ),
        "cache_hit_rate": round(_rate(st.cache_hits, st.cache_hits + st.cache_misses), 4),
    }


def print_report(st: Stats, summ: dict) -> None:
    def pct(x):
        return f"{x*100:5.1f}%"

    bar = "=" * 70
    print(bar)
    print("ai_predict 推理效果分析  (AI candidate selection estimate)")
    print(bar)
    print(f"  日志文件     : {len(st.files)} 个")
    for f in summ["files"]:
        print(f"                 {f}")
    print(f"  进程会话     : {st.sessions}    "
          f"Query 次数: {st.queries}  (无上下文跳过: {st.queries_no_context})")
    print()

    print("── 推理与缓存 ─────────────────────────────────────────────────────")
    print(f"  后端推理次数 : {st.inferences}   (失败/空: {st.inference_failed})")
    print(f"  缓存命中     : {st.cache_hits}   未命中: {st.cache_misses}   "
          f"命中率: {pct(summ['cache_hit_rate'])}")
    lat = summ["latency_ms"]
    if lat:
        print(f"  推理耗时(ms) : p50={lat['p50']}  p90={lat['p90']}  "
              f"p99={lat['p99']}  max={lat['max']}")
    print()

    print("── AI 候选展示 (offer) ────────────────────────────────────────────")
    o = st.offers
    print(f"  inserted(新增) : {o['inserted']:5d}   << 增益:菜单本没有")
    print(f"  promoted(提权) : {o['promoted']:5d}   << 增益:埋没项提到首位")
    print(f"  dedup(已在#1)  : {o['dedup']:5d}   << 无增益:IME 本就排首位")
    print(f"  合计展示       : {summ['offers_total']:5d}   "
          f"(其中增益型 inserted+promoted = {summ['offers_value_add']})")
    print()

    print("── 被选中估计 (window_text 增量回溯) ──────────────────────────────")
    print(f"  可判定提交数         : {st.commits_total}")
    print(f"  其中 AI 有展示       : {st.commits_with_ai_offer}")
    s = st.selected
    ns = st.offered_not_selected
    def row(name, key):
        sel, nsel = s[key], ns[key]
        tot = sel + nsel
        print(f"    {name:14s} 选中 {sel:4d} / 展示 {tot:4d}   "
              f"采纳率 {pct(_rate(sel, tot))}")
    row("inserted", "inserted")
    row("promoted", "promoted")
    row("dedup", "dedup")
    print()
    print(f"  ▶ 总体采纳率         : {pct(summ['selection_rate_overall'])}   "
          f"({summ['selected_total']} / {st.commits_with_ai_offer})")
    print(f"  ▶ 增益型采纳率(主指标): {pct(summ['selection_rate_value_add'])}   "
          f"(inserted+promoted 中被选中的比例)")
    print(bar)
    print("说明: 采纳为启发式推断 -- AI 展示文本与下一次 window_text 增量一致")
    print("      即判为被用户提交。增益型采纳率是评估 AI 帮助大小的核心指标:")
    print("      dedup 即便被选,IME 原本也会给出,不计入 AI 的额外价值。")
    print(bar)


def record_run(history_path: str, summ: dict) -> None:
    with open(history_path, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(summ, ensure_ascii=False) + "\n")
    print(f"\n已记录本次结果到 {history_path}", file=sys.stderr)


def show_history(history_path: str) -> None:
    if not os.path.exists(history_path):
        print(f"无历史记录: {history_path}", file=sys.stderr)
        return
    rows = []
    with open(history_path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    if not rows:
        print("历史记录为空", file=sys.stderr)
        return
    print(f"{'label':<20}{'date':<20}{'value-add%':>11}{'overall%':>10}"
          f"{'offers':>9}{'infer':>8}{'p50ms':>7}")
    print("-" * 85)
    for r in rows:
        lat = r.get("latency_ms", {}) or {}
        print(f"{(r.get('label') or '-'):<20}"
              f"{r.get('generated_at',''):<20}"
              f"{r.get('selection_rate_value_add',0)*100:>10.1f}%"
              f"{r.get('selection_rate_overall',0)*100:>9.1f}%"
              f"{r.get('offers_total',0):>9}"
              f"{r.get('inferences',0):>8}"
              f"{lat.get('p50','-'):>7}")


def freeze_baseline(files: list[str], label: str | None,
                    since: str | None, until: str | None) -> None:
    """Snapshot the current (pre-change) metrics as the immutable A/B baseline."""
    summ = compute(files, since, until, label or "frozen-baseline")
    summ["frozen_at"] = _now_ts()
    with open(FROZEN_BASELINE, "w", encoding="utf-8") as fh:
        json.dump(summ, fh, ensure_ascii=False, indent=2)
    print(f"已冻结基线到 {FROZEN_BASELINE}", file=sys.stderr)
    print(f"  增益型采纳率 {summ['selection_rate_value_add']*100:.1f}%  "
          f"总体 {summ['selection_rate_overall']*100:.1f}%  "
          f"AI展示提交 {summ['commits_with_ai_offer']}", file=sys.stderr)


def mark_boundary() -> None:
    """Record 'now' as the clean A/B split point (deploy moment of new binary)."""
    boundary = _now_ts()
    with open(BOUNDARY_FILE, "w", encoding="utf-8") as fh:
        json.dump({"boundary": boundary, "last_after_count": -1}, fh)
    print(f"已标记 A/B 边界 {boundary} -> {BOUNDARY_FILE}", file=sys.stderr)


def _load_json(path: str) -> dict | None:
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _delta_line(name: str, before: float, after: float, pct: bool = True) -> str:
    d = after - before
    arrow = "▲" if d > 1e-9 else ("▼" if d < -1e-9 else "＝")
    if pct:
        return (f"  {name:18s} {before*100:6.1f}%  →  {after*100:6.1f}%   "
                f"{arrow} {d*100:+.1f} pts")
    return (f"  {name:18s} {before:6.0f}   →  {after:6.0f}   {arrow} {d:+.0f}")


def compare(files: list[str]) -> int:
    """Compare frozen baseline vs after-boundary metrics; append to comparison log.

    Skips writing when the after-window has no new AI-offered commits since the
    last run (avoids launchd spamming identical blocks during idle periods).
    """
    base = _load_json(FROZEN_BASELINE)
    bnd = _load_json(BOUNDARY_FILE)
    if not base:
        print(f"无冻结基线 {FROZEN_BASELINE};先跑 --freeze-baseline。", file=sys.stderr)
        return 1
    if not bnd or not bnd.get("boundary"):
        print(f"无边界 {BOUNDARY_FILE};先跑 --mark-boundary。", file=sys.stderr)
        return 1

    boundary = bnd["boundary"]
    after = compute(files, since=boundary, until=None, label="after-boundary")
    after_count = after["commits_with_ai_offer"]

    if after_count == bnd.get("last_after_count", -1):
        print(f"边界后无新数据(AI展示提交仍为 {after_count}),跳过写入。",
              file=sys.stderr)
        return 0

    low_sample = after_count < MIN_AFTER_SAMPLE
    lines = []
    lines.append("=" * 70)
    lines.append(f"对比运行 @ {_now_ts()}    A/B 边界: {boundary}")
    lines.append(f"  基线: {base.get('label')} (冻结于 {base.get('frozen_at','?')})")
    if low_sample:
        lines.append(f"  ⚠ 样本不足: 边界后 AI 展示提交仅 {after_count} "
                     f"(< {MIN_AFTER_SAMPLE}),结论暂不可信。")
    lines.append("-" * 70)
    lines.append(_delta_line("增益型采纳率(主)",
                             base["selection_rate_value_add"],
                             after["selection_rate_value_add"]))
    lines.append(_delta_line("总体采纳率",
                             base["selection_rate_overall"],
                             after["selection_rate_overall"]))
    lines.append(_delta_line("缓存命中率",
                             base["cache_hit_rate"], after["cache_hit_rate"]))
    lines.append("  ── offer 分布 ──")
    bo, ao = base.get("offers", {}), after.get("offers", {})
    for k in ("inserted", "promoted", "dedup"):
        lines.append(_delta_line(f"  {k}", bo.get(k, 0), ao.get(k, 0), pct=False))
    lines.append(_delta_line("样本(AI展示提交)",
                             base["commits_with_ai_offer"],
                             after_count, pct=False))
    block = "\n".join(lines) + "\n"

    with open(COMPARISON_LOG, "a", encoding="utf-8") as fh:
        fh.write(block)
    bnd["last_after_count"] = after_count
    with open(BOUNDARY_FILE, "w", encoding="utf-8") as fh:
        json.dump(bnd, fh)
    print(block)
    print(f"已追加对比到 {COMPARISON_LOG}", file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Estimate ai_predict candidate selection rate from glog history.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("files", nargs="*", help="explicit log files (default: auto-discover)")
    ap.add_argument("--log-dir", help="directory holding rime.squirrel.ai_predict.*.log")
    ap.add_argument("--json", action="store_true", help="print JSON summary only")
    ap.add_argument("--record", action="store_true", help="append summary to history file")
    ap.add_argument("--label", help="label for this run (used with --record)")
    ap.add_argument("--history", default=DEFAULT_HISTORY, help="history JSONL path")
    ap.add_argument("--show-history", action="store_true", help="print recorded runs and exit")
    ap.add_argument("--since", help='only events at/after this ts "YYYYMMDD HH:MM:SS"')
    ap.add_argument("--until", help='only events at/before this ts "YYYYMMDD HH:MM:SS"')
    ap.add_argument("--freeze-baseline", action="store_true",
                    help="snapshot current metrics as the immutable A/B baseline and exit")
    ap.add_argument("--mark-boundary", action="store_true",
                    help="record 'now' as the A/B boundary (run right after deploying) and exit")
    ap.add_argument("--compare", action="store_true",
                    help="compare frozen baseline vs after-boundary; append to comparison log")
    args = ap.parse_args()

    if args.show_history:
        show_history(args.history)
        return 0

    if args.mark_boundary:
        mark_boundary()
        return 0

    files = discover_logs(args)
    if not files:
        print("未找到 ai_predict 日志。用 --log-dir 或直接传文件路径。", file=sys.stderr)
        for d in _default_log_dirs():
            print(f"默认查找: {os.path.join(d, _LOG_BASENAME_GLOB)}", file=sys.stderr)
        return 1

    if args.freeze_baseline:
        freeze_baseline(files, args.label, args.since, args.until)
        return 0

    if args.compare:
        return compare(files)

    events = filter_events(parse_files(files), args.since, args.until)
    st = analyze(events)
    st.files = files
    summ = build_summary(st, args.label)

    if args.json:
        print(json.dumps(summ, ensure_ascii=False, indent=2))
    else:
        print_report(st, summ)

    if args.record:
        record_run(args.history, summ)

    return 0


if __name__ == "__main__":
    sys.exit(main())
