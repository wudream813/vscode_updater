// ============================================================================
//  VS Code 自动更新程序
//  ----------------------------------------------------------------------------
//  功能:
//    * 通过官方更新接口检测最新版本，已是最新时自动跳过下载
//    * 多线程分段下载（线程数按文件大小自动调整）
//    * 多线程解压，按文件大小智能均衡各线程负载
//    * 实时进度条：下载速度、剩余时间、文件数、解压速度
//    * 支持 x64 / arm64 架构，stable / insider 渠道
//
//  编译 (MinGW-w64, 推荐):
//      g++ -std=c++17 -O2 -Wall -static update.cpp -lwininet -o vscode_updater.exe
//  编译 (MSVC):
//      cl /nologo /EHsc /std:c++17 /O2 /utf-8 update.cpp /link wininet.lib
//  或在 Windows 上直接运行 build.bat
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// 单文件分发：miniz 源码直接编进本文件，无需单独编译 miniz.c
#include "miniz.c"
#include "json.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#pragma execution_character_set("utf-8")
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono;

// --- 终端配色方案（ANSI 真彩色）---
#define R_RESET   "\033[0m"
#define R_BOLD    "\033[1m"
#define R_CYAN    "\033[38;5;51m"
#define R_GREEN   "\033[38;5;82m"
#define R_RED     "\033[38;5;196m"
#define R_YELLOW  "\033[38;5;226m"
#define R_GRAY    "\033[38;5;245m"
#define R_PURPLE  "\033[38;5;147m"

// --- 状态结构体（下载 / 解压共用的进度槽）---
struct ProgressSlot {
    std::atomic<size_t> current{0};     // 已处理的字节数 / 文件数
    std::atomic<bool>   failed{false};  // 该块是否出错
    std::atomic<bool>   finished{false};// 该块是否完成
    size_t              total{0};       // 本块总量（启动线程前写入）
};

std::mutex g_ui_mutex;
steady_clock::time_point g_start_time;

// --- 控制台初始化 ---
void setupConsole() {
    // 统一使用 UTF-8，避免不同系统区域设置下的中文乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
    }
    std::ios::sync_with_stdio(false);
    std::cout << "\033[?25l"; // 隐藏光标
}

void restoreConsole() {
    std::cout << "\033[?25h" << R_RESET; // 恢复光标与颜色
}

// --- 工具函数 ---
std::string fmtSize(size_t bytes) {
    char buf[64];
    if (bytes >= 1073741824ULL)
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

// 按终端显示宽度（CJK 字符占 2 列）居中
std::string center(const std::string& s, int width) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xE0) == 0xC0) { w += 2; ++i; }
        else if ((c & 0xF0) == 0xE0) { w += 2; i += 2; }
        else if ((c & 0xF8) == 0xF0) { w += 2; i += 3; }
        else ++w;
    }
    int pad = std::max(0, width - w);
    return std::string(pad / 2, ' ') + s + std::string(pad - pad / 2, ' ');
}

// 多块进度 UI：slots.size() 个分块进度条 + 总进度条，支持下载(字节)与解压(文件)两种模式
void drawMultiBlockUI(const std::vector<ProgressSlot>& slots, size_t globalTotal,
                      const std::string& title, bool isDownload, bool firstDraw) {
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);

    double duration_sec = duration_cast<milliseconds>(steady_clock::now() - g_start_time).count() / 1000.0;

    if (!firstDraw) os << "\033[" << (slots.size() + 3) << "A"; // 光标上移，覆盖上一帧

    // 1. 标题
    os << " >>> " << R_BOLD << R_CYAN << title << R_RESET << "\033[K\n";
    os << R_GRAY << " ---------------------------------------------------" << R_RESET << "\033[K\n";

    // 2. 各分块进度
    size_t globalCurrent = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        double percent = (slots[i].total > 0) ? (double)slots[i].current / slots[i].total : 0.0;
        globalCurrent += slots[i].current;

        std::string status;
        if (slots[i].finished) status = slots[i].failed ? (R_RED "FAIL" R_RESET) : (R_GREEN " OK " R_RESET);
        else status = R_YELLOW " >> " R_RESET;

        os << " " << status << R_GRAY << "Block #" << i << R_RESET << " [";
        int width = 25;
        int pos = (int)(width * percent);
        for (int j = 0; j < width; ++j) {
            if (j < pos) os << R_CYAN "=";
            else if (j == pos && !slots[i].finished) os << R_CYAN ">";
            else os << R_GRAY "-";
        }
        os << R_RESET << "] " << std::setw(5) << (percent * 100.0) << "%\033[K\n";
    }

    // 3. 总进度 + 速度 / 剩余时间
    if (globalTotal == 0) {
        os << " " << R_BOLD << R_PURPLE << "TOTAL" << R_RESET << "  "
           << R_GRAY << "已下载 " << fmtSize(globalCurrent) << R_RESET << "\033[K\n";
    } else {
        double totalPercent = (double)globalCurrent / globalTotal;
        os << " " << R_BOLD << R_PURPLE << "TOTAL" << R_RESET << " [";
        int tWidth = 25;
        int tPos = (int)(tWidth * totalPercent);
        for (int j = 0; j < tWidth; ++j) {
            if (j < tPos) os << R_PURPLE "#";
            else os << R_GRAY ".";
        }
        os << R_PURPLE << "]" << R_BOLD << " " << std::setw(3) << (int)(totalPercent * 100) << "% ";

        if (isDownload) {
            double speed = (duration_sec > 0.1) ? (double)globalCurrent / 1048576.0 / duration_sec : 0.0;
            os << R_RESET << R_GRAY << "(" << fmtSize(globalCurrent) << " / " << fmtSize(globalTotal) << ") "
               << R_YELLOW << std::setw(5) << speed << " MB/s";
            if (speed > 0.01 && globalCurrent < globalTotal) {
                double eta = (double)(globalTotal - globalCurrent) / 1048576.0 / speed;
                os << R_GRAY << " 剩余 " << (int)(eta / 60.0) << ":"
                   << std::setw(2) << std::setfill('0') << (int)eta % 60 << std::setfill(' ');
            }
        } else {
            double f_speed = (duration_sec > 0.1) ? (double)globalCurrent / duration_sec : 0.0;
            os << R_RESET << R_GRAY << "(" << globalCurrent << "/" << globalTotal << " 文件) "
               << R_YELLOW << (int)f_speed << " 文件/秒";
        }
        os << R_RESET << "\033[K\n";
    }
    std::cout << os.str() << std::flush;
}

// --- HTTP 工具（WinINet）---
namespace http {

// 设置连接与读写超时，避免网络异常时永久挂起
void applyTimeouts(HINTERNET h, DWORD ms) {
    InternetSetOption(h, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
    InternetSetOption(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    InternetSetOption(h, INTERNET_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
}

// 读取 URL 全部内容（用于版本查询等小响应）
bool getString(const std::string& url, std::string& body) {
    body.clear();
    HINTERNET hInt = InternetOpenA("VSCodeUpdater/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInt) return false;
    applyTimeouts(hInt, 30000);
    HINTERNET hUrl = InternetOpenUrlA(hInt, url.c_str(), NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    bool ok = false;
    if (hUrl) {
        char buf[8192];
        DWORD read = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
            body.append(buf, read);
        ok = !body.empty();
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hInt);
    return ok;
}

// 查询资源大小（Content-Length）
bool getContentLength(const std::string& url, size_t& size) {
    size = 0;
    HINTERNET hInt = InternetOpenA("VSCodeUpdater/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInt) return false;
    applyTimeouts(hInt, 30000);
    HINTERNET hUrl = InternetOpenUrlA(hInt, url.c_str(), NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    bool ok = false;
    if (hUrl) {
        char szLen[64];
        DWORD len = sizeof(szLen);
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH, szLen, &len, NULL)) {
            size = (size_t)std::strtoull(szLen, nullptr, 10);
            ok = size > 0;
        }
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hInt);
    return ok;
}

} // namespace http

// --- 版本号 ---
struct Version {
    std::vector<int> parts;  // 主.次.修订.构建
    std::string raw;

    static Version parse(const std::string& s) {
        Version v;
        v.raw = s;
        std::string cur;
        for (char c : s) {
            if (c >= '0' && c <= '9') cur += c;
            else if (c == '.' && !cur.empty()) { v.parts.push_back(std::stoi(cur)); cur.clear(); }
            else if (c == '.') { /* 忽略连续点 */ }
            else break; // 后缀如 "-insider" 直接忽略
        }
        if (!cur.empty()) v.parts.push_back(std::stoi(cur));
        while (v.parts.size() < 4) v.parts.push_back(0);
        return v;
    }

    bool operator<(const Version& o) const {
        for (size_t i = 0; i < 4; ++i)
            if (parts[i] != o.parts[i]) return parts[i] < o.parts[i];
        return false;
    }
};

// 读取本地安装版本（Application/resources/app/product.json 中的 version 字段）
std::string readLocalVersion(const fs::path& appDir) {
    try {
        fs::path pf = appDir / "resources" / "app" / "product.json";
        std::ifstream ifs(pf, std::ios::binary);
        if (!ifs) return "";
        json j = json::parse(ifs);
        return j.value("version", "");
    } catch (...) {
        return "";
    }
}

struct RemoteInfo {
    std::string version;
    std::string url;  // 直链下载地址
    bool ok = false;
};

// 通过官方更新接口获取最新版本号与下载直链
RemoteInfo fetchRemoteInfo(const std::string& apiUrl) {
    RemoteInfo info;
    std::string body;
    if (!http::getString(apiUrl, body)) return info;
    try {
        json j = json::parse(body);
        info.version = j.value("productVersion", j.value("version", ""));
        info.url = j.value("url", "");
        info.ok = !info.version.empty();
    } catch (...) {}
    return info;
}

// --- 下载逻辑 ---
// 每个线程下载一段 Range，独立打开文件句柄写各自区间
void dlWorker(const std::string& url, const std::string& path, int id,
              std::vector<ProgressSlot>& slots, size_t startOffset) {
    auto& slot = slots[id];
    HINTERNET hInt = InternetOpenA("VSCodeUpdater/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInt) { slot.failed = true; slot.finished = true; return; }
    http::applyTimeouts(hInt, 30000);

    std::string range = "Range: bytes=" + std::to_string(startOffset) + "-" +
                        std::to_string(startOffset + slot.total - 1) + "\r\n";
    HINTERNET hUrl = InternetOpenUrlA(hInt, url.c_str(), range.c_str(), (DWORD)-1L,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (hUrl) {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        if (f) {
            f.seekp(startOffset);
            char buf[131072];
            DWORD read = 0;
            while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
                f.write(buf, read);
                slot.current += read;
            }
            if (!f.good()) slot.failed = true;
        } else {
            slot.failed = true;
        }
        InternetCloseHandle(hUrl);
    } else {
        slot.failed = true;
    }
    InternetCloseHandle(hInt);
    slot.finished = true; // 无论成败都必须置位，避免主循环死等
}

// 无 Content-Length 时的降级方案：单线程顺序下载
bool runDownloadStream(const std::string& url, const std::string& path) {
    HINTERNET hInt = InternetOpenA("VSCodeUpdater/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInt) return false;
    http::applyTimeouts(hInt, 60000);
    HINTERNET hUrl = InternetOpenUrlA(hInt, url.c_str(), NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    bool ok = false;
    if (hUrl) {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (ofs) {
            char buf[131072];
            DWORD read = 0;
            size_t total = 0;
            while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
                ofs.write(buf, read);
                total += read;
                std::cout << "\r  正在下载... " << fmtSize(total) << "        " << std::flush;
            }
            ok = ofs.good() && total > 0;
            std::cout << "\n";
        }
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hInt);
    return ok;
}

bool runDownload(const std::string& url, const std::string& path, int threads) {
    size_t totalSize = 0;
    if (!http::getContentLength(url, totalSize)) {
        std::cout << R_YELLOW << "  [i] 服务器未返回文件大小，切换为单线程流式下载..." << R_RESET << "\n";
        return runDownloadStream(url, path);
    }

    // 每块至少 256KB，最多 16 个线程
    threads = std::max(1, threads);
    threads = (int)std::min<size_t>((size_t)threads, std::max<size_t>(1, totalSize / (256 * 1024)));
    threads = std::min(threads, 16);

    // 预创建并分配文件大小，避免写入时反复扩展
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cout << R_RED << "  [x] 无法创建文件: " << path << R_RESET << "\n";
            return false;
        }
    }
    std::error_code ec;
    fs::resize_file(path, totalSize, ec);
    if (ec) {
        std::cout << R_RED << "  [x] 无法预分配磁盘空间，请检查磁盘空间" << R_RESET << "\n";
        return false;
    }

    std::vector<ProgressSlot> slots(threads);
    size_t chunk = totalSize / threads;
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; ++i) {
        slots[i].total = (i == threads - 1) ? (totalSize - (size_t)i * chunk) : chunk;
        workers.emplace_back(dlWorker, url, path, i, std::ref(slots), (size_t)i * chunk);
    }

    bool first = true;
    while (true) {
        drawMultiBlockUI(slots, totalSize, "多线程下载中", true, first);
        first = false;
        bool allDone = true;
        for (auto& s : slots)
            if (!s.finished) allDone = false;
        if (allDone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (auto& t : workers) t.join();

    // 校验：所有分块之和必须等于总大小，且没有任何分块报错
    size_t downloaded = 0;
    for (auto& s : slots) downloaded += s.current;
    bool ok = (downloaded == totalSize) &&
              !std::any_of(slots.begin(), slots.end(),
                           [](const ProgressSlot& s) { return s.failed.load(); });
    if (ok)
        std::cout << "\n\n" << R_GREEN << "  [+] 下载成功: " << fmtSize(totalSize) << R_RESET << "\n";
    else
        std::cout << "\n\n" << R_RED << "  [x] 下载不完整: " << fmtSize(downloaded)
                  << " / " << fmtSize(totalSize) << R_RESET << "\n";
    return ok;
}

// --- 解压逻辑 ---
void extractWorker(const fs::path& zip, const fs::path& dir, int id,
                   std::vector<ProgressSlot>& slots, const std::vector<mz_uint>& myFiles) {
    auto& slot = slots[id];
    mz_zip_archive local;
    memset(&local, 0, sizeof(local));
    if (!mz_zip_reader_init_file(&local, zip.string().c_str(), 0)) {
        slot.failed = true;
        slot.finished = true;
        return;
    }
    for (mz_uint fileIdx : myFiles) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&local, fileIdx, &st)) {
            fs::path out = dir / st.m_filename;
            std::error_code ec;
            if (mz_zip_reader_is_file_a_directory(&local, fileIdx)) {
                fs::create_directories(out, ec);
            } else {
                fs::create_directories(out.parent_path(), ec);
                if (!mz_zip_reader_extract_to_file(&local, fileIdx, out.string().c_str(), 0))
                    slot.failed = true;
            }
        }
        slot.current++;
    }
    slot.finished = true;
    mz_zip_reader_end(&local);
}

bool runDecompress(const fs::path& zip, const fs::path& dir, int threads) {
    mz_zip_archive reader;
    memset(&reader, 0, sizeof(reader));
    if (!mz_zip_reader_init_file(&reader, zip.string().c_str(), 0)) {
        std::cout << R_RED << "  [x] 无法打开压缩包: " << zip << R_RESET << "\n";
        return false;
    }
    mz_uint totalFiles = mz_zip_reader_get_num_files(&reader);
    if (totalFiles == 0) {
        mz_zip_reader_end(&reader);
        std::cout << R_RED << "  [x] 压缩包为空" << R_RESET << "\n";
        return false;
    }

    threads = (int)std::min<mz_uint>((mz_uint)std::max(1, threads), totalFiles);

    // 收集每个文件的解压后大小，按大小降序，采用“最空闲线程优先”策略分配，
    // 避免小文件扎堆、大文件集中在少数线程上
    std::vector<std::pair<mz_uint, mz_uint64>> jobs;
    jobs.reserve(totalFiles);
    for (mz_uint i = 0; i < totalFiles; ++i) {
        mz_zip_archive_file_stat st;
        jobs.emplace_back(i, mz_zip_reader_file_stat(&reader, i, &st) ? st.m_uncomp_size : 0);
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<ProgressSlot> slots(threads);
    std::vector<std::vector<mz_uint>> workerFiles(threads);
    std::vector<mz_uint64> load(threads, 0);
    for (auto& job : jobs) {
        int wid = (int)(std::min_element(load.begin(), load.end()) - load.begin());
        workerFiles[wid].push_back(job.first);
        load[wid] += job.second;
    }
    for (int i = 0; i < threads; ++i) slots[i].total = workerFiles[i].size();

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; ++i)
        workers.emplace_back(extractWorker, zip, dir, i, std::ref(slots), workerFiles[i]);

    bool first = true;
    while (true) {
        drawMultiBlockUI(slots, totalFiles, "多线程解压中", false, first);
        first = false;
        bool allDone = true;
        for (auto& s : slots)
            if (!s.finished) allDone = false;
        if (allDone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (auto& t : workers) t.join();
    mz_zip_reader_end(&reader);

    bool ok = !std::any_of(slots.begin(), slots.end(),
                           [](const ProgressSlot& s) { return s.failed.load(); });
    if (ok)
        std::cout << "\n\n" << R_GREEN << "  [+] 解压成功: " << totalFiles << " 个文件" << R_RESET << "\n";
    else
        std::cout << "\n\n" << R_RED << "  [x] 解压过程中出现错误（磁盘空间或权限不足?）" << R_RESET << "\n";
    return ok;
}

// --- 界面与入口 ---
void pressAnyKey() {
    std::cout << "\n  按任意键退出...";
    _getch();
    std::cout << "\n";
}

void printUsage(const char* exe) {
    std::cout << R_CYAN << "  VS Code 自动更新程序" << R_RESET << "\n"
              << R_GRAY << "  用法: " << exe << " [选项]" << R_RESET << "\n\n"
              << "  选项:\n"
              << "    --dir <路径>      解压目录（默认 ./Application）\n"
              << "    --arch <架构>     x64 | arm64（默认 x64）\n"
              << "    --quality <渠道>  stable | insider（默认 stable）\n"
              << "    --threads <N>     下载/解压线程数（默认自动）\n"
              << "    --force           跳过版本检查，强制重新下载\n"
              << "    --check           仅检查是否有新版本\n"
              << "    --keep-zip        解压后保留 vscode.zip\n"
              << "    --help, -h        显示本帮助\n";
}

void printBanner(const std::string& arch, const std::string& quality, const std::string& appDir) {
    std::ostringstream os;
    os << "\n  " << R_BOLD << R_CYAN
       << "╔═══════════════════════════════════════════╗\n"
       << "║" << center("VS CODE 自动更新程序", 43) << "║\n"
       << "╚═══════════════════════════════════════════╝" << R_RESET << "\n"
       << R_GRAY << "  架构: " << arch << " | 渠道: " << quality << " | 目标目录: " << appDir << R_RESET << "\n\n";
    std::cout << os.str();
}

int main(int argc, char** argv) {
    setupConsole();

    std::string appDir = "./Application";
    std::string arch = "x64";
    std::string quality = "stable";
    int threads = 6;
    bool threadsGiven = false;
    bool force = false, checkOnly = false, keepZip = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                std::cout << R_RED << "  [错误] " << flag << " 需要一个参数" << R_RESET << "\n";
                return "";
            }
            return argv[++i];
        };
        if (a == "--dir") appDir = need(a);
        else if (a == "--arch") arch = need(a);
        else if (a == "--quality") quality = need(a);
        else if (a == "--threads") { threads = std::max(1, std::atoi(need(a).c_str())); threadsGiven = true; }
        else if (a == "--force") force = true;
        else if (a == "--check") checkOnly = true;
        else if (a == "--keep-zip") keepZip = true;
        else if (a == "--help" || a == "-h") { printUsage(argv[0]); restoreConsole(); return 0; }
        else std::cout << R_YELLOW << "  [提示] 未知参数: " << a << R_RESET << "\n";
    }

    if (appDir.empty()) {
        std::cout << R_RED << "  [错误] --dir 不能为空" << R_RESET << "\n";
        pressAnyKey(); restoreConsole(); return 1;
    }
    if (arch != "x64" && arch != "arm64") {
        std::cout << R_RED << "  [错误] 不支持的架构: " << arch << "（可选 x64 / arm64）" << R_RESET << "\n";
        pressAnyKey(); restoreConsole(); return 1;
    }
    if (quality != "stable" && quality != "insider") {
        std::cout << R_RED << "  [错误] 不支持的渠道: " << quality << "（可选 stable / insider）" << R_RESET << "\n";
        pressAnyKey(); restoreConsole(); return 1;
    }

    std::string os = "win32-" + arch + "-archive";
    std::string apiUrl = "https://update.code.visualstudio.com/api/update/" + os + "/" + quality + "/latest";
    std::string fallbackUrl = "https://code.visualstudio.com/sha/download?build=" + quality + "&os=" + os;
    std::string zipFile = (fs::path(appDir).parent_path() / "vscode.zip").string();

    printBanner(arch, quality, appDir);

    // 1) 查询远程版本信息（版本号 + 直链下载地址）
    RemoteInfo remote;
    std::cout << R_GRAY << "  [i] 正在查询最新版本..." << R_RESET << "\n";
    remote = fetchRemoteInfo(apiUrl);
    if (remote.ok) {
        std::cout << R_GRAY << "  [i] 最新版本: " << R_RESET << R_BOLD << remote.version << R_RESET << "\n";
    } else {
        std::cout << R_YELLOW << "  [i] 无法获取版本信息（网络受限?），将直接下载。" << R_RESET << "\n";
    }

    if (checkOnly) {
        std::string local = readLocalVersion(appDir);
        if (!remote.ok) {
            std::cout << R_RED << "  [x] 检查失败：无法获取远程版本信息。" << R_RESET << "\n";
        } else if (local.empty()) {
            std::cout << R_GREEN << "  [i] 本地未安装，最新版本: " << remote.version << R_RESET << "\n";
        } else {
            Version lv = Version::parse(local), rv = Version::parse(remote.version);
            if (lv < rv)
                std::cout << R_YELLOW << "  [i] 发现新版本: " << local << " -> " << remote.version << R_RESET << "\n";
            else
                std::cout << R_GREEN << "  [i] 已是最新版本: " << local << R_RESET << "\n";
        }
        pressAnyKey(); restoreConsole(); return 0;
    }

    // 2) 版本检查：已是最新则跳过下载
    if (!force) {
        std::string local = readLocalVersion(appDir);
        if (!local.empty()) {
            if (!remote.ok) {
                std::cout << R_YELLOW << "  [i] 无法对比版本（无法获取远程版本），将重新下载。" << R_RESET << "\n";
            } else {
                Version lv = Version::parse(local), rv = Version::parse(remote.version);
                if (!(lv < rv)) {
                    std::cout << R_GREEN << "  [✓] 已是最新版本 (" << local << ")，无需更新。" << R_RESET << "\n\n";
                    pressAnyKey(); restoreConsole(); return 0;
                }
                std::cout << R_YELLOW << "  [i] 发现新版本: " << local << " -> " << remote.version << R_RESET << "\n";
            }
        } else {
            std::cout << R_YELLOW << "  [i] 未检测到本地版本，将下载最新版。" << R_RESET << "\n";
        }
    } else {
        std::cout << R_YELLOW << "  [i] --force: 跳过版本检查，强制重新下载。" << R_RESET << "\n";
    }

    // 3) 下载（失败自动重试最多 3 次）
    std::string dlUrl = (remote.ok && !remote.url.empty()) ? remote.url : fallbackUrl;
    int dlThreads = threadsGiven ? threads : std::min(6, std::max(1, (int)std::thread::hardware_concurrency()));
    std::cout << "\n";

    bool downloaded = false;
    for (int attempt = 1; attempt <= 3 && !downloaded; ++attempt) {
        if (attempt > 1) {
            std::cout << R_YELLOW << "  [i] 第 " << attempt << " 次尝试下载..." << R_RESET << "\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        g_start_time = steady_clock::now();
        downloaded = runDownload(dlUrl, zipFile, dlThreads);
    }
    if (!downloaded) {
        std::cout << R_RED << "\n  [x] 下载失败！请检查网络后重试。" << R_RESET << "\n";
        std::error_code ec;
        fs::remove(zipFile, ec); // 清理不完整的文件
        pressAnyKey(); restoreConsole(); return 1;
    }

    // 4) 清理旧目录并解压
    std::error_code ec;
    std::cout << "  [i] 正在清理旧版本目录: " << appDir << " ...\n";
    if (fs::exists(appDir, ec)) fs::remove_all(appDir, ec);
    fs::create_directories(appDir, ec);

    int exThreads = threadsGiven ? threads : std::min(8, std::max(1, (int)std::thread::hardware_concurrency()));
    g_start_time = steady_clock::now();
    bool ok = runDecompress(zipFile, appDir, exThreads);

    if (!keepZip) {
        std::error_code ec2;
        fs::remove(zipFile, ec2);
    } else {
        std::cout << R_GRAY << "  [i] 已保留压缩包: " << zipFile << R_RESET << "\n";
    }

    if (ok) {
        std::cout << R_GREEN << "\n  [✓] 更新完成！" << R_RESET;
        if (remote.ok) std::cout << R_GREEN << " 当前版本: " << remote.version << R_RESET;
        std::cout << "\n      启动: " << (fs::path(appDir) / "Code.exe").string() << "\n";
    } else {
        std::cout << R_RED << "\n  [x] 解压过程出现错误，请检查磁盘空间和权限。" << R_RESET << "\n";
    }

    pressAnyKey();
    restoreConsole();
    return ok ? 0 : 1;
}
