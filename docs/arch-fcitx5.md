# Arch Linux / Omarchy：构建 Fcitx5 AI 拼音完整包

## 验证状态

2026-10-22：已在 Try Omarchy 的 Arch Linux ARM（aarch64）环境复现旧 README 的依赖构建失败。本文和 `packaging/arch/` 是修订中的打包流程，尚未完成新包构建、安装与真实输入验证，不应将下面的步骤视为已通过验收。

测试环境：8 核 CPU、约 8 GiB 内存，GCC 16.1.1、CMake 4.4.4、Fcitx5 5.1.23、google-glog 0.7.1。x86_64 尚未测试。

## 包含内容与系统依赖

`fcitx5-rime-ai-predict` 是供系统 Fcitx5 加载的 Rime 输入法包，包含：

- Fcitx5-Rime 5.1.17 前端，并附带 `_refresh_ui` 异步刷新补丁。
- 合入 `ai_predict` 的私有 librime 1.17.0；CTranslate2 / RUY 静态链接进入该库。
- `zh-base-ct2-int8` 简体模型，以及继承朙月拼音的 `ai_pinyin` 方案。

Fcitx5 主程序、GTK / Qt 集成和基础词库由 pacman 安装。包通过依赖关系提供这些基础组件；不把桌面环境或整个 Fcitx5 框架复制进包内。

私有 librime 安装到 `/usr/lib/fcitx5-rime-ai-predict/`，前端通过 RPATH 查找，不修改全局 `ld.so.conf`。模型位于 `/usr/share/rime-data/predict_models/zh-base-ct2-int8/`，方案使用绝对路径引用它，不需要将模型复制到每个用户的目录。

本包提供并冲突于 `fcitx5-rime`，也声明与 `bangke-pinyin` 冲突。测试用 Omarchy 已预装 `bangke-pinyin`。先检查现有输入法；不要用 `--overwrite` 绕过冲突或直接删除其文件。首次验证优先检查包内容并在隔离的用户数据目录测试，替换现有桌面输入法前保留备份和可恢复的原包。

## 准备依赖

先按系统维护流程完成一致的系统更新，再安装构建依赖。Arch 不支持只刷新数据库后选择性升级部分包。Try Omarchy 可能有固定版本的内核和桌面组件，应保留该环境维护的 `IgnorePkg` 策略。

```bash
sudo pacman -Syu
sudo pacman -S --needed base-devel git cmake ninja extra-cmake-modules boost \
  nlohmann-json fcitx5 fcitx5-gtk fcitx5-qt google-glog gflags yaml-cpp \
  leveldb marisa opencc rime-luna-pinyin rime-prelude rime-essay
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

这份 PKGBUILD 必须放在完整、已提交并已推送的插件 Git checkout 中使用。它从所在仓库取得 HEAD，并让 makepkg 从公开仓库获取同一个提交；未提交的本地改动不会进入构建。跨机器测试必须先发布测试提交，远端 fetch 后检出同一提交并核对 SHA，不能靠复制未提交的文件同步源码。

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
pacman -Qip ./fcitx5-rime-ai-predict-*.pkg.tar.zst
pacman -Qlp ./fcitx5-rime-ai-predict-*.pkg.tar.zst
```

如启用了 makepkg 的 debug 包，使用 `makepkg --packagelist` 列出的主包准确路径执行检查和安装，不要把 debug 包误当作主包。

必须检查前端、私有 librime、模型和方案均在包内，且没有 `/home/`、构建临时目录或用户配置文件。再解包检查 ELF 架构、RPATH 与共享库依赖，确认不是意外链接到测试机器上 `bangke-pinyin` 的私有库。

PKGBUILD 的 `check()` 检查插件符号、前端 RPATH，并编译运行 `smoke-test.cc`：加载前端 ELF、在独立 Rime 数据目录部署方案、运行真实模型推理、等待异步刷新通知、检查候选和提交，再完整初始化第二次以检查重载。该测试不启动桌面 Fcitx5 UI，不能代替真实桌面输入验收。

## 安装与用户配置

以下步骤待新包实测。安装前保存现有包和配置，按 pacman 的冲突提示审阅替换关系。

```bash
sudo pacman -U ./fcitx5-rime-ai-predict-<版本>-<架构>.pkg.tar.zst
```

Fcitx5 用户数据目录为 `${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/rime`。先备份整个目录，再编辑 `default.custom.yaml`，将 `ai_pinyin` 加入现有 `patch/schema_list`；如果已有该文件，应合并配置，不能直接覆盖：

```yaml
patch:
  schema_list:
    - schema: ai_pinyin
```

在 Fcitx5 的配置工具中添加 Rime 输入法，在 Rime 菜单中重新部署并选择「AI 拼音」。更新共享库后需要重新启动 Fcitx5 或重新登录桌面，使新进程加载新库。GTK / Qt 集成和 Wayland 输入法配置沿用 Fcitx5 / Omarchy 的系统设置。

AI 方案默认简体，AI 候选默认位于第 2 位。模型已经随包安装，完全离线推理。本补丁处理异步刷新；不承诺提供与鼠须管相同的候选注释配色。

## 运行验收

应使用新的临时 XDG 配置和数据目录，避免将原输入法的配置、模型或已部署缓存当作新包的能力。至少完成：

1. 部署 `ai_pinyin` 成功，日志无缺失词典、模型或组件错误。
2. Fcitx5 正确加载包内的前端和私有 librime，动态依赖全部可解析。
3. 输入足够长的拼音产生 AI 候选；停止按键后，异步推理结果能主动刷新到菜单中。
4. 输入上文并提交，再继续输入，验证上下文预测、候选选择和提交。
5. 验证翻页、移动高亮、清空输入、重新部署及退出；特别检查退出时 glog 生命周期。
6. 保存包文件校验和、源码 SHA、构建日志和运行日志。真实桌面输入尚未验证时必须明确记录。

## 实测问题记录（2026-10-22）

| 问题 | 证据、影响与处理状态 |
| --- | --- |
| README 的「非 macOS」构建步骤实际上依赖 Xcode / brew / Squirrel | 已更正入口，Arch 使用本文的 Fcitx5 流程。 |
| 构建依赖未安装 | 已安装 cmake、ninja、extra-cmake-modules、boost、nlohmann-json；其余运行库已存在于测试机器。 |
| makepkg 调试包工具缺失 | 检查发现缺少 debugedit，已通过 pacman 安装，保留默认调试信息处理流程。 |
| 安装 base-devel 时镜像返回 404 | `texinfo-7.3-1-aarch64.pkg.tar.xz` 返回 404，整个安装事务失败，未升级任何包。后续单独安装上述可用构建依赖成功。base-devel 元包仍未安装；不能将当前环境称为干净的 base-devel 验证环境。文档提供完整更新后安装的标准流程，镜像一致性仍需确认。 |
| 首次 `make deps` 失败 | 基线提交 `5ee4dc5` 下 CMake 报 `ruy_cpuinfo` 引用不存在的 `cpuinfo::cpuinfo`。原因是 Makefile 未初始化 RUY 的嵌套 cpuinfo；本地修订加入初始化命令，待发布后复测。 |
| CMake 弃用警告 | 基线将最低策略版本设为 3.5；CMake 4.4.4 提示将移除低于 3.10 的兼容行为。修订提高到 3.10，待重新验证各依赖。 |
| 静态归档合并使用 macOS 工具 | 补齐 cpuinfo 后基线构建退出码为 0，但 GNU libtool 输出 Usage，`lib/` 仅有 `libctranslate2.a`，没有必需的 `libruy.a`；后续 echo 错误宣称安装成功。修订为 macOS 使用 libtool、Linux 使用 GNU ar MRI，并保留错误，待运行验证。 |
| 静态库链接顺序不正确 | CMake 将 RUY 放在 CT2 前面，Linux 静态链接可能无法解析 CT2 对 RUY 的引用；修订为 CT2、RUY、CPU features 顺序，待验证。 |
| CTranslate2 / spdlog 编译警告 | 补齐 cpuinfo 后，基线在 GCC 16.1.1 下出现 stringop-overread（cpuinfo）、array-bounds（CT2 decoding）、dangling-reference（spdlog/fmt），以及 unused-parameter、comment、implicit-fallthrough、reorder、maybe-uninitialized 等警告；完整日志保留在远端。来源和运行影响仍需逐项核查，不能声明构建无警告。 |
| 上游前端不处理刷新通知 | 固定的 Fcitx5-Rime 提交未处理 `property/_refresh_ui`；包附补丁在事件循环中更新匹配会话的聚焦输入框，待运行验证。 |
| 插件可能关闭宿主的 glog | 基线 finalize 不区分 glog 所有权。修订仅关闭插件自己初始化的实例，待部署和退出测试。 |
| 已有输入法包冲突 | Omarchy 预装 `bangke-pinyin 5.0.19-12`，提供并冲突于 `fcitx5-rime`。新 PKGBUILD 显式声明冲突；目前没有卸载或替换它。 |
| 远端目录权限异常 | 环境检查发现 `~/.cache/dconf` 由 root 持有，权限 0700，普通用户遍历 HOME 时报 Permission denied。此目录不属于构建源码；未修改它，仍需单独处理桌面环境权限。 |
| 本地 Ruby 校验的 PATH 权限警告 | macOS 上 `/opt/homebrew/bin` 权限为 0777，Ruby 报 Insecure world writable dir。该目录属于本机工具环境，未改权限；以仅含系统目录的 PATH 重新运行 YAML 解析成功。 |

基线日志位于测试机 `~/build/librime-ai-predict-audit/baseline-make-deps.log` 和 `baseline-with-cpuinfo.log`。测试用源码在 `baseline` 临时 worktree 中，使用公开提交通过 Git 获取；未传输未提交的源码。

## 参考

- [Arch makepkg 手册](https://man.archlinux.org/man/makepkg.8)：构建、日志与包管理参数。
- [Arch PKGBUILD 手册](https://man.archlinux.org/man/PKGBUILD.5)：依赖、冲突和打包函数。
- [Fcitx5-Rime 源码](https://github.com/fcitx/fcitx5-rime)：前端构建要求和通知处理。
