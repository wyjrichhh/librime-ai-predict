# GitHub 仓库创建（需你在网页上操作）

计划将插件发布为独立仓库（例如 `wyjrichhh/librime-ai-predict`）时：

1. 登录 GitHub → **New repository**
2. Repository name：`librime-ai-predict`
3. Description：`Rime plugin: neural pinyin prediction with CTranslate2`
4. Public
5. **不要**勾选 “Add a README” / “Add .gitignore”（本地已有）
6. License：可在网页选 BSD-3-Clause，或推送后仅以仓库内 `LICENSE` 为准

创建完成后在插件目录执行：

```bash
cd /path/to/librime-ai-predict
git remote add origin https://github.com/wyjrichhh/librime-ai-predict.git
git branch -M main
git push -u origin main
```

将 `wyjrichhh` 替换为你的用户名。
