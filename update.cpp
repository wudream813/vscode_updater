// Windows portable VS Code updater. See README.md for safety and recovery rules.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <conio.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "updater_core.hpp"
#include "miniz.c"
#include "json.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using updater::require;
using namespace std::chrono_literals;

namespace {
bool interactiveOutput = false;


int cwidth(char32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x80) return 1;
    // East Asian Wide / Fullwidth characters
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

int strDisplayWidth(const std::string& s) {
    int w = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > s.size()) break;
        for (int j = 1; j < len; ++j) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
        }
        i += len;
        w += cwidth(cp);
    }
    return w;
}

std::string padBoxLine(const std::string& content, int innerWidth) {
    int currentWidth = strDisplayWidth(content);
    int pad = innerWidth - currentWidth;
    if (pad < 0) pad = 0;
    return "  │" + content + std::string(pad, ' ') + "│\n";
}


std::atomic<bool> g_interruptRequested{false};

enum class StepStatus {
    Pending,
    Active,
    Success,
    Failed,
    Skipped
};

struct StepInfo {
    std::string name;
    std::string detail;
    StepStatus status = StepStatus::Pending;
};

class ModernUI {
public:
    bool enabled = false;
    bool colorSupported = false;
    bool inAltScreen = false;

    enum StepIndex {
        STEP_QUERY = 0,
        STEP_DOWNLOAD,
        STEP_VERIFY,
        STEP_AUDIT,
        STEP_EXTRACT,
        STEP_APPLY,
        STEP_COUNT
    };

    std::array<StepInfo, STEP_COUNT> steps;
    std::string targetDir;
    std::string arch;
    std::string quality;
    bool headerRendered = false;

    ~ModernUI() {
        restoreScreen();
    }

    void init(bool interactive, bool forcePlain, const std::string& dir,
              const std::string& a, const std::string& q) {
        targetDir = dir;
        arch = a;
        quality = q;
        enabled = interactive && !forcePlain;

        steps[STEP_QUERY]    = {"查询官方更新信息", "", StepStatus::Pending};
        steps[STEP_DOWNLOAD] = {"分段下载与断点续传", "", StepStatus::Pending};
        steps[STEP_VERIFY]   = {"核验官方 SHA-256", "", StepStatus::Pending};
        steps[STEP_AUDIT]    = {"解包目录安全审计", "", StepStatus::Pending};
        steps[STEP_EXTRACT]  = {"解压释放与断点就绪", "", StepStatus::Pending};
        steps[STEP_APPLY]    = {"便携版数据保留与原子切换", "", StepStatus::Pending};

        if (enabled) {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                if (SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                    colorSupported = true;
                    // Switch to alternate screen buffer, clear screen, and hide cursor
                    std::cout << "\033[?1049h\033[2J\033[H\033[?25l" << std::flush;
                    inAltScreen = true;

                    // Register console control handler for graceful Ctrl+C interruption
                    SetConsoleCtrlHandler([](DWORD signal) -> BOOL {
                        if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
                            g_interruptRequested.store(true);
                            return TRUE; // Handled! Give worker threads a chance to pause and show prompt
                        }
                        if (signal == CTRL_CLOSE_EVENT) {
                            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                            DWORD w = 0;
                            const char seq[] = "\033[?25h\033[?1049l\n";
                            WriteConsoleA(h, seq, sizeof(seq) - 1, &w, nullptr);
                        }
                        return FALSE;
                    }, TRUE);
                }
            }
        }
    }

    void restoreScreen() {
        if (inAltScreen) {
            // Restore primary screen buffer and show cursor
            std::cout << "\033[?25h\033[?1049l" << std::flush;
            inAltScreen = false;
        }
    }

    void renderHeader() {
        if (!enabled) {
            std::cout << "VS Code 更新器 | " << arch << " | " << quality << "\n目标: " << targetDir << "\n";
            return;
        }
        clearScreen();
        std::cout << (colorSupported ? "\033[1;36m" : "")
                  << "┌─────────────────────────────────────────────────────────────┐\n"
                  << "│  VS Code Portable Updater                                   │\n"
                  << "└─────────────────────────────────────────────────────────────┘"
                  << (colorSupported ? "\033[0m\n" : "\n");
        std::cout << (colorSupported ? "\033[90m" : "")
                  << "  目标架构: " << arch << "  •  更新渠道: " << quality
                  << "  •  目标: " << targetDir
                  << (colorSupported ? "\033[0m\n\n" : "\n\n");
        headerRendered = true;
        renderDashboard();
    }

    void setStep(StepIndex idx, StepStatus status, const std::string& detail = "") {
        steps[idx].status = status;
        if (!detail.empty()) steps[idx].detail = detail;
        if (!enabled) {
            std::string prefix;
            switch (status) {
                case StepStatus::Active:  prefix = "[..] "; break;
                case StepStatus::Success: prefix = "[OK] "; break;
                case StepStatus::Failed:  prefix = "[ERR] "; break;
                case StepStatus::Skipped: prefix = "[--] "; break;
                default:                  prefix = "     "; break;
            }
            std::cout << prefix << steps[idx].name;
            if (!detail.empty()) std::cout << " (" << detail << ")";
            std::cout << '\n';
            return;
        }
        renderDashboard();
    }

    void renderDashboard(double progressRatio = -1.0,
                         double currentMiB = 0.0, double totalMiB = 0.0,
                         double speedMiB = 0.0, std::uint64_t etaSeconds = 0,
                         std::size_t extractedFiles = 0, std::size_t totalFiles = 0,
                         const std::string& currentFile = "") {
        if (!enabled) return;

        if (colorSupported) {
            std::cout << "\033[5;1H";
            std::cout << "\033[1m更新流水线:\033[0m\n";
        } else {
            std::cout << "\r更新流水线:\n";
        }

        for (int i = 0; i < STEP_COUNT; ++i) {
            const auto& st = steps[i];
            std::string icon;
            std::string color;
            switch (st.status) {
                case StepStatus::Success:
                    icon = "[√]";
                    color = colorSupported ? "\033[32;1m" : "";
                    break;
                case StepStatus::Active:
                    icon = "[>]";
                    color = colorSupported ? "\033[33;1m" : "";
                    break;
                case StepStatus::Failed:
                    icon = "[x]";
                    color = colorSupported ? "\033[31;1m" : "";
                    break;
                case StepStatus::Skipped:
                    icon = "[-]";
                    color = colorSupported ? "\033[90m" : "";
                    break;
                default:
                    icon = "[ ]";
                    color = colorSupported ? "\033[90m" : "";
                    break;
            }

            std::string prefix = "  " + icon + " " + std::to_string(i + 1) + ". " + st.name;
            int prefixWidth = strDisplayWidth(prefix);
            int pad = 36 - prefixWidth;
            if (pad < 2) pad = 2;

            std::cout << "  " << color << icon << " " << (i + 1) << ". " << st.name;
            if (colorSupported) std::cout << "\033[0m";
            std::cout << std::string(pad, ' ');

            if (!st.detail.empty()) {
                std::cout << (colorSupported ? "\033[90m" : "") << st.detail << (colorSupported ? "\033[0m" : "");
            }
            std::cout << (colorSupported ? "\033[K\n" : "                                  \n");

            if (st.status == StepStatus::Active && progressRatio >= 0.0) {
                std::cout << "         " << renderProgressBar(progressRatio, 24) << "  ";
                std::cout << std::fixed << std::setprecision(1) << (progressRatio * 100.0) << "%";
                if (speedMiB > 0.01) {
                    std::cout << "  •  " << speedMiB << " MiB/s";
                }
                if (totalMiB > 0.1) {
                    std::cout << "  (" << currentMiB << "/" << totalMiB << " MiB)";
                }
                if (etaSeconds > 0) {
                    std::cout << "  •  剩余 " << etaSeconds << "s";
                }
                std::cout << (colorSupported ? "\033[K\n" : "       \n");

                if (!currentFile.empty() || totalFiles > 0) {
                    std::string displayFile = currentFile;
                    if (displayFile.size() > 40) displayFile = "..." + displayFile.substr(displayFile.size() - 37);
                    std::cout << (colorSupported ? "\033[90m" : "")
                              << "         正在提取: " << displayFile;
                    if (totalFiles > 0) {
                        std::cout << "  [" << extractedFiles << "/" << totalFiles << "]";
                    }
                    std::cout << (colorSupported ? "\033[0m\033[K\n" : "                             \n");
                }
            }
        }
        std::cout << std::flush;
    }

    bool promptConfirmContinue(const std::string& currentVer, const std::string& commit) {
        if (!enabled) {
            std::cout << "\n当前安装已是目标渠道最新版本 (" << currentVer << ")。\n是否仍要重新安装？[y/N]: " << std::flush;
            std::string line;
            if (std::getline(std::cin, line)) {
                return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
            }
            return false;
        }

        const int innerW = 58;
        std::string shortCommit = commit.size() > 16 ? commit.substr(0, 16) + "..." : commit;

        std::cout << "\033[13;1H";
        std::cout << (colorSupported ? "\033[1;36m" : "")
                  << "  ┌──────────────────────────────────────────────────────────┐\n"
                  << (colorSupported ? "\033[0m" : "");
        std::cout << padBoxLine("                      更新确认提示", innerW);
        std::cout << (colorSupported ? "\033[1;36m" : "")
                  << "  ├──────────────────────────────────────────────────────────┤\n"
                  << (colorSupported ? "\033[0m" : "");
        std::cout << padBoxLine("  当前本地安装已是目标渠道最新构建: " + currentVer, innerW);
        std::cout << padBoxLine("  构建提交: " + shortCommit, innerW);
        std::cout << padBoxLine("", innerW);
        std::cout << padBoxLine("  是否继续强制重新安装？", innerW);
        std::cout << padBoxLine("", innerW);
        std::cout << padBoxLine("           [Y] 确认重新安装     [N] 取消退出", innerW);
        std::cout << (colorSupported ? "\033[1;36m" : "")
                  << "  └──────────────────────────────────────────────────────────┘\n"
                  << (colorSupported ? "\033[0m" : "") << std::flush;

        while (true) {
            int ch = _getch();
            if (ch == 'y' || ch == 'Y') {
                std::cout << "\033[13;1H\033[J" << std::flush;
                return true;
            }
            if (ch == 'n' || ch == 'N' || ch == 27 || ch == '\r' || ch == '\n') {
                return false;
            }
        }
    }

    bool promptCancelOrExit(const std::string& actionName) {
        if (!enabled) {
            std::cout << "\n检测到中断信号 (Ctrl+C)。是否退出当前" << actionName << "？[Y/n]: " << std::flush;
            std::string line;
            if (std::getline(std::cin, line)) {
                return line.empty() || line[0] == 'y' || line[0] == 'Y';
            }
            return true;
        }

        const int innerW = 58;
        std::cout << "\033[13;1H";
        std::cout << (colorSupported ? "\033[1;33m" : "")
                  << "  ┌──────────────────────────────────────────────────────────┐\n"
                  << (colorSupported ? "\033[0m" : "");
        std::cout << padBoxLine("                      中断操作提示", innerW);
        std::cout << (colorSupported ? "\033[1;33m" : "")
                  << "  ├──────────────────────────────────────────────────────────┤\n"
                  << (colorSupported ? "\033[0m" : "");
        std::cout << padBoxLine("  检测到用户键盘中断信号 (Ctrl+C)", innerW);
        std::cout << padBoxLine("  当前正在执行: " + actionName, innerW);
        std::cout << padBoxLine("  已完成的进度与数据均已安全暂存，支持断点续传。", innerW);
        std::cout << padBoxLine("", innerW);
        std::cout << padBoxLine("  请选择下一步操作：", innerW);
        std::cout << padBoxLine("", innerW);
        std::cout << padBoxLine("      [Y] 强制退出 / 取消下载      [C] 继续当前任务", innerW);
        std::cout << (colorSupported ? "\033[1;33m" : "")
                  << "  └──────────────────────────────────────────────────────────┘\n"
                  << (colorSupported ? "\033[0m" : "") << std::flush;

        while (true) {
            int ch = _getch();
            if (ch == 'y' || ch == 'Y' || ch == 27 || ch == '\r' || ch == '\n') {
                return true; // Exit
            }
            if (ch == 'c' || ch == 'C' || ch == 'n' || ch == 'N') {
                // Clear modal and resume
                std::cout << "\033[13;1H\033[J" << std::flush;
                g_interruptRequested.store(false);
                return false; // Continue
            }
        }
    }

    void finish(bool success, const std::string& msg = "") {
        if (!enabled) {
            if (!msg.empty()) std::cout << (success ? "[成功] " : "[失败] ") << msg << '\n';
            return;
        }
        std::cout << "\033[13;1H\033[J\n" << (colorSupported ? (success ? "\033[32;1m" : "\033[31;1m") : "");
        std::cout << (success ? "[完成] " : "[错误] ") << msg;
        std::cout << (colorSupported ? "\033[0m\n\n" : "\n\n");
    }

private:
    void clearScreen() {
        if (colorSupported) {
            std::cout << "\033[2J\033[H";
        }
    }

    std::string renderProgressBar(double ratio, int width = 24) {
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        int filled = static_cast<int>(ratio * width);
        std::string bar;
        bar.reserve(width * 4);
        if (colorSupported) bar += "\033[36;1m";
        for (int i = 0; i < filled; ++i) bar += "━";
        if (colorSupported) bar += "\033[90m";
        for (int i = filled; i < width; ++i) bar += "─";
        if (colorSupported) bar += "\033[0m";
        return bar;
    }
};

ModernUI ui;



std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    require(n > 0, "Invalid Unicode input");
    std::string result(n, '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr) == n,
            "Cannot convert Unicode input");
    return result;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    require(n > 0, "Invalid UTF-8 input");
    std::wstring result(n, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n) == n,
            "Cannot convert UTF-8 input");
    return result;
}

std::string pathText(const fs::path& p) { return utf8(p.native()); }
std::string winError(const std::string& action) { return action + " (Windows error " + std::to_string(GetLastError()) + ")"; }

struct WinHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit WinHandle(HANDLE h) : value(h) {}
    ~WinHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
};

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET h) : value(h) { require(h != nullptr, winError("Network handle creation failed")); }
    ~InternetHandle() { InternetCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

struct Console {
    UINT oldOutput = GetConsoleOutputCP(), oldInput = GetConsoleCP();
    Console() {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        DWORD mode = 0;
        interactiveOutput = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    }
    ~Console() {
        if (oldOutput) SetConsoleOutputCP(oldOutput);
        if (oldInput) SetConsoleCP(oldInput);
    }
};

struct OrdinalLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

bool pathWithin(const fs::path& child, const fs::path& parent) {
    const auto c = child.lexically_normal(), p = parent.lexically_normal();
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end()) return false;
        if (CompareStringOrdinal(ci->c_str(), -1, pi->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    }
    return true;
}

void rejectReparse(const fs::path& p) {
    const auto attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, winError("Cannot inspect path"));
    } else require((attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "Links/junctions are not allowed here: " + pathText(p));
}

void safeAncestors(const fs::path& p) {
    for (fs::path q = p; !q.empty();) {
        rejectReparse(q);
        const auto parent = q.parent_path();
        if (q == parent) break;
        q = parent;
    }
}

fs::path executablePath() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    require(n && n < buf.size(), winError("Cannot locate updater executable"));
    buf.resize(n);
    return fs::path(buf);
}

json readJson(const fs::path& p) {
    require(fs::file_size(p) <= 4 * 1024 * 1024, "JSON file exceeds size limit: " + pathText(p));
    std::ifstream f(p, std::ios::binary);
    require(f.good(), "Cannot open JSON file: " + pathText(p));
    return json::parse(f);
}

void writeJson(const fs::path& p, const json& j) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    require(f.good(), "Cannot create metadata file");
    f << j.dump(2) << '\n';
    f.flush();
    require(f.good(), "Cannot write metadata file");
    f.close();
    require(!f.fail(), "Cannot close metadata file");
}

fs::path locateProductJson(const fs::path& dir) {
    const auto standard = dir / "resources/app/product.json";
    if (fs::is_regular_file(standard)) return standard;
    if (fs::is_directory(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                const auto candidate = entry.path() / "resources/app/product.json";
                if (fs::is_regular_file(candidate)) return candidate;
            }
        }
    }
    return {};
}

bool looksLikeVSCode(const fs::path& p) {
    const auto product = locateProductJson(p);
    if (product.empty()) return false;
    const auto appDir = product.parent_path();
    return fs::is_regular_file(appDir / "package.json") &&
           (fs::is_regular_file(p / "Code.exe") || fs::is_regular_file(p / "Code - Insiders.exe"));
}

fs::path validateTarget(const std::string& input) {
    const fs::path raw(wide(input));
    require(!raw.has_root_name() || raw.has_root_directory(), "Drive-relative paths such as C:folder are not allowed");
    auto p = fs::absolute(raw).lexically_normal();
    // Remove trailing separators, except the volume root.
    while (p.filename().empty() && p != p.root_path()) p = p.parent_path();
    require(p.root_name().native().rfind(L"\\\\", 0) != 0, "Use a local drive, not a UNC/device path");
    require(p != p.root_path() && !p.filename().empty(), "A volume root cannot be an installation target");
    // Validate the target basename against Windows aliases and reserved names too.
    updater::safeArchivePath(utf8(p.filename().native()));
    require(!pathWithin(fs::current_path(), p), "Target cannot contain the current working directory");
    require(!pathWithin(executablePath(), p), "Run the updater from outside the installation directory");
    std::array<wchar_t, 32768> env{};
    for (const wchar_t* name : {L"USERPROFILE", L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
        const DWORD n = GetEnvironmentVariableW(name, env.data(), static_cast<DWORD>(env.size()));
        if (n && n < env.size()) {
            const fs::path protectedPath(env.data());
            require(!pathWithin(protectedPath, p), "Target is a protected system/user directory");
            if (std::wcscmp(name, L"SystemRoot") == 0)
                require(!pathWithin(p, protectedPath), "Installing inside Windows is not allowed");
        }
    }
    safeAncestors(p);
    if (fs::exists(p)) {
        require(fs::is_directory(p), "Target is not a directory");
        require(fs::is_empty(p) || looksLikeVSCode(p), "Refusing to replace an unrelated non-empty directory");
    }
    return p;
}

void ensureNotRunning(const fs::path& target) {
    WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    require(snapshot.value != INVALID_HANDLE_VALUE, winError("Cannot enumerate running applications"));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    require(Process32FirstW(snapshot.value, &entry) != 0, winError("Cannot enumerate processes"));
    do {
        const std::wstring name(entry.szExeFile);
        if (CompareStringOrdinal(name.c_str(), -1, L"Code.exe", -1, TRUE) != CSTR_EQUAL &&
            CompareStringOrdinal(name.c_str(), -1, L"Code - Insiders.exe", -1, TRUE) != CSTR_EQUAL) continue;
        WinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        require(process.value != nullptr, "Cannot inspect a running VS Code process; close VS Code before updating");
        std::wstring buf(32768, L'\0');
        DWORD count = static_cast<DWORD>(buf.size());
        require(QueryFullProcessImageNameW(process.value, 0, buf.data(), &count) != 0,
                "Cannot inspect a running VS Code process; close VS Code before updating");
        buf.resize(count);
        require(!pathWithin(fs::path(buf), target), "Close this installation of VS Code before updating");
    } while (Process32NextW(snapshot.value, &entry));
}

std::string executableArch(const fs::path& exe) {
    std::ifstream f(exe, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    require(static_cast<bool>(f.read(reinterpret_cast<char*>(&dos), sizeof(dos))) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            dos.e_lfanew > 0 && dos.e_lfanew < 16 * 1024 * 1024, "Invalid VS Code executable");
    f.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    require(static_cast<bool>(f.read(reinterpret_cast<char*>(&signature), sizeof(signature))) && signature == IMAGE_NT_SIGNATURE &&
            static_cast<bool>(f.read(reinterpret_cast<char*>(&header), sizeof(header))), "Invalid PE header");
    if (header.Machine == IMAGE_FILE_MACHINE_AMD64) return "x64";
    if (header.Machine == IMAGE_FILE_MACHINE_ARM64) return "arm64";
    throw std::runtime_error("Unsupported VS Code executable architecture");
}

updater::Identity localIdentity(const fs::path& dir) {
    try {
        const auto productPath = locateProductJson(dir);
        if (productPath.empty()) return {};
        const auto appDir = productPath.parent_path();
        const auto product = readJson(productPath);
        const auto package = readJson(appDir / "package.json");
        updater::Identity id;
        id.version = package.value("version", product.value("version", ""));
        id.commit = product.value("commit", "");
        id.quality = product.value("quality", "");
        if (id.quality != "stable" && id.quality != "insider") return {};
        id.arch = executableArch(dir / (id.quality == "insider" ? "Code - Insiders.exe" : "Code.exe"));
        return id;
    } catch (...) { return {}; }
}

namespace http {
void timeouts(HINTERNET h) {
    DWORD timeout = 30000;
    for (DWORD option : {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_RECEIVE_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT})
        require(InternetSetOptionW(h, option, &timeout, sizeof(timeout)) != 0, winError("Cannot set network timeout"));
}

void validateHttps(const std::wstring& url) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = 1;
    require(InternetCrackUrlW(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) != 0 &&
            parts.nScheme == INTERNET_SCHEME_HTTPS && parts.dwHostNameLength > 0 &&
            parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0, "Only HTTPS URLs without embedded credentials are allowed");
}

struct Request {
    const std::chrono::steady_clock::time_point deadline;
    InternetHandle session;
    std::unique_ptr<InternetHandle> response;
    explicit Request(const std::string& url, const std::wstring& headers = L"", std::chrono::seconds lifetime = 900s)
        : deadline(std::chrono::steady_clock::now() + lifetime),
          session(InternetOpenW(L"VSCodeUpdater/2.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0)) {
        const auto address = wide(url);
        validateHttps(address);
        timeouts(session.value);
        const std::wstring allHeaders = L"Accept-Encoding: identity\r\n" + headers;
        response = std::make_unique<InternetHandle>(InternetOpenUrlW(session.value, address.c_str(), allHeaders.c_str(),
            static_cast<DWORD>(allHeaders.size()), INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI, 0));
        timeouts(response->value);
        DWORD bytes = 0;
        InternetQueryOptionW(response->value, INTERNET_OPTION_URL, nullptr, &bytes);
        require(bytes > 0 && bytes <= 64 * 1024, "Cannot inspect redirect URL");
        std::vector<wchar_t> finalUrl(bytes / sizeof(wchar_t) + 1, L'\0');
        require(InternetQueryOptionW(response->value, INTERNET_OPTION_URL, finalUrl.data(), &bytes) != 0, "Cannot inspect redirect URL");
        validateHttps(finalUrl.data());
        const auto encoding = header(HTTP_QUERY_CONTENT_ENCODING);
        require(encoding.empty() || updater::asciiLower(encoding) == "identity", "Unexpected HTTP content encoding");
    }
    unsigned status() const {
        DWORD result = 0, size = sizeof(result);
        require(HttpQueryInfoW(response->value, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &result, &size, nullptr) != 0,
                winError("Missing HTTP status"));
        return result;
    }
    std::string header(DWORD key) const {
        DWORD size = 0;
        HttpQueryInfoW(response->value, key, nullptr, &size, nullptr);
        if (GetLastError() == ERROR_HTTP_HEADER_NOT_FOUND) return {};
        require(size > 0 && size <= 64 * 1024, "Invalid HTTP header size");
        std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
        require(HttpQueryInfoW(response->value, key, value.data(), &size, nullptr) != 0, winError("Cannot read HTTP header"));
        return utf8(value.data());
    }
    std::size_t read(char* data, std::size_t capacity) {
        require(std::chrono::steady_clock::now() < deadline, "HTTP request exceeded its overall deadline");
        DWORD n = 0;
        require(InternetReadFile(response->value, data, static_cast<DWORD>(capacity), &n) != 0,
                winError("Network read failed"));
        require(std::chrono::steady_clock::now() < deadline, "HTTP request exceeded its overall deadline");
        return n;
    }
};

std::string getJson(const std::string& url) {
    Request request(url, L"", 120s);
    require(request.status() == 200, "Version query returned HTTP " + std::to_string(request.status()));
    std::string text;
    updater::copyResponse([&](char* p, std::size_t n) { return request.read(p, n); },
                          [&](const char* p, std::size_t n) { text.append(p, n); }, 1024 * 1024);
    return text;
}
} // namespace http

struct Remote {
    updater::Identity id;
    std::string url, sha256;
};

Remote fetchRemote(const updater::Options& options) {
    const auto api = "https://update.code.visualstudio.com/api/update/win32-" + options.arch + "-archive/" + options.quality + "/latest";
    const auto j = json::parse(http::getJson(api));
    Remote r;
    r.id = {j.at("productVersion").get<std::string>(), j.at("version").get<std::string>(), options.arch, options.quality};
    r.url = j.at("url").get<std::string>();
    r.sha256 = updater::asciiLower(j.at("sha256hash").get<std::string>());
    require(!r.id.version.empty() && updater::hexString(r.id.commit, 40) && updater::hexString(r.sha256, 64), "Invalid official update metadata");
    http::validateHttps(wide(r.url));
    return r;
}

class Sha256Hasher {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
    bool finalized_ = false;

public:
    Sha256Hasher() {
        require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                "Cannot initialize SHA-256");
        DWORD size = 0, returned = 0;
        require(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) >= 0,
                "Cannot query SHA-256 state size");
        object_.resize(size);
        require(BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) >= 0,
                "Cannot create SHA-256 state");
    }

    ~Sha256Hasher() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void update(const void* data, std::size_t len) {
        require(!finalized_, "Hash already finalized");
        if (len == 0) return;
        require(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(len), 0) >= 0,
                "SHA-256 update failed");
    }

    std::string finish() {
        require(!finalized_, "Hash already finalized");
        finalized_ = true;
        std::array<UCHAR, 32> digest{};
        require(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
                "SHA-256 finalization failed");
        std::ostringstream text;
        for (auto b : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
        return text.str();
    }
};

std::string sha256(const fs::path& file) {
    Sha256Hasher hasher;
    std::ifstream f(file, std::ios::binary);
    require(f.good(), "Cannot read downloaded archive");
    std::array<char, 128 * 1024> data{};
    while (f.read(data.data(), data.size()) || f.gcount())
        hasher.update(data.data(), static_cast<std::size_t>(f.gcount()));
    require(f.eof() && !f.bad(), "Archive read failed during SHA-256 verification");
    return hasher.finish();
}

struct Progress {
    std::atomic<std::uint64_t> count{0};
};

// Every worker catches exceptions. Even partial thread creation is joined safely.
template<class Work>
void parallelWork(int count, std::vector<Progress>& progress, std::uint64_t total, const char* title, bool byteProgress,
                  const std::atomic<const char*>* currentItem, Work work) {
    const auto started = std::chrono::steady_clock::now();
    std::atomic<bool> cancel{false};
    std::atomic<int> done{0};
    std::vector<std::string> errors(count);
    std::vector<std::thread> workers;
    workers.reserve(count);
    try {
        for (int i = 0; i < count; ++i) workers.emplace_back([&, i] {
            try { work(i, cancel); }
            catch (const std::exception& e) { errors[i] = e.what(); cancel = true; }
            catch (...) { errors[i] = "Unexpected worker failure"; cancel = true; }
            ++done;
        });
    } catch (...) {
        cancel = true;
        for (auto& t : workers) t.join();
        throw;
    }
    while (done.load() != count) {
        if (g_interruptRequested.load()) {
            bool shouldExit = ui.promptCancelOrExit(title);
            if (shouldExit) {
                cancel = true;
                for (auto& t : workers) t.join();
                ui.finish(false, "用户已强制退出 / 取消");
                exit(130);
            }
        }
        if (interactiveOutput) {
            std::uint64_t current = 0;
            for (auto& p : progress) current += p.count.load();
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double rate = elapsed > 0.01 ? current / elapsed : 0;
            const double ratio = total ? static_cast<double>(current) / static_cast<double>(total) : 0.0;
            const std::uint64_t eta = (rate > 0 && current < total) ? static_cast<std::uint64_t>((total - current) / rate) : 0;
            if (ui.enabled) {
                if (byteProgress) {
                    ui.renderDashboard(ratio, current / 1048576.0, total / 1048576.0, rate / 1048576.0, eta);
                } else {
                    const char* curFile = currentItem ? currentItem->load() : "";
                    ui.renderDashboard(ratio, 0.0, 0.0, 0.0, eta, current, total, curFile ? curFile : "");
                }
            } else {
                std::ostringstream line;
                line << '\r' << title << ": " << (total ? std::min<std::uint64_t>(100, current * 100 / total) : 0)
                     << "%  " << std::fixed << std::setprecision(1);
                if (byteProgress) line << current / 1048576.0 << "/" << total / 1048576.0 << " MiB  " << rate / 1048576.0 << " MiB/s";
                else line << current << "/" << total << " files  " << rate << " files/s";
                if (rate > 0 && current < total) line << "  ETA " << eta << "s";
                std::cout << line.str() << "                    " << std::flush;
            }
        }
        std::this_thread::sleep_for(100ms);
    }
    for (auto& t : workers) t.join();
    if (interactiveOutput && !ui.enabled) std::cout << '\r' << title << ": finished                                                            \n";
    for (const auto& error : errors) if (!error.empty()) throw std::runtime_error(error);
}

void finishFile(std::ofstream& f) {
    f.flush();
    require(f.good(), "File write/flush failed (disk full or permission denied)");
    f.close();
    require(!f.fail(), "File close failed");
}

void streamDownload(http::Request& request, const fs::path& path) {
    require(request.status() == 200, "Download returned HTTP " + std::to_string(request.status()));
    const auto length = request.header(HTTP_QUERY_CONTENT_LENGTH);
    const auto expected = length.empty() ? UINT64_MAX : updater::number(length);
    require(expected == UINT64_MAX || (expected > 0 && expected <= updater::maxArchiveBytes), "Invalid download size");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    require(f.good(), "Cannot create download file");
    updater::copyResponse([&](char* p, std::size_t n) { return request.read(p, n); },
        [&](const char* p, std::size_t n) { f.write(p, n); require(f.good(), "Download write failed"); },
        expected == UINT64_MAX ? updater::maxArchiveBytes : expected, expected);
    finishFile(f);
}

void downloadOnce(const Remote& remote, const fs::path& path, int requestedThreads) {
    std::uint64_t size = 0;
    std::string etag;
    {
        http::Request probe(remote.url, L"Range: bytes=0-0\r\n");
        if (probe.status() == 200) {
            std::cout << "服务器未采用分段请求，使用单线程下载。\n";
            streamDownload(probe, path);
            return;
        }
        require(probe.status() == 206, "Download probe returned HTTP " + std::to_string(probe.status()));
        const auto range = updater::parseContentRange(probe.header(HTTP_QUERY_CONTENT_RANGE));
        updater::validateRange(206, probe.header(HTTP_QUERY_CONTENT_RANGE), 0, 1, range.total);
        size = range.total;
        require(size <= updater::maxArchiveBytes, "Archive exceeds the 2 GiB download limit");
        etag = probe.header(HTTP_QUERY_ETAG);
        if (etag.rfind("W/", 0) == 0) etag.clear();
        updater::copyResponse([&](char* p, std::size_t n) { return probe.read(p, n); }, [](const char*, std::size_t) {}, 1, 1);
    }
    require(fs::space(path.parent_path()).available >= size + 64ULL * 1024 * 1024, "Not enough space for the download");

    const std::uint64_t chunkSize = 2ULL * 1024 * 1024; // 2 MiB chunks for granular resumability
    const std::size_t totalChunks = static_cast<std::size_t>((size + chunkSize - 1) / chunkSize);
    const auto metaPath = path.string() + ".part.json";

    std::vector<bool> chunkCompleted(totalChunks, false);
    std::uint64_t existingBytes = 0;

    // Check if partial download file and meta match
    if (fs::exists(path) && fs::exists(metaPath) && fs::file_size(path) == size) {
        try {
            const auto meta = readJson(metaPath);
            if (meta.value("url", "") == remote.url &&
                meta.value("etag", "") == etag &&
                meta.value("size", 0ULL) == size &&
                meta.value("chunkSize", 0ULL) == chunkSize &&
                meta.value("sha256", "") == remote.sha256) {
                const auto compList = meta.value("completed", std::vector<std::size_t>{});
                for (auto idx : compList) {
                    if (idx < totalChunks && !chunkCompleted[idx]) {
                        chunkCompleted[idx] = true;
                        const auto len = std::min(chunkSize, size - idx * chunkSize);
                        existingBytes += len;
                    }
                }
            }
        } catch (...) {
            chunkCompleted.assign(totalChunks, false);
            existingBytes = 0;
        }
    }

    if (existingBytes == 0) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        require(f.good(), "Cannot create download file");
        f.close();
        fs::resize_file(path, size);
    }

    struct Job {
        std::size_t index;
        std::uint64_t offset;
        std::uint64_t length;
    };

    std::vector<Job> jobs;
    for (std::size_t i = 0; i < totalChunks; ++i) {
        if (!chunkCompleted[i]) {
            const auto off = i * chunkSize;
            const auto len = std::min(chunkSize, size - off);
            jobs.push_back({i, off, len});
        }
    }

    if (jobs.empty()) {
        std::error_code ec;
        fs::remove(metaPath, ec);
        return;
    }

    const int count = static_cast<int>(std::min<std::uint64_t>(requestedThreads, std::max<std::uint64_t>(1, jobs.size())));
    std::vector<Progress> progress(count);
    progress[0].count = existingBytes;

    std::atomic<std::size_t> nextJobIndex{0};
    std::mutex metaMutex;

    auto saveMeta = [&]() {
        std::lock_guard<std::mutex> lock(metaMutex);
        std::vector<std::size_t> doneIndices;
        doneIndices.reserve(totalChunks);
        for (std::size_t i = 0; i < totalChunks; ++i) {
            if (chunkCompleted[i]) doneIndices.push_back(i);
        }
        json meta = {
            {"url", remote.url},
            {"etag", etag},
            {"size", size},
            {"chunkSize", chunkSize},
            {"sha256", remote.sha256},
            {"completed", doneIndices}
        };
        try {
            writeJson(metaPath, meta);
        } catch (...) {}
    };

    parallelWork(count, progress, size, "分段断点下载", true, nullptr, [&](int id, const std::atomic<bool>& cancel) {
        while (!cancel) {
            const auto jobIdx = nextJobIndex.fetch_add(1);
            if (jobIdx >= jobs.size()) return;
            const auto& job = jobs[jobIdx];

            for (int attempt = 0; attempt < 3; ++attempt) {
                try {
                    require(!cancel, "Download cancelled because another worker failed");
                    auto headers = L"Range: bytes=" + std::to_wstring(job.offset) + L"-" + std::to_wstring(job.offset + job.length - 1) + L"\r\n";
                    if (!etag.empty()) headers += L"If-Match: " + wide(etag) + L"\r\n";
                    http::Request request(remote.url, headers);
                    updater::validateRange(request.status(), request.header(HTTP_QUERY_CONTENT_RANGE), job.offset, job.length, size);
                    const auto contentLength = request.header(HTTP_QUERY_CONTENT_LENGTH);
                    require(contentLength.empty() || updater::number(contentLength) == job.length, "Segment Content-Length mismatch");

                    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
                    require(f.good(), "Cannot open segment output file");
                    f.seekp(static_cast<std::streamoff>(job.offset));
                    require(f.good(), "Cannot seek segment output file");

                    updater::copyResponse([&](char* p, std::size_t n) {
                        require(!cancel, "Download cancelled");
                        return request.read(p, n);
                    }, [&](const char* p, std::size_t n) {
                        f.write(p, n);
                        require(f.good(), "Segment write failed");
                        progress[id].count += n;
                    }, job.length, job.length);

                    f.flush();
                    require(f.good(), "Segment flush failed");
                    f.close();
                    require(!f.fail(), "Segment close failed");

                    {
                        std::lock_guard<std::mutex> lock(metaMutex);
                        chunkCompleted[job.index] = true;
                    }
                    if (jobIdx % 5 == 0 || jobIdx + 1 == jobs.size()) {
                        saveMeta();
                    }
                    break;
                } catch (...) {
                    if (attempt == 2 || cancel.load()) throw;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300 * (1 << attempt)));
                }
            }
        }
    });

    saveMeta();
    require(fs::file_size(path) == size, "Final archive size mismatch");
    std::error_code ec;
    fs::remove(metaPath, ec);
}

void download(const Remote& remote, const fs::path& file, int threads) {
    // If complete, valid archive already exists in persistent cache, skip re-downloading entirely!
    std::error_code ec;
    if (fs::exists(file, ec) && fs::file_size(file, ec) > 0) {
        try {
            if (sha256(file) == remote.sha256) {
                return;
            }
        } catch (...) {}
    }

    // Segment retries preserve completed ranges. An overall failure gets one fresh retry.
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            downloadOnce(remote, file, threads);
            return;
        } catch (const std::exception& e) {
            if (attempt == 1) throw;
            std::cerr << "下载异常，将尝试续传: " << e.what() << '\n';
            std::this_thread::sleep_for(1s);
        }
    }
}


class ZipStreamUnpacker {
public:
    enum State {
        STATE_HEADER,
        STATE_DATA,
        STATE_DONE
    };

    explicit ZipStreamUnpacker(const updater::ZipStreamPlan& plan) : plan_(plan) {
        initCurrentEntry();
    }

    ~ZipStreamUnpacker() {
        if (inflaterActive_) {
            mz_inflateEnd(&inflater_);
        }
    }

    // Callbacks:
    // onDir: void(const std::string& safePath)
    // onFileStart: void(const updater::PlannedEntry& entry)
    // onFileData: void(const uint8_t* chunk, std::size_t len)
    // onFileEnd: void(const updater::PlannedEntry& entry)
    template<class OnDir, class OnFileStart, class OnFileData, class OnFileEnd>
    void feed(std::uint64_t streamOffset, const std::uint8_t* data, std::size_t size,
              OnDir onDir, OnFileStart onFileStart, OnFileData onFileData, OnFileEnd onFileEnd) {
        std::size_t processed = 0;
        while (processed < size && currentEntryIdx_ < plan_.entries.size()) {
            const std::uint64_t byteOffset = streamOffset + processed;
            const auto& entry = plan_.entries[currentEntryIdx_];

            if (state_ == STATE_HEADER) {
                if (byteOffset < entry.localOffset) {
                    const std::size_t skip = std::min<std::size_t>(size - processed, static_cast<std::size_t>(entry.localOffset - byteOffset));
                    processed += skip;
                    continue;
                }

                const std::size_t needHeader = localHeaderBuf_.size();
                if (needHeader < 30) {
                    const std::size_t toCopy = std::min<std::size_t>(size - processed, 30 - needHeader);
                    localHeaderBuf_.insert(localHeaderBuf_.end(), data + processed, data + processed + toCopy);
                    processed += toCopy;
                    if (localHeaderBuf_.size() < 30) continue;

                    const std::uint32_t sig = updater::readU32(localHeaderBuf_.data());
                    require(sig == 0x04034b50, "Invalid local header signature");
                    const std::uint16_t flen = updater::readU16(localHeaderBuf_.data() + 26);
                    const std::uint16_t elen = updater::readU16(localHeaderBuf_.data() + 28);
                    localHeaderTargetLen_ = 30 + flen + elen;
                }

                if (localHeaderBuf_.size() < localHeaderTargetLen_) {
                    const std::size_t toCopy = std::min<std::size_t>(size - processed, localHeaderTargetLen_ - localHeaderBuf_.size());
                    localHeaderBuf_.insert(localHeaderBuf_.end(), data + processed, data + processed + toCopy);
                    processed += toCopy;
                    if (localHeaderBuf_.size() < localHeaderTargetLen_) continue;
                }

                // Header is fully received
                state_ = STATE_DATA;
                dataBytesLeft_ = entry.compSize;

                if (entry.isDirectory) {
                    onDir(entry.safePath);
                } else {
                    onFileStart(entry);
                }

                if (entry.compSize == 0) {
                    completeEntry(onFileEnd);
                }
            } else if (state_ == STATE_DATA) {
                const std::size_t toConsume = std::min<std::size_t>(size - processed, dataBytesLeft_);
                if (toConsume > 0) {
                    consumeFileData(data + processed, toConsume, onFileData);
                    processed += toConsume;
                    dataBytesLeft_ -= toConsume;
                }
                if (dataBytesLeft_ == 0) {
                    flushRemaining(onFileData);
                    completeEntry(onFileEnd);
                }
            }
        }
    }

    bool isComplete() const {
        return currentEntryIdx_ >= plan_.entries.size();
    }

    std::size_t completedEntriesCount() const {
        return currentEntryIdx_;
    }

private:
    void initCurrentEntry() {
        if (currentEntryIdx_ >= plan_.entries.size()) {
            state_ = STATE_DONE;
            return;
        }
        state_ = STATE_HEADER;
        localHeaderBuf_.clear();
        localHeaderTargetLen_ = 30;
        dataBytesLeft_ = 0;
        currUncompBytes_ = 0;
        currCrc_ = 0;

        const auto& entry = plan_.entries[currentEntryIdx_];
        if (!entry.isDirectory && entry.method == 8) {
            if (inflaterActive_) mz_inflateEnd(&inflater_);
            std::memset(&inflater_, 0, sizeof(inflater_));
            const int res = mz_inflateInit2(&inflater_, -MZ_DEFAULT_WINDOW_BITS);
            require(res == MZ_OK, "Cannot initialize Deflate stream inflater");
            inflaterActive_ = true;
        }
    }

    template<class OnFileData>
    void consumeFileData(const std::uint8_t* chunk, std::size_t len, OnFileData onFileData) {
        const auto& entry = plan_.entries[currentEntryIdx_];
        if (entry.isDirectory) return;

        if (entry.method == 0) { // Store
            onFileData(chunk, len);
            currCrc_ = mz_crc32(currCrc_, chunk, len);
            currUncompBytes_ += len;
            require(currUncompBytes_ <= entry.uncompSize, "Stored file size exceeded expected size");
        } else if (entry.method == 8) { // Deflate
            inflater_.next_in = chunk;
            inflater_.avail_in = static_cast<mz_uint32>(len);

            std::uint8_t outBuf[32768];
            while (inflater_.avail_in > 0) {
                inflater_.next_out = outBuf;
                inflater_.avail_out = sizeof(outBuf);
                const int res = mz_inflate(&inflater_, MZ_NO_FLUSH);
                const std::size_t produced = sizeof(outBuf) - inflater_.avail_out;
                if (produced > 0) {
                    onFileData(outBuf, produced);
                    currCrc_ = mz_crc32(currCrc_, outBuf, produced);
                    currUncompBytes_ += produced;
                    require(currUncompBytes_ <= entry.uncompSize, "Decompressed size exceeded expected entry size");
                }
                if (res == MZ_STREAM_END) break;
                require(res == MZ_OK || res == MZ_BUF_ERROR, "Inflate decompression error");
            }
        }
    }

    template<class OnFileData>
    void flushRemaining(OnFileData onFileData) {
        const auto& entry = plan_.entries[currentEntryIdx_];
        if (entry.isDirectory || entry.method != 8 || !inflaterActive_) return;

        std::uint8_t outBuf[32768];
        while (true) {
            inflater_.next_out = outBuf;
            inflater_.avail_out = sizeof(outBuf);
            const int res = mz_inflate(&inflater_, MZ_FINISH);
            const std::size_t produced = sizeof(outBuf) - inflater_.avail_out;
            if (produced > 0) {
                onFileData(outBuf, produced);
                currCrc_ = mz_crc32(currCrc_, outBuf, produced);
                currUncompBytes_ += produced;
                require(currUncompBytes_ <= entry.uncompSize, "Decompressed size exceeded expected entry size");
            }
            if (res == MZ_STREAM_END) break;
            require(res == MZ_BUF_ERROR && produced == 0, "Inflate finish error");
            if (produced == 0) break;
        }
    }

    template<class OnFileEnd>
    void completeEntry(OnFileEnd onFileEnd) {
        const auto& entry = plan_.entries[currentEntryIdx_];
        if (!entry.isDirectory) {
            if (entry.method == 8 && inflaterActive_) {
                mz_inflateEnd(&inflater_);
                inflaterActive_ = false;
            }
            require(currUncompBytes_ == entry.uncompSize, "Uncompressed size mismatch for " + entry.safePath);
            require(currCrc_ == entry.crc32, "CRC-32 checksum mismatch for " + entry.safePath);
            onFileEnd(entry);
        }
        ++currentEntryIdx_;
        initCurrentEntry();
    }

    updater::ZipStreamPlan plan_;
    std::size_t currentEntryIdx_ = 0;
    State state_ = STATE_HEADER;

    std::vector<std::uint8_t> localHeaderBuf_;
    std::size_t localHeaderTargetLen_ = 30;
    std::size_t dataBytesLeft_ = 0;

    mz_stream inflater_{};
    bool inflaterActive_ = false;
    std::uint32_t currCrc_ = 0;
    std::uint64_t currUncompBytes_ = 0;
};

struct RemoteProbe {
    std::uint64_t totalSize = 0;
    std::string etag;
    bool supportsRange = false;
};

RemoteProbe probeRemote(const std::string& url) {
    http::Request probe(url, L"Range: bytes=0-0\r\n");
    RemoteProbe result{};
    if (probe.status() == 200) {
        result.supportsRange = false;
        const auto len = probe.header(HTTP_QUERY_CONTENT_LENGTH);
        result.totalSize = len.empty() ? 0 : updater::number(len);
        return result;
    }
    require(probe.status() == 206, "Download probe returned HTTP " + std::to_string(probe.status()));
    const auto range = updater::parseContentRange(probe.header(HTTP_QUERY_CONTENT_RANGE));
    updater::validateRange(206, probe.header(HTTP_QUERY_CONTENT_RANGE), 0, 1, range.total);
    result.totalSize = range.total;
    result.supportsRange = true;
    result.etag = probe.header(HTTP_QUERY_ETAG);
    if (result.etag.rfind("W/", 0) == 0) result.etag.clear();
    updater::copyResponse([&](char* p, std::size_t n) { return probe.read(p, n); }, [](const char*, std::size_t) {}, 1, 1);
    return result;
}

updater::ZipStreamPlan fetchAndPlanCentralDirectory(const std::string& url, std::uint64_t totalSize, const std::string& etag) {
    // 1. Fetch the last 65536 bytes (or entire file if smaller)
    const std::uint64_t tailFetch = std::min<std::uint64_t>(65536, totalSize);
    const std::uint64_t tailStart = totalSize - tailFetch;
    std::vector<std::uint8_t> tailBuf;
    tailBuf.reserve(tailFetch);

    auto tailHeaders = L"Range: bytes=" + std::to_wstring(tailStart) + L"-" + std::to_wstring(totalSize - 1) + L"\r\n";
    if (!etag.empty()) tailHeaders += L"If-Match: " + wide(etag) + L"\r\n";

    http::Request tailReq(url, tailHeaders);
    updater::validateRange(tailReq.status(), tailReq.header(HTTP_QUERY_CONTENT_RANGE), tailStart, tailFetch, totalSize);
    updater::copyResponse([&](char* p, std::size_t n) { return tailReq.read(p, n); },
                          [&](const char* p, std::size_t n) {
                              tailBuf.insert(tailBuf.end(), reinterpret_cast<const std::uint8_t*>(p),
                                                            reinterpret_cast<const std::uint8_t*>(p) + n);
                          }, tailFetch, tailFetch);

    const auto eocd = updater::findEocd(tailBuf.data(), tailBuf.size(), totalSize);

    // 2. Fetch Central Directory
    std::vector<std::uint8_t> cdBuf;
    cdBuf.reserve(eocd.cdSize);

    auto cdHeaders = L"Range: bytes=" + std::to_wstring(eocd.cdOffset) + L"-" + std::to_wstring(static_cast<std::uint64_t>(eocd.cdOffset) + eocd.cdSize - 1) + L"\r\n";
    if (!etag.empty()) cdHeaders += L"If-Match: " + wide(etag) + L"\r\n";

    http::Request cdReq(url, cdHeaders);
    updater::validateRange(cdReq.status(), cdReq.header(HTTP_QUERY_CONTENT_RANGE), eocd.cdOffset, eocd.cdSize, totalSize);
    updater::copyResponse([&](char* p, std::size_t n) { return cdReq.read(p, n); },
                          [&](const char* p, std::size_t n) {
                              cdBuf.insert(cdBuf.end(), reinterpret_cast<const std::uint8_t*>(p),
                                                          reinterpret_cast<const std::uint8_t*>(p) + n);
                          }, eocd.cdSize, eocd.cdSize);

    return updater::parseCentralDirectory(cdBuf.data(), cdBuf.size(), eocd.totalEntries, totalSize);
}

// Stream download while inflating and hashing in one pass
void streamDownloadAndExtract(const Remote& remote, const fs::path& staged, const fs::path& zipPath,
                             const updater::ZipStreamPlan& plan, std::uint64_t totalSize, const std::string& etag) {
    ZipStreamUnpacker unpacker(plan);
    Sha256Hasher hasher;

    std::ofstream currentFile;
    fs::path currentFilePath;
    std::size_t extractedCount = 0;

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t bytesReceived = 0;

    auto onDir = [&](const std::string& safePath) {
        const auto dirPath = staged / fs::path(wide(safePath));
        require(pathWithin(dirPath, staged), "ZIP output escaped staging directory");
        fs::create_directories(dirPath);
    };

    auto onFileStart = [&](const updater::PlannedEntry& entry) {
        const auto filePath = staged / fs::path(wide(entry.safePath));
        require(pathWithin(filePath, staged), "ZIP output escaped staging directory");
        safeAncestors(filePath.parent_path());
        rejectReparse(filePath);
        fs::create_directories(filePath.parent_path());
        // Resumable stream unpack: if already cleanly extracted with exact size, skip re-extracting
        std::error_code ec;
        if (fs::exists(filePath, ec) && fs::file_size(filePath, ec) == entry.uncompSize) {
            currentFilePath.clear();
            return;
        }
        currentFile.open(filePath, std::ios::binary | std::ios::trunc);
        require(currentFile.good(), "Cannot create extracted file: " + pathText(filePath));
        currentFilePath = filePath;
    };

    auto onFileData = [&](const std::uint8_t* chunk, std::size_t len) {
        if (currentFile.is_open()) {
            currentFile.write(reinterpret_cast<const char*>(chunk), static_cast<std::streamsize>(len));
            require(currentFile.good(), "Write failed for " + pathText(currentFilePath));
        }
    };

    auto onFileEnd = [&](const updater::PlannedEntry&) {
        if (currentFile.is_open()) {
            finishFile(currentFile);
        }
        ++extractedCount;
    };

    // Download sequential stream
    auto headers = L"Range: bytes=0-" + std::to_wstring(totalSize - 1) + L"\r\n";
    if (!etag.empty()) headers += L"If-Match: " + wide(etag) + L"\r\n";

    http::Request req(remote.url, headers);
    updater::validateRange(req.status(), req.header(HTTP_QUERY_CONTENT_RANGE), 0, totalSize, totalSize);

    const std::uint64_t chunkSize = 2ULL * 1024 * 1024;
    // total chunks computed on demand
    const auto metaPath = zipPath.string() + ".part.json";
    std::ofstream zipFile(zipPath, std::ios::binary | std::ios::trunc);
    require(zipFile.good(), "Cannot create archive cache file");

    auto saveStreamMeta = [&](std::uint64_t currentBytes) {
        std::vector<std::size_t> doneIndices;
        std::size_t fullChunks = static_cast<std::size_t>(currentBytes / chunkSize);
        doneIndices.reserve(fullChunks);
        for (std::size_t i = 0; i < fullChunks; ++i) doneIndices.push_back(i);
        json meta = {
            {"url", remote.url},
            {"etag", etag},
            {"size", totalSize},
            {"chunkSize", chunkSize},
            {"sha256", remote.sha256},
            {"completed", doneIndices}
        };
        try {
            writeJson(metaPath, meta);
        } catch (...) {}
    };

    std::uint64_t lastMetaBytes = 0;
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (bytesReceived < totalSize) {
        if (g_interruptRequested.load()) {
            bool shouldExit = ui.promptCancelOrExit("流式下载与解压");
            if (shouldExit) {
                finishFile(zipFile);
                ui.finish(false, "用户已强制退出 / 取消");
                exit(130);
            }
        }
        const std::size_t toRead = std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(totalSize - bytesReceived));
        const std::size_t n = req.read(reinterpret_cast<char*>(buffer.data()), toRead);
        require(n > 0, "Unexpected early EOF during streaming download");

        zipFile.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n));
        hasher.update(buffer.data(), n);
        unpacker.feed(bytesReceived, buffer.data(), n, onDir, onFileStart, onFileData, onFileEnd);
        bytesReceived += n;

        if (bytesReceived - lastMetaBytes >= chunkSize) {
            zipFile.flush();
            saveStreamMeta(bytesReceived);
            lastMetaBytes = bytesReceived;
        }

        if (interactiveOutput) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double rate = elapsed > 0.01 ? bytesReceived / elapsed : 0;
            const double ratio = totalSize ? static_cast<double>(bytesReceived) / static_cast<double>(totalSize) : 0.0;
            const std::uint64_t eta = (rate > 0 && bytesReceived < totalSize) ? static_cast<std::uint64_t>((totalSize - bytesReceived) / rate) : 0;
            if (ui.enabled) {
                ui.renderDashboard(ratio, bytesReceived / 1048576.0, totalSize / 1048576.0,
                                   rate / 1048576.0, eta, extractedCount, plan.entries.size(),
                                   currentFilePath.filename().string());
            } else {
                std::ostringstream line;
                line << "\r边下边解: " << (totalSize ? std::min<std::uint64_t>(100, bytesReceived * 100 / totalSize) : 0)
                     << "%  " << std::fixed << std::setprecision(1)
                     << bytesReceived / 1048576.0 << "/" << totalSize / 1048576.0 << " MiB  "
                     << rate / 1048576.0 << " MiB/s  [" << extractedCount << "/" << plan.entries.size() << " files]";
                if (rate > 0 && bytesReceived < totalSize) {
                    line << "  ETA " << eta << "s";
                }
                std::cout << line.str() << "                    " << std::flush;
            }
        }
    }

    if (interactiveOutput && !ui.enabled) {
        std::cout << "\r边下边解: 完成 (已提取 " << extractedCount << " 个文件)                                              \n";
    }

    finishFile(zipFile);
    require(unpacker.isComplete(), "Incomplete archive decompression");
    ui.setStep(ModernUI::STEP_EXTRACT, StepStatus::Success, "已提取 " + std::to_string(extractedCount) + " 个文件");
    ui.setStep(ModernUI::STEP_VERIFY, StepStatus::Active, "计算哈希值...");
    const std::string computedSha = hasher.finish();
    require(computedSha == remote.sha256, "SHA-256 mismatch; stream integrity verification failed");
    std::error_code ec;
    fs::remove(metaPath, ec);
}

struct Zip {
    mz_zip_archive archive{};
    FILE* file = nullptr;
    bool initialized = false;
    explicit Zip(const fs::path& path) {
        file = _wfopen(path.c_str(), L"rb");
        require(file != nullptr, "Cannot open ZIP file");
        initialized = mz_zip_reader_init_cfile(&archive, file, 0, 0) != 0;
        if (!initialized) { std::fclose(file); file = nullptr; throw std::runtime_error("Invalid ZIP archive"); }
    }
    ~Zip() { if (initialized) mz_zip_reader_end(&archive); if (file) std::fclose(file); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;
};

struct Entry { mz_uint index; fs::path relative; std::uint64_t size; bool directory; std::string u8Name; };
struct ZipPlan { std::vector<Entry> entries; std::uint64_t expanded = 0; };

ZipPlan inspectZip(const fs::path& file) {
    Zip zip(file);
    const auto count = mz_zip_reader_get_num_files(&zip.archive);
    require(count > 0 && count <= updater::maxEntries, "Invalid ZIP entry count");
    ZipPlan plan;
    std::map<std::wstring, bool, OrdinalLess> names;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        require(mz_zip_reader_file_stat(&zip.archive, i, &st) != 0, "Cannot inspect ZIP entry");
        require(!st.m_is_encrypted && st.m_is_supported, "Encrypted/unsupported ZIP entry");
        const auto length = mz_zip_reader_get_filename(&zip.archive, i, nullptr, 0);
        require(length > 1 && length <= 4097, "Invalid ZIP filename length");
        std::vector<char> name(length, '\0');
        require(mz_zip_reader_get_filename(&zip.archive, i, name.data(), length) == length &&
                std::strlen(name.data()) == length - 1, "Truncated/embedded-NUL ZIP filename");
        const auto safe = updater::safeArchivePath(std::string(name.data(), length - 1));
        const bool directory = mz_zip_reader_is_file_a_directory(&zip.archive, i) != 0;
        updater::validateZipAttributes(st.m_external_attr, directory);
        require(st.m_uncomp_size <= updater::maxEntryBytes && st.m_uncomp_size <= updater::maxExpandedBytes - plan.expanded,
                "ZIP expanded size limit exceeded");
        plan.expanded += st.m_uncomp_size;
        const auto relative = fs::path(wide(safe));
        require(names.emplace(relative.generic_wstring(), directory).second, "Duplicate/case-colliding ZIP entry");
        plan.entries.push_back({i, relative, st.m_uncomp_size, directory, safe});
    }
    for (const auto& entry : plan.entries) {
        for (auto parent = entry.relative.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            const auto found = names.find(parent.generic_wstring());
            require(found == names.end() || found->second, "ZIP file/directory path conflict");
        }
    }
    return plan;
}

struct ExtractOutput { std::ofstream file; std::uint64_t written = 0, limit = 0; bool failed = false; const std::atomic<bool>* cancel; };
size_t extractWrite(void* opaque, mz_uint64 offset, const void* buffer, size_t count) noexcept {
    auto& out = *static_cast<ExtractOutput*>(opaque);
    try {
        if (out.cancel->load() || offset != out.written || out.written > out.limit || count > out.limit - out.written) {
            out.failed = true; return 0;
        }
        out.file.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(count));
        if (!out.file.good()) { out.failed = true; return 0; }
        out.written += count;
        return count;
    } catch (...) { out.failed = true; return 0; }
}

void extractZip(const fs::path& file, const fs::path& target, const ZipPlan& plan, int threadCount) {
    std::vector<const Entry*> jobs;
    std::size_t alreadyExtracted = 0;
    for (const auto& entry : plan.entries) {
        const auto output = target / entry.relative;
        require(pathWithin(output, target), "ZIP output escaped staging directory");
        fs::create_directories(entry.directory ? output : output.parent_path());
        if (!entry.directory) {
            // Check if file is already completely extracted in previous attempt (resumable extraction)
            std::error_code ec;
            if (fs::exists(output, ec) && fs::is_regular_file(output, ec) && fs::file_size(output, ec) == entry.size) {
                ++alreadyExtracted;
            } else {
                jobs.push_back(&entry);
            }
        }
    }
    const std::size_t totalFiles = plan.entries.size();
    if (jobs.empty()) {
        if (interactiveOutput && !ui.enabled) std::cout << "\r断点解压: 全部 " << totalFiles << " 个文件已就绪                                                            \n";
        return;
    }
    std::sort(jobs.begin(), jobs.end(), [](const Entry* a, const Entry* b) { return a->size > b->size; });
    const int count = static_cast<int>(std::min<std::size_t>(threadCount, jobs.size()));
    std::atomic<std::size_t> next{0};
    std::vector<Progress> progress(count);
    progress[0].count = alreadyExtracted;

    std::atomic<const char*> lastExtractedName{""};
    parallelWork(count, progress, totalFiles, "断点解压", false, &lastExtractedName, [&](int id, const std::atomic<bool>& cancel) {
        Zip zip(file);
        while (!cancel) {
            const auto n = next.fetch_add(1);
            if (n >= jobs.size()) return;
            const auto& entry = *jobs[n];
            const auto output = target / entry.relative;
            safeAncestors(output.parent_path());
            rejectReparse(output);
            ExtractOutput sink{std::ofstream(output, std::ios::binary | std::ios::trunc), 0, entry.size, false, &cancel};
            require(sink.file.good(), "Cannot create extracted file: " + pathText(output));
            const bool ok = mz_zip_reader_extract_to_callback(&zip.archive, entry.index, extractWrite, &sink, 0) != 0;
            require(ok && !sink.failed && sink.written == entry.size, "ZIP extraction/CRC/write failed: " + pathText(entry.relative));
            finishFile(sink.file);
            lastExtractedName.store(entry.u8Name.c_str());
            ++progress[id].count;
        }
        throw std::runtime_error("Extraction cancelled");
    });
}

std::uint64_t dataSize(const fs::path& data) {
    rejectReparse(data);
    if (!fs::exists(data)) return 0;
    require(fs::is_directory(data), "Portable data path is not a directory");
    std::uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(data)) {
        rejectReparse(entry.path());
        require(entry.is_directory() || entry.is_regular_file(), "Unsupported file in portable data");
        if (entry.is_regular_file()) {
            const auto size = entry.file_size();
            require(size <= UINT64_MAX - total, "Portable data size overflow");
            total += size;
        }
    }
    return total;
}

void copyData(const fs::path& source, const fs::path& dest) {
    if (!fs::exists(source)) return;
    rejectReparse(source);
    fs::create_directory(dest);
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        rejectReparse(entry.path());
        const auto output = dest / entry.path().lexically_relative(source);
        require(pathWithin(output, dest), "Portable data path escaped staging directory");
        if (entry.is_directory()) fs::create_directories(output);
        else {
            require(entry.is_regular_file(), "Unsupported portable data entry");
            fs::create_directories(output.parent_path());
            fs::copy_file(entry.path(), output); // do not copy symlinks or replace existing data
        }
    }
}

std::string randomSuffix() {
    std::array<UCHAR, 12> bytes{};
    require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
            "Cannot create a unique staging name");
    std::ostringstream s;
    for (auto b : bytes) s << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
    return s.str();
}

struct Workspace {
    fs::path root;
    bool preserve = false;
    explicit Workspace(const fs::path& target) {
        for (int i = 0; i < 10; ++i) {
            const auto candidate = target.parent_path() / (L"." + target.filename().native() + L".update-" + wide(randomSuffix()));
            if (fs::create_directory(candidate)) { root = candidate; return; }
        }
        throw std::runtime_error("Cannot create an exclusive staging directory");
    }
    ~Workspace() {
        if (!preserve && !root.empty()) {
            std::error_code ec;
            fs::remove_all(root, ec); // only this exclusively created, owned staging directory
            if (ec) std::cerr << "临时目录未清理，请检查: " << pathText(root) << '\n';
        }
    }
};

int run(const updater::Options& options) {
    const auto target = validateTarget(options.directory);
    ui.init(interactiveOutput, options.plain, pathText(target), options.arch, options.quality);
    ui.renderHeader();

    ui.setStep(ModernUI::STEP_QUERY, StepStatus::Active, "连接更新服务器...");
    const auto remote = fetchRemote(options); // fail closed: no unverified fallback download
    const auto local = localIdentity(target);
    const bool current = updater::sameBuild(local, remote.id);
    ui.setStep(ModernUI::STEP_QUERY, StepStatus::Success, remote.id.version + " (" + remote.id.commit.substr(0, 7) + ")");

    if (options.check) {
        if (current) {
            ui.finish(true, "已是最新版本 (" + remote.id.version + ")，无需更新");
        } else {
            ui.finish(true, "检测到新版本: " + remote.id.version + " (当前: " + (local.version.empty() ? "未安装" : local.version) + ")");
        }
        return 0;
    }
    if (current && !options.force) {
        // If interactive, prompt dialog whether to continue reinstalling
        if (interactiveOutput && !options.plain) {
            bool proceed = ui.promptConfirmContinue(remote.id.version, remote.id.commit);
            if (!proceed) {
                for (int s = 1; s < ModernUI::STEP_COUNT; ++s) {
                    ui.setStep(static_cast<ModernUI::StepIndex>(s), StepStatus::Skipped);
                }
                ui.finish(true, "用户取消操作，当前已是最新版本: " + remote.id.version);
                return 0;
            }
            ui.setStep(ModernUI::STEP_QUERY, StepStatus::Success, remote.id.version + " (用户确认重新安装)");
        } else {
            for (int s = 1; s < ModernUI::STEP_COUNT; ++s) {
                ui.setStep(static_cast<ModernUI::StepIndex>(s), StepStatus::Skipped);
            }
            ui.finish(true, "无需更新: " + remote.id.version);
            return 0;
        }
    }

    fs::create_directories(target.parent_path());
    safeAncestors(target);
    const auto lockPath = target.parent_path() / (L"." + target.filename().native() + L".updater.lock");
    rejectReparse(lockPath);
    WinHandle lock(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    require(lock.value != INVALID_HANDLE_VALUE, "Another updater is using this target, or the parent directory is not writable");
    ensureNotRunning(target);

    const auto cacheDir = target.parent_path() / (L"." + target.filename().native() + L".cache");
    fs::create_directories(cacheDir);
    const auto zip = cacheDir / (L"vscode-" + wide(remote.id.version) + L"-" + wide(remote.id.commit.substr(0, 10)) + L".zip");
    const auto staged = cacheDir / (L"staged-" + wide(remote.id.version) + L"-" + wide(remote.id.commit.substr(0, 10)));
    fs::create_directories(staged);
    const int automatic = static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    const int threads = options.threads ? options.threads : automatic;
    const auto portableBytes = dataSize(target / "data");
    const std::uint64_t reserve = 64ULL * 1024 * 1024;
    bool streamed = false;

    bool alreadyHasValidZip = false;
    {
        std::error_code ec;
        if (fs::exists(zip, ec) && fs::file_size(zip, ec) > 0) {
            try {
                if (sha256(zip) == remote.sha256) alreadyHasValidZip = true;
            } catch (...) {}
        }
    }

    if (options.stream && !options.keepZip && !alreadyHasValidZip) {
        // Stream mode requested and no complete archive cached yet
        ui.steps[ModernUI::STEP_DOWNLOAD].name = "边下载边解压 (流式)";
        ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "探测分段支持并预读中央目录...");
        try {
            const auto probe = probeRemote(remote.url);
            if (probe.supportsRange && probe.totalSize > 0 && probe.totalSize <= updater::maxArchiveBytes) {
                const auto streamPlan = fetchAndPlanCentralDirectory(remote.url, probe.totalSize, probe.etag);
                if (streamPlan.isSequential) {
                    const auto freeBytes = fs::space(target.parent_path()).available;
                    require(freeBytes > reserve && streamPlan.expanded <= freeBytes - reserve &&
                            portableBytes <= freeBytes - reserve - streamPlan.expanded,
                            "Not enough disk space for streaming extraction and portable user data");
                    ui.setStep(ModernUI::STEP_AUDIT, StepStatus::Success, "安全校验通过 (" + std::to_string(streamPlan.entries.size()) + " files)");
                    ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "流式提取中 (边下边解)...");
                    streamDownloadAndExtract(remote, staged, zip, streamPlan, probe.totalSize, probe.etag);
                    ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Success, "流式传输与提取完成");
                    ui.setStep(ModernUI::STEP_VERIFY, StepStatus::Success, "SHA-256 核验一致");
                    ui.setStep(ModernUI::STEP_EXTRACT, StepStatus::Success, "全部文件提取就绪 (" + std::to_string(streamPlan.entries.size()) + " files)");
                    streamed = true;
                } else {
                    ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "非顺序排列，转为常规分段");
                }
            } else {
                ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "不支持分段，转为常规下载");
            }
        } catch (const std::exception& e) {
            ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "预检未通过，转为常规下载");
        }
    }

    if (!streamed) {
        // 1. Download stage with multi-chunk resumable downloading
        if (alreadyHasValidZip) {
            ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Success, "完整安装包已就绪 (无需重复下载)");
        } else {
            ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Active, "多线程分段断点下载...");
            download(remote, zip, threads);
            ui.setStep(ModernUI::STEP_DOWNLOAD, StepStatus::Success, "下载完成 (已缓存)");
        }

        // 2. Hash verification
        ui.setStep(ModernUI::STEP_VERIFY, StepStatus::Active, "核验官方 SHA-256...");
        require(sha256(zip) == remote.sha256, "SHA-256 mismatch; refusing to install this archive");
        ui.setStep(ModernUI::STEP_VERIFY, StepStatus::Success, "SHA-256 核验一致");

        // 3. Inspect ZIP and security audit
        ui.setStep(ModernUI::STEP_AUDIT, StepStatus::Active, "解包目录安全审计...");
        const auto plan = inspectZip(zip);
        const auto freeBytes = fs::space(target.parent_path()).available;
        require(freeBytes > reserve && plan.expanded <= freeBytes - reserve && portableBytes <= freeBytes - reserve - plan.expanded,
                "Not enough disk space for extraction and a copy of portable user data");
        ui.setStep(ModernUI::STEP_AUDIT, StepStatus::Success, "安全校验通过 (" + std::to_string(plan.entries.size()) + " files)");

        // 4. Resumable multi-threaded extraction
        ui.setStep(ModernUI::STEP_EXTRACT, StepStatus::Active, "解压释放并就绪...");
        extractZip(zip, staged, plan, threads);
        ui.setStep(ModernUI::STEP_EXTRACT, StepStatus::Success, "全部解压就绪 (" + std::to_string(plan.entries.size()) + " files)");
    }

    const auto stagedIdentity = localIdentity(staged);
    require(updater::sameBuild(stagedIdentity, remote.id), "Extracted VS Code version/commit/channel/architecture does not match official metadata");
    ensureNotRunning(target);

    // Only create ephemeral switch workspace when all files are 100% verified and ready
    Workspace workspace(target);
    const auto backup = workspace.root / "previous";

    ui.setStep(ModernUI::STEP_APPLY, StepStatus::Active, "保留 portable data 目录...");
    copyData(target / "data", staged / "data");
    writeJson(staged / ".vscode-updater.json", {{"version", remote.id.version}, {"commit", remote.id.commit},
              {"arch", remote.id.arch}, {"quality", remote.id.quality}, {"sha256", remote.sha256}});
    validateTarget(options.directory);
    ensureNotRunning(target);
    const bool hasOld = fs::exists(target);
    writeJson(workspace.root / "recovery.json", {{"target", pathText(target)}, {"backup", pathText(backup)},
              {"staged", pathText(staged)}, {"state", "prepared"}});
    workspace.preserve = true; // never automatically remove recovery files once switching starts
    const std::map<std::string, fs::path> locations{{"target", target}, {"backup", backup}, {"staged", staged}};
    try {
        updater::activate(hasOld, [&](const char* from, const char* to) { fs::rename(locations.at(from), locations.at(to)); });
    } catch (...) {
        ui.setStep(ModernUI::STEP_APPLY, StepStatus::Failed, "切换失败已回滚");
        std::cerr << "切换失败，已尝试回滚。请检查恢复目录: " << pathText(workspace.root) << '\n';
        throw;
    }
    try {
        writeJson(workspace.root / "recovery.json", {{"target", pathText(target)}, {"backup", pathText(backup)}, {"state", "installed"}});
        if (!options.keepZip) {
            std::error_code ec;
            fs::remove_all(cacheDir, ec);
        }
        if (!hasOld && !options.keepZip) { fs::remove(workspace.root / "recovery.json"); fs::remove(workspace.root); }
    } catch (const std::exception& e) { std::cerr << "更新成功，但清理/记录失败: " << e.what() << '\n'; }

    ui.setStep(ModernUI::STEP_APPLY, StepStatus::Success, "切换完成并就绪");
    ui.finish(true, "VS Code " + remote.id.version + " 更新成功！");

    if (hasOld) std::cout << "旧版本及其原始 data 已备份: " << pathText(backup) << '\n';
    if (options.keepZip) std::cout << "已保留安装包: " << pathText(zip) << '\n';
    std::cout << "启动程序: " << pathText(target / (options.quality == "insider" ? "Code - Insiders.exe" : "Code.exe")) << '\n';
    return 0;
}

void usage() {
    std::cout << "VS Code 便携版安全更新器\n"
        "用法: vscode_updater.exe [选项]\n"
        "  --dir <路径>       安装目录，默认 ./Application\n"
        "  --arch x64|arm64   目标架构，默认本机原生架构\n"
        "  --quality stable|insider  默认 stable\n"
        "  --threads <1-16>   下载/解压线程数，默认自动\n"
        "  --check            仅检查；检查失败返回非零退出码\n"
        "  --force            重新安装目标构建，不绕过安全校验\n"
        "  --stream           边下载边解压流式提速（默认启用）\n"
        "  --no-stream        禁用流式解压，采用先完整下载再解压\n"
        "  --plain            纯文本输出模式（关闭图形化 TUI 面板）\n"
        "  --keep-zip         保留已验证的安装包（自动禁用流式）\n"
        "  --pause            完成后等待按键（仅交互终端）\n"
        "  --no-pause         不等待按键（默认）\n"
        "  --help, -h         显示帮助\n";
}
} // namespace

int main() {
    Console console;
    bool pause = false;
    int result = 1;
    try {
        int argc = 0;
        LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &argc);
        require(raw != nullptr, "Cannot parse command line");
        struct FreeArgs { LPWSTR* p; ~FreeArgs() { LocalFree(p); } } freeArgs{raw};
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.push_back(utf8(raw[i]));
        SYSTEM_INFO system{};
        GetNativeSystemInfo(&system);
        updater::Options options;
        try { options = updater::parseOptions(args, system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x64"); }
        catch (const std::exception& e) { std::cerr << "参数错误: " << e.what() << '\n'; return 2; }
        pause = options.pause;
        if (options.help) { usage(); return 0; }
        result = run(options);
    } catch (const std::exception& e) { std::cerr << "错误: " << e.what() << '\n'; }
    catch (...) { std::cerr << "错误: 未预期的内部异常。\n"; }
    DWORD inputMode = 0;
    if (pause && interactiveOutput && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &inputMode)) {
        std::cout << "按任意键退出..." << std::flush;
        _getch();
        std::cout << '\n';
    }
    return result;
}
