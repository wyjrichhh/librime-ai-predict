# Arch Linux / Omarchy：构建 Fcitx5 AI 拼音完整包

## 验证状态

2026-10-22：已在 Try Omarchy 的 Arch Linux ARM（aarch64）环境完成静态依赖、官方 librime 与 AI 插件、Fcitx5-Rime 前端的实际编译和链接，生成完整 pacman 安装包。部署、真实模型推理、刷新通知、候选读取、提交和重新初始化自检通过。系统安装及桌面输入尚未验收，现有输入法未被替换；上游源码和词库的警告仍保留并记录在下文，不能将本次结果描述为“零警告”。

测试环境：8 核 CPU、约 8 GiB 内存，GCC 16.1.1、CMake 4.4.4、Fcitx5 5.1.23、google-glog 0.7.1。x86_64 尚未测试。

## 包含内容与系统依赖

`fcitx5-rime-ai-predict` 是供系统 Fcitx5 加载的 Rime 输入法包，包含：

- Fcitx5-Rime 5.1.17 前端，并附带 `_refresh_ui` 异步刷新补丁。
- 从官方 `rime/librime` 源码编译并合入 `ai_predict` 的 librime 1.17.0；CTranslate2 / RUY 静态链接进入该库。使用的提交与本工作区 `librime/` checkout 一致，没有换用其他 fork。
- `zh-base-ct2-int8` 简体模型，以及继承朙月拼音的 `ai_pinyin` 方案。

Fcitx5 主程序和基础词库由包依赖交给 pacman 安装；GTK / Qt 集成见下面的环境准备命令。桌面环境和整个 Fcitx5 框架不会复制进包内。

随包编译的 librime 安装到 `/usr/lib/fcitx5-rime-ai-predict/`，前端通过 RPATH 查找，避免覆盖系统已有的同名库；这只是安装路径隔离，不是另一个源码项目，也不修改全局 `ld.so.conf`。模型位于 `/usr/share/rime-data/predict_models/zh-base-ct2-int8/`，方案使用绝对路径引用它，不需要将模型复制到每个用户的目录。

本包提供并冲突于 `fcitx5-rime`，也声明与 `bangke-pinyin` 冲突。测试用 Omarchy 已预装 `bangke-pinyin`。先检查现有输入法；不要用 `--overwrite` 绕过冲突或直接删除其文件。首次验证优先检查包内容并在隔离的用户数据目录测试，替换现有桌面输入法前保留备份和可恢复的原包。

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

这份 PKGBUILD 必须放在完整、已提交并已推送的插件 Git checkout 中使用。它从所在仓库取得 HEAD，并让 makepkg 从公开仓库获取同一个提交；未提交的本地改动不会进入构建。跨机器测试必须先发布测试提交，远端 fetch 后检出同一提交并核对 SHA，不能靠复制未提交的文件同步源码。

上游源码固定为：

| 组件 | 提交 |
| --- | --- |
| librime | `1b3417d2b6a11b5e50e0e1fa9dc396056aa11dce` |
| Fcitx5-Rime | `f5048611ea60e53955d5f013f608551e85d38f09` |
| CTranslate2 | 插件子模块锁定的 `226c95d94e660c48b11c62e108886b7ef76d589d` |

模型来自 `model-v1/zh-base-ct2-int8.tar.gz`，压缩包大小为 213,846,663 字节（约 204 MiB），SHA-256 为 `667d7f5e3c87073593e890299e917a28d45505e2c023692ac91e1b08354690d4`。makepkg 校验模型；Git 源码通过固定提交锁定。首次获取 CTranslate2 的嵌套子模块需要网络，已下载的模型可由 makepkg 缓存复用。

日志由 `makepkg --log` 保存在打包目录，失败时应检查完整日志，不只看最后的退出码。

本改动尚未合并 main 时，clone 后需先运行 `git checkout codex/arch-fcitx5-packaging-20261022`，再进入 `packaging/arch`。每次修改后应将新提交发布到该测试分支，并让构建机 fetch、检出和核对同一 SHA。

## 检查构建产物

```bash
makepkg --packagelist
package_file=$(makepkg --packagelist | head -n 1)
pacman -Qip "$package_file"
pacman -Qlp "$package_file"
```

使用 `makepkg --packagelist` 列出的主包准确路径执行检查和安装，不要把 debug 包误当作主包。压缩格式由 makepkg 配置决定，本次 Omarchy 实测输出 `.pkg.tar.xz`，不能假定所有环境都输出 `.pkg.tar.zst`。

必须检查前端、随包编译的 librime、模型和方案均在包内，且没有 `/home/`、构建临时目录或用户配置文件。再解包检查 ELF 架构、RPATH 与共享库依赖，确认加载的是本包的 librime，而不是测试机器上 `bangke-pinyin` 自带的库。

PKGBUILD 的 `check()` 检查插件符号、前端 RPATH，并编译运行 `smoke-test.cc`：加载前端 ELF、在独立 Rime 数据目录部署方案、运行真实模型推理、等待异步刷新通知、检查候选和提交，再完整初始化第二次以检查重载。该测试不启动桌面 Fcitx5 UI，不能代替真实桌面输入验收。

## 安装与用户配置

以下步骤待新包实测。安装前保存现有包和配置，按 pacman 的冲突提示审阅替换关系。

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

AI 方案默认简体，AI 候选默认位于第 2 位。模型已经随包安装，完全离线推理。本补丁处理异步刷新；不承诺提供与鼠须管相同的候选注释配色。

## 运行验收

应使用新的临时 XDG 配置和数据目录，避免将原输入法的配置、模型或已部署缓存当作新包的能力。至少完成：

1. 部署 `ai_pinyin` 成功，日志无缺失词典、模型或组件错误。
2. Fcitx5 正确加载包内的前端和随包编译的 librime，动态依赖全部可解析。
3. 输入足够长的拼音产生 AI 候选；停止按键后，异步推理结果能主动刷新到菜单中。
4. 输入上文并提交，再继续输入，验证上下文预测、候选选择和提交。
5. 验证翻页、移动高亮、清空输入、重新部署及退出；特别检查退出时 glog 生命周期。
6. 保存包文件校验和、源码 SHA、构建日志和运行日志。真实桌面输入尚未验证时必须明确记录。

## 实测问题记录（2026-10-22）

代码与配置验证基线为插件提交 `628bd8c0494e684ff645dbf7cb193d71b275b228`，对应安装包约 205 MiB。后续文档提交会改变包版本中的 Git 短 SHA；以该次 `makepkg --packagelist`、构建日志和产物校验和为准。

| 验证项 | 结果 |
| --- | --- |
| 静态库 | `libctranslate2.a` 含 115 个成员，`libruy.a` 含 47 个成员，均实际编译生成并链接进 librime。 |
| 前端与后端 | 两者都是 aarch64 ELF；前端的 RUNPATH 恰为 `/usr/lib/fcitx5-rime-ai-predict`，所有动态依赖可解析。后端不依赖额外的 CTranslate2 / RUY 共享库。 |
| 资源 | 包内 6 个模型文件逐一与经校验的模型源文件比较，字节完全一致；AI 方案也与仓库文件一致。 |
| 源码路径 | 两个随包 ELF 均不再含本次 `$srcdir` 路径。 |
| 运行自检 | 两轮输入 `jintiantianqihenhao` 均预测并提交「今天天气很好」，收到异步刷新通知；第二轮在 finalize 后重新初始化，正常完成。 |
| 覆盖边界 | 测试加载前端 ELF 并直接调用 Rime API；未实例化桌面 Fcitx5 输入框，没有验收 GTK / Qt / Wayland 实际输入及候选窗口行为。 |

第一次构建从空源码目录启动，后续修复通过核对提交后使用 `makepkg --noextract --log --force` 增量验证。`--noextract` 会明确警告 `Using existing $srcdir/ tree`；更新提交时还必须同步 makepkg 的 `src/plugin` checkout，并按新提交重新应用前端补丁，不能只更新外层 PKGBUILD。普通用户首次构建应使用上面的默认流程。

| 问题 | 证据、影响与处理状态 |
| --- | --- |
| README 的「非 macOS」构建步骤实际上依赖 Xcode / brew / Squirrel | 已更正入口，Arch 使用本文的 Fcitx5 流程。 |
| 构建依赖未安装 | 已安装 cmake、ninja、extra-cmake-modules、boost、nlohmann-json；其余运行库已存在于测试机器。 |
| makepkg 调试包工具缺失 | 检查发现缺少 debugedit，已通过 pacman 安装，保留默认调试信息处理流程。 |
| 安装 base-devel 时镜像返回 404 | `texinfo-7.3-1-aarch64.pkg.tar.xz` 返回 404，整个安装事务失败，未升级任何包。后续单独安装上述可用构建依赖成功。base-devel 元包仍未安装；不能将当前环境称为干净的 base-devel 验证环境。文档提供完整更新后安装的标准流程，镜像一致性仍需确认。 |
| 首次 `make deps` 失败 | 基线提交 `5ee4dc5` 下 CMake 报 `ruy_cpuinfo` 引用不存在的 `cpuinfo::cpuinfo`。原因是 Makefile 未初始化 RUY 的嵌套 cpuinfo；已加入初始化命令，后续构建通过。 |
| CMake 弃用和策略警告 | 依赖与前端的最低策略版本提高到 3.10，librime 查找 Boost 时优先使用其配置包；重新构建不再出现相关 CMake 警告。 |
| 静态归档合并使用 macOS 工具 | 基线 GNU libtool 失败后 echo 仍错误报告成功。Linux 改用 GNU ar MRI，生成包含 47 个成员的 RUY 归档；Linux 构建和真实推理均通过。macOS 分支保留 Apple libtool，本次未重新构建 macOS。 |
| 静态库链接顺序不正确 | 改为 CT2、RUY、CPU features 的顺序，后端链接及真实推理通过。 |
| glog 导出宏重复定义 | 插件原先将宿主为共享 glog 设置的导出宏覆盖为空。现继承 librime 的设置，重新编译无重定义警告。 |
| 包内泄漏构建目录 | 新增编译器文件路径映射，并显式传递打包器的编译选项。两个随包 ELF 检查均无本次源码目录，makepkg 不再报告包内源码路径。 |
| CTranslate2 / spdlog 编译警告 | 补齐 cpuinfo 后，基线在 GCC 16.1.1 下出现 stringop-overread（cpuinfo）、array-bounds（CT2 decoding）、dangling-reference（spdlog/fmt），以及 unused-parameter、comment、implicit-fallthrough、reorder、maybe-uninitialized 等警告；完整日志保留在远端。来源和运行影响仍需逐项核查，不能声明构建无警告。 |
| 上游前端不处理刷新通知 | 包附补丁在 Fcitx 事件循环中更新匹配会话的聚焦输入框。补丁已编译、前端 ELF 加载通过；真实桌面刷新仍待验收。 |
| 刷新通知的封装格式判断错误 | 已按官方 librime 的 `_refresh_ui=source=ai_predict&kind=full` 封装修正前端补丁和自检，Rime API 自检确认两轮均收到刷新通知。 |
| 插件可能关闭宿主的 glog | 仅关闭插件自己初始化的实例。两轮初始化 / finalize 均正常结束，没有关闭宿主 glog 导致的重载故障。 |
| 已有输入法包冲突 | Omarchy 预装 `bangke-pinyin 5.0.19-12`，提供并冲突于 `fcitx5-rime`。新 PKGBUILD 显式声明冲突；目前没有卸载或替换它。 |
| 方案能打包但不能部署 | 将 Rime 在继承展开前要求的方案元数据放到顶层，并显式依赖 rime-stroke。AI 拼音与 stroke 的实际部署通过，自检同时检查部署状态和编译后的配置。 |
| AI 方案忽略用户自定义配置 | 显式 __patch 会关闭自动 custom 补丁。现以补丁列表在默认设置后加载 `ai_pinyin.custom:/patch?`；自检确认自定义模型路径生效，并成功加载对应模型。 |
| 词库预设词条编码警告 | 标准 rime-essay 的 193 条含间隔号或全角字母等词条出现 `Encode failure`。已核对 EntryCollector：失败发生在预设词汇补充阶段，这些词条被跳过，部署仍成功；例如「亞歷山大·杜布切克」和「ＡＡ制」。这是实际词汇覆盖限制，不是修复完成；未修改发行版词库或隐藏日志。 |
| 首次配置文件不存在 | Rime 在首次生成 build / user 文件，以及查找可选 grammar 配置时记录不存在的路径；随后按默认流程部署成功，未发现缺失模型或 AI 组件的错误。 |
| 远端目录权限异常 | 环境检查发现 `~/.cache/dconf` 由 root 持有，权限 0700，普通用户遍历 HOME 时报 Permission denied。此目录不属于构建源码；未修改它，仍需单独处理桌面环境权限。 |
| 本地 Ruby 校验的 PATH 权限警告 | macOS 上 `/opt/homebrew/bin` 权限为 0777，Ruby 报 Insecure world writable dir。该目录属于本机工具环境，未改权限；以仅含系统目录的 PATH 重新运行 YAML 解析成功。 |

基线日志位于测试机 `~/build/librime-ai-predict-audit/baseline-make-deps.log` 和 `baseline-with-cpuinfo.log`。完整构建与运行自检的迭代日志为同目录的 `package-build-01.log`、`package-build-02.log`、`package-build-03.log`；第 3 轮正常产出安装包，运行日志没有 ERROR 级记录。所有源码通过 Git 获取并核对提交。

剩余上游编译警告的核查边界：Fcitx5-Rime 的 `RIME_STRUCT` 使用 `{0}`，其余成员按 C++ 聚合规则初始化为零，不是未初始化内存；CT2 的 tensor-parallel 局部变量警告位于当前 CPU 单副本路径未使用的分支。cpuinfo 的 Raspberry Pi 修正函数将长度为 9 的 revision 参数声明为至少 64 字节，实际读取使用 revision 的长度界限，声明仍不一致；spdlog/fmt 的临时 mapper、CT2 的形状边界告警尚未得到覆盖所有输入的证明。clog 忽略 write 返回值，可能丢失日志；其余还有未使用参数、初始化顺序、fallthrough 和注释警告。没有关闭这些告警，也没有把两轮推理通过当作所有上游问题已解决。

## 参考

- [Arch makepkg 手册](https://man.archlinux.org/man/makepkg.8)：构建、日志与包管理参数。
- [Arch PKGBUILD 手册](https://man.archlinux.org/man/PKGBUILD.5)：依赖、冲突和打包函数。
- [Fcitx5-Rime 源码](https://github.com/fcitx/fcitx5-rime)：前端构建要求和通知处理。
