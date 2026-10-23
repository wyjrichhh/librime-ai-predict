# Arch Linux / Omarchy：构建 Fcitx5 AI 拼音完整包

## 包含内容与系统依赖

`fcitx5-rime-ai-predict` 是供系统 Fcitx5 加载的 Rime 输入法包，包含：

- Fcitx5-Rime 5.1.17 前端，并附带 `_refresh_ui` 异步刷新补丁。
- 从官方 `rime/librime` 源码编译并合入 `ai_predict` 的 librime 1.17.0；CTranslate2 / RUY 静态链接进入该库。上游版本由 PKGBUILD 中的提交固定。
- `zh-base-ct2-int8` 简体模型，以及继承朙月拼音的 `ai_pinyin` 方案。

Fcitx5 主程序和基础词库由包依赖交给 pacman 安装；GTK / Qt 集成见下面的环境准备命令。桌面环境和整个 Fcitx5 框架不会复制进包内。

随包编译的 librime 安装到 `/usr/lib/fcitx5-rime-ai-predict/`，前端通过 RPATH 查找，避免覆盖系统已有的同名库；这只是安装路径隔离，不是另一个源码项目，也不修改全局 `ld.so.conf`。模型位于 `/usr/share/rime-data/predict_models/zh-base-ct2-int8/`，方案使用绝对路径引用它，不需要将模型复制到每个用户的目录。

本包提供并冲突于 `fcitx5-rime`。支持与使用独立 addon、库名和用户数据目录的新版 `bangke-pinyin` 共存；旧版全局接管候选面板或复用 `librime.so.1` 的蚌壳包需要先升级。先检查现有输入法；不要用 `--overwrite` 绕过冲突或直接删除其文件。首次验证优先检查包内容并在隔离的用户数据目录测试，替换现有桌面输入法前保留备份和可恢复的原包。

## 准备依赖

先按系统维护流程完成一致的系统更新，再安装构建依赖。Arch 不支持只刷新数据库后选择性升级部分包。Try Omarchy 可能有固定版本的内核和桌面组件，应保留该环境维护的 `IgnorePkg` 策略。

```bash
sudo pacman -Syu
sudo pacman -S --needed base-devel git cmake ninja extra-cmake-modules boost \
  nlohmann-json fcitx5 fcitx5-gtk fcitx5-qt google-glog gflags yaml-cpp \
  leveldb marisa opencc rime-luna-pinyin rime-prelude rime-essay rime-stroke
```

使用普通用户运行 `makepkg`，不要用 sudo 构建。建议预留至少 4 GiB 构建空间，以及模型和压缩包所需空间。内存不足时降低并行度，例如 `export CMAKE_BUILD_PARALLEL_LEVEL=2`；CTranslate2 的 Makefile 还支持 `NOPARALLEL=1`。

## 获取并核对源码

```bash
git clone https://github.com/wyjrichhh/librime-ai-predict.git
cd librime-ai-predict
git rev-parse HEAD
git status --short
cd packaging/arch
makepkg --printsrcinfo
makepkg --syncdeps --log
```

这份 PKGBUILD 必须放在完整、已提交并已推送的插件 Git checkout 中使用。它从所在仓库取得 HEAD，并让 makepkg 从公开仓库获取同一个提交；未提交的本地改动不会进入构建。

上游源码固定为：

| 组件 | 提交 |
| --- | --- |
| librime | `1b3417d2b6a11b5e50e0e1fa9dc396056aa11dce` |
| Fcitx5-Rime | `f5048611ea60e53955d5f013f608551e85d38f09` |
| CTranslate2 | 插件子模块锁定的 `226c95d94e660c48b11c62e108886b7ef76d589d` |

模型来自 `model-v1/zh-base-ct2-int8.tar.gz`，压缩包大小为 213,846,663 字节（约 204 MiB），SHA-256 为 `667d7f5e3c87073593e890299e917a28d45505e2c023692ac91e1b08354690d4`。makepkg 校验模型；Git 源码通过固定提交锁定。首次获取 CTranslate2 的嵌套子模块需要网络，已下载的模型可由 makepkg 缓存复用。

日志由 `makepkg --log` 保存在打包目录，失败时应检查完整日志，不只看最后的退出码。

## 检查构建产物

```bash
makepkg --packagelist
package_file=$(makepkg --packagelist | head -n 1)
pacman -Qip "$package_file"
pacman -Qlp "$package_file"
```

使用 `makepkg --packagelist` 列出的主包准确路径执行检查和安装，不要把 debug 包误当作主包。压缩格式由 makepkg 配置决定，可能为 `.pkg.tar.xz` 或 `.pkg.tar.zst`。

必须检查前端、随包编译的 librime、模型和方案均在包内，且没有 `/home/`、构建临时目录或用户配置文件。再解包检查 ELF 架构、RPATH 与共享库依赖，确认加载的是本包的 librime，避免意外加载其他输入法包自带的库。

PKGBUILD 的 `check()` 检查插件符号、前端 RPATH，并编译运行 `smoke-test.cc`：加载前端 ELF、在独立 Rime 数据目录部署方案、运行真实模型推理、等待异步刷新通知、检查候选和提交，再完整初始化第二次以检查重载。该测试不启动桌面 Fcitx5 UI，不能代替真实桌面输入验收。

## 安装与用户配置

安装前保存现有包和配置，按 pacman 的冲突提示审阅替换关系。包声明了实际链接的 OpenCC SONAME 依赖；OpenCC ABI 更新后应重新编译安装包，不能用同名软链接绕过依赖。

```bash
sudo pacman -U "$package_file"
```

Fcitx5 用户数据目录为 `${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/rime`。先备份整个目录，再编辑 `default.custom.yaml`，将 `ai_pinyin` 加入现有 `patch/schema_list`；如果已有该文件，应合并配置，不能直接覆盖：

```yaml
patch:
  schema_list:
    - schema: ai_pinyin
```

在 Fcitx5 的配置工具中添加 Rime 输入法，在 Rime 菜单中重新部署并选择「AI 拼音」。更新共享库后需要重新启动 Fcitx5 或重新登录桌面，使新进程加载新库。GTK / Qt 集成和 Wayland 输入法配置沿用 Fcitx5 / Omarchy 的系统设置。

Omarchy 由 `omarchy-fcitx5.service` 管理进程。更新配置或共享库后使用 `systemctl --user restart omarchy-fcitx5.service`，不要额外运行 `fcitx5 -rd`；独立进程会占用 D-Bus 名称，导致服务循环重启。如果已经发生，先停止该服务，退出同一用户手动启动的 Fcitx5，再启动服务。用 `systemctl --user show omarchy-fcitx5.service -p MainPID -p NRestarts` 检查进程及重启情况。

如果 `fcitx5-configtool` 只打开配置目录，检查是否已安装图形配置工具 `fcitx5-config-qt`；或者先停止服务、备份并编辑 `~/.config/fcitx5/profile`，在当前组的 `Items` 中保留原有条目并增加 `Name=rime`，将 `DefaultIM` 设置为 `rime`，随后启动服务。`fcitx5-remote -s rime` 切换输入法，`fcitx5-remote -n` 应返回 `rime`。

从 Bangke 迁移还应检查用户目录中的 `luna_pinyin.custom.yaml`。AI 拼音继承朙月拼音，旧覆盖配置也会被继承，可能重复注入 AI translator/filter，或覆盖词典与 emoji 设置。迁移时先保存原配置，再逐项合并所需设置，保留用户词库；重新部署后检查 AI translator/filter 各出现一次。

AI 方案默认简体，新插入或提权的 AI 候选默认位于第 2 位。如果模型输出已经是首候选，则去重并保留原首选，不额外添加 AI 行。模型已经随包安装，完全离线推理。本补丁处理异步刷新；不承诺提供与鼠须管相同的候选注释配色。

## 开发与运行检查

应使用新的临时 XDG 配置和数据目录，避免将原输入法的配置、模型或已部署缓存当作新包的能力。至少完成：

1. 部署 `ai_pinyin` 成功，日志无缺失词典、模型或组件错误。
2. Fcitx5 正确加载包内的前端和随包编译的 librime，动态依赖全部可解析。
3. 输入足够长的拼音产生 AI 候选；停止按键后，异步推理结果能主动刷新到菜单中。
4. 输入上文并提交，再继续输入，验证上下文预测、候选选择和提交。
5. 验证翻页、移动高亮、清空输入、重新部署及退出；特别检查退出时 glog 生命周期。
6. 报告问题时附上包文件校验和、源码 SHA、构建日志和相关运行日志。分享日志前移除输入内容等个人信息。

## 常见问题

### 增量构建使用旧源码

修改插件后，应先将提交发布到可访问的仓库，再重新运行默认的 makepkg 流程。`makepkg --noextract` 会复用现有 `$srcdir`，不会自动更新其中的插件 checkout 或重新应用前端补丁。更换提交后，优先重新提取源码并构建，避免缓存掩盖依赖或补丁问题。

### 词库部署出现 `Encode failure`

`rime-essay` 的部分预设词条包含间隔号或全角字母等无法由当前拼音方案编码的字符，例如「亞歷山大·杜布切克」和「ＡＡ制」。Rime 会跳过这些预设词条，影响相应词汇的覆盖；应结合后续日志和方案部署结果判断部署是否成功。

### 上游依赖的编译警告

GCC 16 构建可能在 CTranslate2、cpuinfo 和 spdlog/fmt 中报告 `stringop-overread`、`array-bounds`、`dangling-reference` 等警告。这些警告涉及缓冲区长度、形状边界或对象生命周期，需要结合具体依赖版本和触发代码分析。构建成功或推理自检通过不足以排除相关风险；报告问题时应保留完整诊断和编译器版本。

## 参考

- [Arch makepkg 手册](https://man.archlinux.org/man/makepkg.8)：构建、日志与包管理参数。
- [Arch PKGBUILD 手册](https://man.archlinux.org/man/PKGBUILD.5)：依赖、冲突和打包函数。
- [Fcitx5-Rime 源码](https://github.com/fcitx/fcitx5-rime)：前端构建要求和通知处理。
