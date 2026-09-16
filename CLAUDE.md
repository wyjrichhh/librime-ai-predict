# librime-ai-predict

librime 的 CTranslate2 推理插件,为拼音输入实时补 AI 候选。模型在键入时异步推理,把预测结果作为候选注入菜单。

## 架构(src/)

- **`predict_translator.cc`** — 驱动异步 CT2 推理。命中缓存时直接产出 AI 候选(`SimpleCandidate`, type=`ai_predict`),否则 `Schedule()` 一次推理。同时通过 Context property `ai_predict/text` 发布当前预测,供 filter 读取。在 `engine/translators` 中**必须列在首位**,否则 `MergedTranslation::Elect` 不会先问它。
- **`prediction_engine.cc`** — 后台 worker 线程。防抖(`debounce_ms`)合并快速连击,跑完推理后用 `RefreshNonConfirmedComposition()` 刷新候选菜单,触发缓存命中路径把 AI 候选浮现出来。会跳过两种情况:组合已变、用户已翻页/移动高亮(`selected_index > 0`)。
- **`context_builder.cc`** — 从 `commit_history` 回溯重建中文上下文窗口(`window_text`),拒绝非拼音 prompt(标点、ASCII 符号、大写缩写),剥离模型可能吐出的标点。cache_key = `window_text|prompt`。触发策略:有上下文(`window_text` 含至少一个汉字)时任意非空 prompt 都推理;无上下文(冷启动)要求 prompt ≥ `min_input_length`。**标点 `punct` 会进入 `window_text`**——CT2 模型带标点反而预测更准,标点充当句子结构信号;候选回显标点的问题由展示层 `StripAllPunctuation`(`ExtractDisplayText`)独立兜底剥离。仅 `thru` / `raw`(ASCII 杂质)被跳过。`has_context` 用"含汉字"而非"非空"判定,避免纯标点窗口(如 `。`)绕过冷启动阈值。
- **`predict_filter.cc`** — 候选流重写。把 AI 文本放到 `target_index`(默认 #1,即第 2 位),三种处置:
  - `inserted` — AI 文本是新的,菜单本没有 → 插入新候选(**增益**)
  - `promoted` — AI 文本在靠后位置 → 用 ShadowCandidate 提到目标位(**增益**)
  - `dedup` — AI 文本已在 #1 → 不动菜单(**无增益**,IME 本就给出)

## 评估推理效果:scripts/analyze_predictions.py

估算「AI 候选被用户选中的概率」,用来判断推理是否真的帮上忙、对比代码优化前后的效果。

### 数据来源与方法论前提(重要)

推理历史是插件的 glog 输出:`~/Library/Logs/Squirrel/rime.squirrel.ai_predict.*.log`。

> ⚠️ **日志可能落在 `$TMPDIR/rime.squirrel/`**。插件 glog 的 `FLAGS_log_dir` 靠宿主传 `RIME_LOG_DIR`/`GOOGLE_LOG_DIR`;两者都缺失时(如某些方式启动的 Squirrel 进程)fallback 到 `$TMPDIR`(macOS 上是 `/var/folders/.../T/rime.squirrel/`),**不在** `~/Library/Logs/Squirrel/`。`analyze_predictions.py` 默认已同时扫这两处(`_default_log_dirs()`)。若发现"重新部署用了一段时间却零变化",八成是只读到了旧日志——先 `lsof -p <squirrel_pid> | grep ai_predict` 确认日志真实落点,或 `find $TMPDIR -iname '*ai_predict*.log*'`。根因排查见 `predict_module.cc:EnsurePluginLoggingInitialized`。

**日志不直接记录用户是否选中 AI 候选**,只记录 AI *展示了什么*。选中靠启发式回溯:用户每提交一段,就进入 `commit_history`,成为下一次推理 `window_text` 的前缀。所以 `window_text` 的增量 == 用户实际提交的内容。**AI 展示文本 == 下一次 window 增量 ⇒ 判为被选中**。

这是启发式,看不到:不回流到新 Query 的提交、退格修改、跨应用切换等。但它稳定、可跨版本对比,正适合衡量优化效果。

### 核心指标:增益型采纳率

`dedup` 即便被选,IME 原本也会把它排 #1,不算 AI 的额外价值。所以**主指标是增益型采纳率 = (inserted+promoted 中被选中的) / (inserted+promoted 总展示)**,而非含 dedup 的总体采纳率。

### 用法

```bash
cd scripts

# 直接分析(自动找 Squirrel 日志)
python3 analyze_predictions.py

# 改代码 / 重新部署 / 用一段时间后,记录一次带标签的结果
python3 analyze_predictions.py --record --label "提高quality阈值"

# 横向对比各次 run(增益型采纳率 / 总体 / 展示数 / 推理数 / p50)
python3 analyze_predictions.py --show-history

# A/B 流程:改代码前冻结基线,改后划边界,只统计边界后的新日志再对比
python3 analyze_predictions.py --freeze-baseline --label "before-xxx"  # 改动前:落盘 baseline_frozen.json
python3 analyze_predictions.py --mark-boundary                          # 部署新二进制后:把"现在"设为 A/B 边界
python3 analyze_predictions.py --compare                                # 用一段时间后:只用边界后的提交 vs 冻结基线

# 其它
python3 analyze_predictions.py --json              # 机器可读摘要
python3 analyze_predictions.py --log-dir DIR       # 指定日志目录
python3 analyze_predictions.py FILE ...            # 分析归档的日志文件
```

`--record` 把结果追加到 `scripts/prediction_eval_history.jsonl`。

`--freeze-baseline` 把当前指标定格到 `baseline_frozen.json`(不可变的"改动前"参照);`--mark-boundary` 把当前时刻写入 `.eval_boundary`;`--compare` 只取边界**之后**时间戳的提交事件与冻结基线对比,并把结果追加到 `eval_comparison.log`。边界后样本 < 30 会标注"结论暂不可信";无新数据则跳过写入。日志按事件时间戳过滤(`HHMMSS.us` 解析),旧二进制产生的事件不会污染"改动后"统计。

### 基线(2026-06-22,基于 6-17 的日志)

| 指标 | 值 |
|---|---|
| 增益型采纳率(主指标) | 11.9% |
| 总体采纳率 | 37.5% |
| 缓存命中率 | 5.2% |
| 推理耗时 p50 / max | 89ms / 5615ms |

分类采纳率:`inserted` 4.5%、`promoted` 18.3%、`dedup` 68.6%。`inserted`(模型无中生有的新候选)采纳率最低,`promoted`(把对的候选提上来)明显更可信——优化时优先信提权而非新增。

## 配置

schema 配置见 `examples/schema.fragment.yaml`。关键 knobs:`model_path`、`min_input_length`、`context_window_size`、`debounce_ms`、`max_tokens`、`device`、`target_index`、`search_range`、`quality`。
