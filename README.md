# librime-ai-predict

Rime 插件：使用 **CTranslate2** 做 seq2seq 预测，异步推理并在结果就绪后通过刷新 composition / Context 属性让前端更新候选栏。

与官方 [rime/librime-predict](https://github.com/rime/librime-predict)（Trie 词表预测）不同，本仓库面向 **神经网络 / CT2 导出模型**，因此单独成库，命名为 **`librime-ai-predict`**。

## Quick start（约 5 分钟）

前提：

- **macOS**（其他平台未验证）
- 已安装 **Xcode Command Line Tools**（`xcode-select --install`）
- 已安装 **CMake**、**Boost**（`brew install cmake boost`）
- 能访问 GitHub（拉取 librime / 本仓库 / 子模块）

下面假设你在一个空目录 `~/work/` 下从零开始。

1. **拉取 librime 与本插件，并把插件放进 librime 的 `plugins/` 目录**

   librime 通过扫描自身的 `plugins/<目录名>/CMakeLists.txt` 来发现插件，所以本插件**必须**位于 `librime/plugins/librime-ai-predict/` 才会一起编译。

   ```bash
   cd ~/work
   git clone --recursive https://github.com/rime/librime.git
   git clone --recursive https://github.com/wyjrichhh/librime-ai-predict.git \
     librime/plugins/librime-ai-predict
   ```

   > 已经有 librime 源码？直接把本仓库 clone（或 `cp -R` / `ln -s`）到它的 `plugins/librime-ai-predict/` 即可。目录名必须是 `librime-ai-predict`。

2. **编译 CTranslate2（插件目录内，一次即可）**

   ```bash
   cd ~/work/librime/plugins/librime-ai-predict
   make deps
   ```

   产物：当前目录下的 `include/`、`lib/libctranslate2.a`（x86_64 还会有 `libcpu_features.a`）。librime 的 CMake 会自动把这里作为 `CTRANSLATE2_ROOT`。

3. **编译 librime（含本插件）**

   ```bash
   cd ~/work/librime
   make deps   # 若尚未构建 glog / yaml-cpp 等
   make
   ```

   编译日志中应能看到 `librime-ai-predict` 被作为插件加入构建。

4. **下载模型**

   ```bash
   cd ~/work/librime/plugins/librime-ai-predict
   ./scripts/download_model.sh
   ```

   脚本会从本仓库的 GitHub Release 拉取 CT2 模型并解压到 `~/Library/Rime/predict_models/zh-base-ct2-int8/`。详见下文「获取模型」。

5. **启用模块与方案**（示例：`luna_pinyin`）

   编辑用户配置目录（鼠须管：`~/Library/Rime/`）下的：

   - `default.custom.yaml` 的 `patch` 里为 `modules` 增加 `- ai_predict`。
   - `luna_pinyin.custom.yaml`（或对应 schema 的 `*.custom.yaml`）中按 [`examples/schema.fragment.yaml`](examples/schema.fragment.yaml) 合并 `engine/translators`、`engine/filters` 与 `ai_predict` 段。

6. **部署并重载**

   鼠须管：右上角菜单「重新部署」→ 完成后切到目标方案；若候选不刷新，**注销并重新登录** macOS 让输入法进程完全重启。

## 获取模型

模型托管在本仓库的 [GitHub Release](https://github.com/wyjrichhh/librime-ai-predict/releases) 中（`zh-base-ct2-int8.tar.gz`，约 300MB）。

### 方式 A：脚本下载（推荐）

```bash
./scripts/download_model.sh
```

默认安装到 `~/Library/Rime/predict_models/zh-base-ct2-int8/`，对应 schema 中的：

```yaml
ai_predict:
  model_path: predict_models/zh-base-ct2-int8
```

如需指向自建镜像：`MODEL_URL=https://your-mirror/.../zh-base-ct2-int8.tar.gz ./scripts/download_model.sh`

### 方式 B：手动放置

把含 `shared_vocabulary.json`、`tokenizer.json`、`model.bin`、`config.json` 的目录放到上述路径下即可，无需运行脚本。

## 构建方式（推荐）：作为 librime 子目录插件

这是 Rime 生态的常见做法（与 `librime-lua` 等一致）：插件源码放在 **`librime/plugins/<目录名>/`**，与 librime **同一 CMake 工程** 编译。

### 1. 准备 CTranslate2 静态库

在本插件仓库根目录执行（只需一次）：

```bash
git submodule update --init --recursive   # 或至少 init deps/CTranslate2 与所需子模块
make deps
```

会在当前目录下生成 `include/`、`lib/libctranslate2.a`（及 x86_64 时的 `libcpu_features.a`）。

### 2. 放入 librime 的 `plugins/` 目录

```bash
cd /path/to/librime
cp -R /path/to/librime-ai-predict plugins/librime-ai-predict
# 或使用 git submodule / symlink
```

目录名 **`librime-ai-predict`** 会被 `plugins/CMakeLists.txt` 扫描到；也可用环境变量：

```bash
export RIME_PLUGINS="librime-ai-predict"
```

### 3. 编译 librime

```bash
cd /path/to/librime
make deps    # 若尚未拉取 librime 依赖
make
```

CMake 会将 `-DCTRANSLATE2_ROOT` 默认指向插件源码根；若 CT2 安装在其他前缀：

```bash
cmake . -Bbuild -DCTRANSLATE2_ROOT=/path/to/ct2/prefix ...
```

## 模块与配置名（避免与官方 predict 冲突）

| 项目 | 名称 |
|------|------|
| Rime 模块名 | `ai_predict`（请在 `default.yaml` / `default.custom.yaml` 的 `modules` 中加入） |
| Translator 组件名 | `ai_predict_translator`（**必须放在 `engine/translators` 第一位**） |
| Filter 组件名 | `ai_predict_filter`（推荐放在 `engine/filters` 末尾） |
| 方案里配置段 | `ai_predict`（见 `examples/schema.fragment.yaml`） |
| Context 属性 | `ai_predict/text`：当前 AI 展示文本，由 Translator 写、Filter 读 |

### 常用配置项

| 键 | 说明 | 默认 |
|----|------|------|
| `ai_predict/model_path` | CT2 模型目录；相对路径基于 `user_data_dir` | — |
| `ai_predict/min_input_length` | 触发推理的最小有效拼音长度 | `10` |
| `ai_predict/context_window_size` | 从 commit history 取多少条作为上下文窗口 | `10` |
| `ai_predict/debounce_ms` | 防抖间隔（毫秒） | `200` |
| `ai_predict/max_tokens` | CT2 单次解码上限 | `256` |
| `ai_predict/device` | `cpu` / `cuda` 等 | `cpu` |
| `ai_predict/target_index` | Filter：AI 候选的 0 基目标位（默认第 2 位） | `1` |
| `ai_predict/search_range` | Filter：在前 N 个上游候选中查找重复 | `10` |
| `ai_predict/quality` | Translator 候选 quality（fallback） | `-1` |

## 架构说明

```
Translator                 Filter                前端 UI
─────────────              ─────────────────     ───────────────
PredictTranslator   ──┐                          comment="AI"
  ├─ Schedule(ctx)   │                           （可配色）
  ├─ cache HIT       │
  │   └─ set_property("ai_predict/text", text)
  │   └─ emit AI cand (slot policy: yield #1, claim #2, dedup #1)
  └─ 兜底候选          ──→  PredictFilter
                            ├─ get_property("ai_predict/text")
                            ├─ 在前 N 个候选中找 text 匹配
                            ├─ 找到 → ShadowCandidate + 移到 slot #2
                            └─ 没找到 → SimpleCandidate 插入 slot #2
```

- `PredictTranslator`：Rime `Translator`，调度推理并写入 Context 属性。
- `PredictFilter`：Rime `Filter`，重排候选使 AI 建议落在指定位并去重。
- `PredictionEngine`：后台线程、防抖、缓存；完成后 `RefreshNonConfirmedComposition` 并设置 `ai_predict/ready`。
- `ContextBuilder`：从 `CommitHistory` 构造上下文（跳过 `punct` / `thru` / `ai_predict` 等类型）。
- `InferenceBackend` / `CT2Backend`：推理后端抽象与 CTranslate2 实现。

### Translator 与 Filter 的分工

| 责任 | Translator | Filter |
|------|------------|--------|
| 触发异步推理 | 是 | 否 |
| 维护 cache | 是（PredictionEngine） | 否 |
| 广播当前结果 | 是（写 Context 属性） | 否 |
| 精确挪位与去重 | 简易 | 完整 |

两者通过 `ai_predict/text` 解耦。仅配置 Translator 时仍可工作（去重精度较弱）。

## Troubleshooting

| 现象 | 检查 |
|------|------|
| `download_model.sh` 下载失败 / 404 | 网络或代理问题；或 Release 资产名变化。可访问 [Releases 页](https://github.com/wyjrichhh/librime-ai-predict/releases) 手动下载，或用 `MODEL_URL=...` 指向镜像 |
| 插件未加载 / 无 `ai_predict` 组件 | `default.yaml` 或 `default.custom.yaml` 的 `modules` 是否包含 `ai_predict`；部署后是否重载 |
| 报错找不到模型 | `ai_predict/model_path` 相对路径是否相对于 `~/Library/Rime`（`user_data_dir`）；目录下是否有 `shared_vocabulary.json` |
| 无 AI 候选或界面不刷新 | 前端是否处理 `property` 通知中的 `ai_predict/ready`（鼠须管侧需配合刷新）；见 Squirrel 相关讨论 |
| 日志找不到 | 插件使用独立 glog 实例；宿主可设置 `RIME_LOG_DIR`（如鼠须管 `~/Library/Logs/Squirrel`）。查找 `*.ai_predict.*.log.INFO` |
| `make deps` 子模块失败 | 配置代理后重试；或手动 `git submodule update --init deps/CTranslate2` |
| 编译找不到 nlohmann | 先执行插件目录 `make deps`，确保 `include/nlohmann/json.hpp` 存在 |

## 许可证

见 [LICENSE](LICENSE)（BSD 3-Clause，与 [librime](https://github.com/rime/librime) 一致）。
