# Hugging Face Model Card 模板

将以下内容保存为仓库根目录的 `README.md`（Hugging Face 模型页展示用）。请把**斜体占位**替换为你的真实情况。

---

# Model name（例如：rime-ai-predict zh-base CT2）

## Model summary

用于 [Rime](https://rime.im) 输入法插件 **librime-ai-predict**（见 GitHub 仓库说明）的 **CTranslate2** 导出模型：拼音上下文 → 中文续写候选（seq2seq）。

- **框架**：*（例如 PyTorch 训练 → ONNX / OpenNMT → ct2-transformers 转换）*
- **推理**：仅支持通过 CTranslate2 加载；插件期望目录内含至少：
  - `shared_vocabulary.json`
  - `tokenizer.json`
  - `model.bin`
  - `config.json`

## Training data

- *简述语料来源、规模、是否可公开再分发。*
- *若使用网络爬取数据，说明合规性与过滤方式。*

## License

- **模型权重**：*（例如 Apache-2.0 / CC-BY-SA-4.0 / 自定义，与训练数据一致）*
- **本 Card 提及的插件代码**：BSD-3-Clause（见插件仓库 LICENSE）

## Limitations

- 仅用于输入法候选辅助，不保证事实准确性。
- 性能与设备（CPU/GPU）、线程数、模型大小相关。

## Citation

```bibtex
@software{librime_ai_predict,
  title = {librime-ai-predict},
  year = {2025},
  url = {https://github.com/YOUR_USER/librime-ai-predict}
}
```

## How to use

与插件文档一致：设置 `HUGGINGFACE_REPO` 后运行 `scripts/download_model.sh`，或在 `default.custom.yaml` / schema 中配置 `ai_predict/model_path`。

---

上传完成后，将 `scripts/download_model.sh` 中的默认 `HUGGINGFACE_REPO` 或本地说明改为你的 `用户名/仓库名`。
