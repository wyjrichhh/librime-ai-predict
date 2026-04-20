# Fresh clone 端到端复现（检查清单）

在联系维护者之前，建议在**干净目录**按下列步骤自测一遍，并在本文件勾选或记录实际路径。

## 1. 克隆 librime 与插件

```bash
mkdir -p /tmp/rime-fresh && cd /tmp/rime-fresh
git clone <你的 librime fork 或上游> librime
cd librime
git submodule update --init --recursive
```

将本插件放入 `plugins/librime-ai-predict`（clone 你的 `librime-ai-predict` 仓库，或复制目录）。

进入插件目录后**务必**初始化 CT2 子模块：

```bash
cd plugins/librime-ai-predict
git submodule update --init deps/CTranslate2
```

（维护者自测：已用 `git clone file:///…/librime-ai-predict` 验证提交历史与子模块指针正常。）

## 2. 构建 CTranslate2（插件目录）

```bash
cd plugins/librime-ai-predict
make deps
cd ../..
```

## 3. 构建 librime

```bash
make deps
make
```

确认 CMake 输出中包含 `Found plugin: .../librime-ai-predict` 与 `rime-ai-predict provides modules: ai_predict`。

## 4. 模型

- [ ] 运行 `./plugins/librime-ai-predict/scripts/download_model.sh`，或
- [ ] 手动将 CT2 目录放到 `~/Library/Rime/predict_models/zh-base-ct2-int8/`

## 5. Rime 配置

- [ ] `default.custom.yaml`：`modules` 含 `ai_predict`
- [ ] 所用拼音方案 `*.custom.yaml`：已按 `examples/schema.fragment.yaml` 合并

## 6. 前端（鼠须管示例）

- [ ] 将编译产物中的 `librime` 与插件 dylib 按你平时的方式接入 Squirrel（或替换 `Squirrel.app` 内文件）
- [ ] 部署；输入拼音；确认第 2 位出现带 `AI` 注释的候选（若配置了 Filter）

## 7. 日志

- [ ] `~/Library/Logs/Squirrel/`（若宿主设置 `RIME_LOG_DIR`）下存在 `*.ai_predict*.log.*`

---

**记录（本次自测填写）：**

| 项 | 值 |
|----|-----|
| 日期 | |
| macOS 版本 | |
| Xcode / CLT | |
| 失败步骤（如有） | |
