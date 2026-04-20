# 用 GitHub Releases 托管模型（约 300MB 适用）

Git 仓库**不能直接**提交超过 **100MB** 的单个文件；**不要**把 `model.bin` 直接 `git add` 进主分支。

更合适的方式是 **[GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)**：在任意仓库（通常与本插件同一仓库）创建一次 Release，把模型打成压缩包作为**附件**上传。单个附件上限约 **2GB**，300MB 完全可行，且用户用普通 `curl` 即可下载，无需 Hugging Face 账号。

## 打包

在导出好的 CT2 目录**内部**执行（保证解压后根目录即有 `shared_vocabulary.json` 等文件）：

```bash
cd /path/to/zh-base-ct2-int8
tar -czf ../zh-base-ct2-int8.tar.gz .
```

或若顶层多包了一层目录，也可在上一级目录打包该文件夹；`scripts/download_model.sh` 会在压缩包内**自动查找** `shared_vocabulary.json` 并把其**所在目录**的文件复制到目标路径。

## 创建 Release

1. 打开 GitHub 仓库 → **Releases** → **Draft a new release**。
2. **Tag**：例如 `model-v1` 或 `zh-base-ct2-v1`（与脚本里 `GITHUB_RELEASE_TAG` 一致）。
3. **Attach** `zh-base-ct2-int8.tar.gz`。
4. 发布后在附件上右键复制链接，即为 `MODEL_URL`。

## 本地下载

**方式 A — 完整 URL（最直接）**

```bash
export MODEL_URL='https://github.com/你的用户名/librime-ai-predict/releases/download/model-v1/zh-base-ct2-int8.tar.gz'
./scripts/download_model.sh
```

**方式 B — 用仓库名 + 标签 + 文件名拼 URL**

```bash
export GITHUB_REPO=你的用户名/librime-ai-predict
export GITHUB_RELEASE_TAG=model-v1
export GITHUB_ASSET=zh-base-ct2-int8.tar.gz
./scripts/download_model.sh
```

## 与 Hugging Face 的取舍

| 方式 | 优点 | 缺点 |
|------|------|------|
| **GitHub Releases** | 与代码同一生态、Release 页可写更新说明、无需 HF 账号 | Release 附件占用仓库「发布」流程；大流量时留意 GitHub 合理使用政策 |
| **Hugging Face** | 模型社区惯例、版本与 Card 规范成熟 | 多一个平台维护 |

`download_model.sh` 在未设置 `MODEL_URL` / `GITHUB_*` 时仍会尝试 Hugging Face（需你确有对应仓库）；**仅使用 GitHub 时**请务必设置 `MODEL_URL` 或 `GITHUB_REPO` + `GITHUB_RELEASE_TAG`。

## Git LFS 说明（一般不推荐）

也可把大文件放在 **Git LFS**，但会增大 `git clone` 成本、占用 LFS 配额，对「只下载模型、不 clone 源码」的用户不如 Release 直观。300MB 级别优先用 **Release 附件**。
