# VS Code 便携版安全更新器

下载微软官方 Windows ZIP 版 VS Code，验证 SHA-256 后安装到指定目录。支持 **x64 / arm64**、**stable / insider**。

> 更新前请关闭目标目录中的 VS Code，并在更新期间保持关闭。请把更新器放在安装目录外运行。不要以管理员身份运行，除非目标目录确实需要管理员权限。

## 功能

- **按构建检查更新**：比较产品版本、commit ID、渠道和实际 EXE 架构，不会因产品版本号相同而漏掉 Insiders 更新或架构切换。
- **验证下载**：检查 HTTP 状态、每段 Content-Range / 长度，并使用官方更新接口提供的 `sha256hash` 校验整个 ZIP。
- **有界并发**：下载、解压均最多 16 个线程；不支持 Range 时退回顺序下载。失败分段单独重试，必要时整体重试一次。
- **提前预审目录与边下边解**：提前拉取压缩包尾部中央目录，在下载实质数据前 0 流量完成路径穿越、保留名与解压体积预审；采用流水线边下载边流式解压，省去整包落盘与后续重头解压时间，同时流式累计计算全包 SHA-256。
- **安全解压与降级保护**：严格限制解压大小、拦截特殊文件；若服务器不支持分段或压缩包非顺序排列，自动平滑退回传统完整下载验证流程。
- **先准备再切换**：在同一磁盘的独立临时目录下载、解压并验证，旧安装不会在解压前被删除。
- **便携数据保留**：将旧安装的 `data` 目录复制到新安装；原 `data` 留在旧版本备份中。
- **回滚与备份**：切换失败时尝试还原旧目录。成功后也保留旧版本，不自动删除用户的备份。
- **脚本友好**：默认不等待按键；重定向输出时不绘制动态进度，不输出 ANSI 转义符。
- **运行时无需额外安装第三方库**：miniz 与 nlohmann/json 编入可执行文件，网络和 SHA-256 使用 Windows 系统库。

## 使用

```bat
rem 默认安装到当前工作目录的 Application，自动选择本机原生架构
vscode_updater.exe

rem 仅检查，不下载、不修改安装目录
vscode_updater.exe --check

rem 指定路径、架构、渠道或线程数
vscode_updater.exe --dir "D:\开发工具\VSCode"
vscode_updater.exe --arch arm64
vscode_updater.exe --quality insider
vscode_updater.exe --threads 8

rem 强制重新安装，但不绕过任何安全校验
vscode_updater.exe --force

rem 保留安装包；需要交互时显式等待按键
vscode_updater.exe --keep-zip
vscode_updater.exe --pause
vscode_updater.exe --help
```

| 参数 | 说明 |
|---|---|
| `--dir <路径>` | 默认 `./Application`；必须是新目录、空目录或可识别的 VS Code 安装目录 |
| `--arch x64\|arm64` | 默认本机原生架构；CI 分发的更新器本身为 x64 EXE |
| `--quality stable\|insider` | 默认 `stable` |
| `--threads <1-16>` | 默认按硬件线程数选择，最多 8；显式设置最多 16 |
| `--check` | 仅查询更新；有无更新通过输出区分 |
| `--force` | 即使构建相同也重新安装 |
| `--keep-zip` | 保留已验证的 ZIP，程序会输出实际路径 |
| `--pause` | 仅在交互终端中等待按键 |
| `--no-pause` | 不等待按键，默认行为 |

退出码：`0` = 操作/检查成功（不代表一定有更新）；`1` = 网络、验证、磁盘或安装错误；`2` = 参数错误。

网络使用 Windows 系统代理设置。连接、发送及接收操作设置 30 秒超时；版本查询另有 2 分钟整体截止时间，单个下载请求为 15 分钟（在读操作边界检查）。官方元数据查询失败或缺少 SHA-256 时**停止安装**，不再退回未经校验的下载方式。

### 目录限制与数据保留范围

- 拒绝磁盘根目录、系统目录、用户资料根目录、当前工作目录及其上级、包含更新器自身的目录、UNC/设备路径。
- 目标路径及其父目录不能包含符号链接或 junction；便携版 `data` 中存在链接时也会停止，而不是跟随链接复制外部文件。
- 检测到目标 VS Code 正在运行时拒绝更新；无法检查相关进程时也会停止，请先关闭它。
- 同一目标使用独占锁，防止本更新器的多个实例同时安装。
- 自动迁移的用户文件仅限 `data`。其他自定义文件仍保存在旧版本备份中，需要时手动迁移。
- **新安装不会自动创建 `data`**。要启用 VS Code 的便携模式，可在安装完成后、首次启动前，在安装目录创建 `data` 文件夹。
- Windows 系统用户目录中的 VS Code 配置不在本程序的替换范围内。

## 安全更新流程

```text
查询官方元数据（产品版本、commit、下载地址、SHA-256）
  → 创建独占锁与唯一临时目录
  → 下载、检查 HTTP/Range、验证 SHA-256
  → 检查所有 ZIP 条目与磁盘可用空间
  → 解压到 staged，并校验产品版本 / commit / 渠道 / EXE 架构
  → 复制便携版 data，写入构建信息
  → 旧目录重命名为 previous
  → staged 重命名为目标目录；失败时尝试还原 previous
  → 保留旧版本备份并报告路径
```

临时/恢复目录与目标目录位于同一父目录，例如：

```text
D:\Tools\Application\                         新安装
D:\Tools\.Application.update-<随机值>\
    previous\                                完整旧安装及原 data（如有）
    recovery.json                            目标和备份位置、阶段信息
    vscode.zip                               仅 --keep-zip 时在成功后保留
```

确认新版本及用户数据正常后，可以手动删除不再需要的 `.Application.update-*` 备份目录。备份可能包含个人配置，**不要上传到仓库或共享给他人**。

### 故障恢复与边界

- 下载、校验或解压失败：旧安装不变；自动清理本次独占创建的临时目录。
- 切换失败：尝试自动回滚，保留恢复文件并打印位置。
- 回滚也失败：不要删除恢复目录；先关闭 VS Code，再依据 `recovery.json` 找到 `previous` 并手动还原。若目标目录已存在，先另行备份它，不要直接覆盖。
- **两次目录重命名不是掉电原子事务**。断电或强制终止可能留下备份和 staging，程序不会自动猜测并覆盖它们。旧文件仍可能需要从恢复目录手动还原。
- 复制 `data` 会临时占用额外磁盘空间；成功后旧版本备份也会持续占用空间。
- 默认防护上限：ZIP 2 GiB、展开总量 20 GiB、单文件 4 GiB、20 万条目。超过上限会拒绝安装。

## 编译

Windows 上运行 `build.bat`，自动优先使用 MinGW-w64；没有 g++ 时使用当前开发者终端中的 MSVC。

```bat
rem MinGW-w64（C++17，建议较新的工具链）
g++ -std=c++17 -O2 -Wall -Wextra -static update.cpp -lwininet -lbcrypt -lshell32 -o vscode_updater.exe

rem MSVC：在 x64 Native Tools Command Prompt 中运行
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W4 /MT update.cpp /Fe:vscode_updater.exe /link wininet.lib bcrypt.lib shell32.lib
```

也可使用 `make`。三种编译入口链接的系统库保持一致。当前 CI 不生成 arm64 原生更新器；`--arch arm64` 选择的是要安装的 VS Code 架构。

## 测试

平台无关回归测试覆盖参数、构建身份、HTTP 分段、网络中断、写入失败、ZIP 路径和切换/回滚故障注入：

```sh
g++ -std=c++17 -O2 -Wall -Wextra tests/core_tests.cpp -o core-tests
./core-tests
```

Windows 集成测试直接复用生产实现，覆盖 SHA-256、真实 ZIP/CRC、Unicode 路径、便携数据复制和真实目录回滚。测试 ZIP 均在本地生成，不下载 VS Code：

```bat
python tests\make_fixtures.py tests\fixtures

rem MinGW
make all core-tests.exe windows-tests.exe
core-tests.exe
windows-tests.exe tests\fixtures
python tests\smoke.py .\vscode_updater.exe

rem 或在 MSVC x64 开发者终端运行
call tests\run_msvc.bat
```

测试均不进行真实安装。发布前仍建议在 Windows 上对测试目录执行完整下载/更新，并人工测试文件占用、权限限制、代理、断网和磁盘空间不足等场景。

## CI 与发布

- 普通 push、PR、手动触发都会运行 Linux 回归测试，以及 Windows MinGW / MSVC 编译与离线测试。
- 第三方 Actions 固定到 commit SHA；普通测试仅有 `contents: read` 权限。
- MinGW 构建提供 `vscode-updater-windows-x64` Artifact，包含 EXE 与 `SHA256SUMS.txt`。
- 只有推送 `v*` tag 且全部测试成功后，独立发布任务才有 `contents: write` 权限。
- **发布任务只上传到已经存在的 Release，不自动用机器人身份创建 Release。** 请先用自己的 GitHub 账号创建相应 Release。若 tag 构建先完成导致发布任务提示 Release 不存在，创建后重新运行失败任务即可。
- 上传同名 EXE 和校验文件会覆盖原附件；已有 Release 的发布者不变。
- 普通分支推送和手动构建不会自动发布，不应为发布更新而移动已有版本 tag。

PowerShell 验证下载附件：

```powershell
(Get-FileHash .\vscode_updater.exe -Algorithm SHA256).Hash.ToLowerInvariant()
# 与 SHA256SUMS.txt 中的哈希比对
```

## 文件结构

```text
update.cpp             Windows 网络、解压、验证和安装流程
updater_core.hpp        平台无关校验、参数解析和切换状态机
tests/                 离线回归测试、ZIP 测试数据生成器
miniz.c / miniz.h       内置 ZIP 库，保留上游许可
json.hpp               内置 nlohmann/json，MIT 许可
Makefile / build.bat    编译入口
.github/workflows/     编译、测试和发布流程
```
