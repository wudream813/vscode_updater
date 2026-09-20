# VS Code 自动更新程序

自动下载最新版 **VS Code**（Windows x64 / arm64），并解压到 `Application` 目录，无需安装、即解即用。

## 功能特性

- 🔄 **版本检测**：通过官方更新接口对比本地版本，已是最新则自动跳过下载，不浪费流量
- 🚀 **多线程分段下载**：线程数按文件大小自动调整（每块至少 256KB，最多 16 线程）
- ⚡ **多线程解压**：按文件大小智能均衡各线程负载，避免大文件扎堆
- 🎨 **实时进度条**：下载速度、剩余时间、文件数、解压速度一目了然
- 🛡️ **健壮性**：网络超时保护、失败自动重试（最多 3 次）、下载完整性校验、失败自动清理
- 🖥️ 支持 `x64` / `arm64` 架构，`stable` / `insider` 渠道
- 📦 单文件分发：内置 miniz 与 JSON 解析，无任何第三方依赖

## 使用

```bat
vscode_updater.exe                 :: 下载最新版并解压到 Application
vscode_updater.exe --check         :: 仅检查是否有新版本
vscode_updater.exe --force         :: 跳过版本检查，强制重新下载
vscode_updater.exe --arch arm64    :: 下载 arm64 版本
vscode_updater.exe --quality insider
vscode_updater.exe --dir D:\vscode :: 解压到指定目录
vscode_updater.exe --threads 8     :: 指定下载/解压线程数
vscode_updater.exe --keep-zip      :: 解压后保留 vscode.zip
```

## 编译

Windows 上直接双击运行 `build.bat`（自动选择 MinGW 或 MSVC）。

手动编译：

```bat
rem MinGW-w64（推荐）
g++ -std=c++17 -O2 -Wall -static update.cpp -lwininet -o vscode_updater.exe

rem MSVC（需 VS 开发环境）
cl /nologo /EHsc /std:c++17 /O2 /utf-8 update.cpp /link wininet.lib
```

## 目录结构

```
update.cpp   主程序（单文件，内含 miniz 与 JSON 解析）
miniz.c/h    解压库（miniz，MIT 许可）
json.hpp     JSON 解析（nlohmann/json，MIT 许可）
Makefile     编译脚本（MinGW）
build.bat    Windows 编译脚本
```

## 工作原理

1. 调用官方更新接口 `update.code.visualstudio.com/api/update/...` 获取最新版本号与下载直链
2. 对比本地 `Application/resources/app/product.json` 中的版本号，相同则跳过
3. 多线程按 `Range` 分段下载，实时显示速度与剩余时间
4. 下载完成校验字节数后，删除旧目录并按文件大小均衡地多线程解压
5. 清理压缩包，输出 VS Code 启动路径 `Application\Code.exe`
