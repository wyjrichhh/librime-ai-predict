# 草稿：发往 rime/squirrel Discussions（勿直接复制发送，请按需修改）

**标题建议**：Neural pinyin prediction plugin (CT2) + async UI refresh hook — seeking feedback

---

大家好，

我开发了一个 **Rime 外部插件** [librime-ai-predict](https://github.com/wyjrichhh/librime-ai-predict)（BSD-3-Clause）：在**不修改 librime 核心**的前提下，用 **CTranslate2** 做 seq2seq 预测，为拼音输入提供额外候选；设计上与官方 trie 版 [rime/librime-predict](https://github.com/rime/librime-predict) 互补。

### 设计要点

- 独立插件模块 `ai_predict`，组件 `ai_predict_translator` + `ai_predict_filter`。
- 异步推理 + debounce + cache；完成后通过 `Context::RefreshNonConfirmedComposition` 与属性 `ai_predict/ready` 通知可在**菜单构建完成之后**再刷新 UI（避免与 `ai_predict/text` 竞态）。
- 推理后端抽象 `InferenceBackend`，当前实现 `CT2Backend`，便于后续加其他后端。

### Demo

- （在此插入 GIF 或链接，例如 `https://github.com/wyjrichhh/librime-ai-predict/raw/main/docs/demo.gif`）

### 想请教维护者的问题

1. Rime 生态是否欢迎此类**可选神经网络插件**以独立仓库形式存在？若合适，是否可能将来纳入 `rime` 组织（transfer 或官方 fork）？
2. **鼠须管侧**：异步推理完成后需要**一次 UI 刷新**，当前通过监听 `property` 消息且匹配 `ai_predict/ready` 前缀触发 `rimeUpdate()`。维护者是否更倾向于**通用命名**（例如与具体插件无关的 property 约定），以便其他插件复用？
3. 插件日志使用独立 glog 实例；宿主侧设置 `RIME_LOG_DIR` 是否与上游对前端的预期一致？

感谢阅读；期待任何方向上的反馈。

---

（署名）
