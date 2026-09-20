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

bool looksLikeVSCode(const fs::path& p) {
    return fs::is_regular_file(p / "resources/app/product.json") &&
           fs::is_regular_file(p / "resources/app/package.json") &&
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
        const auto product = readJson(dir / "resources/app/product.json");
        const auto package = readJson(dir / "resources/app/package.json");
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

struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    ~Hash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

std::string sha256(const fs::path& file) {
    Hash h;
    require(BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot initialize SHA-256");
    DWORD size = 0, returned = 0;
    require(BCryptGetProperty(h.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) >= 0,
            "Cannot query SHA-256 state size");
    h.object.resize(size);
    require(BCryptCreateHash(h.algorithm, &h.hash, h.object.data(), size, nullptr, 0, 0) >= 0, "Cannot create SHA-256 state");
    std::ifstream f(file, std::ios::binary);
    require(f.good(), "Cannot read downloaded archive");
    std::array<char, 128 * 1024> data{};
    while (f.read(data.data(), data.size()) || f.gcount())
        require(BCryptHashData(h.hash, reinterpret_cast<PUCHAR>(data.data()), static_cast<ULONG>(f.gcount()), 0) >= 0, "SHA-256 update failed");
    require(f.eof() && !f.bad(), "Archive read failed during SHA-256 verification");
    std::array<UCHAR, 32> digest{};
    require(BCryptFinishHash(h.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA-256 finalization failed");
    std::ostringstream text;
    for (auto b : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
    return text.str();
}

struct Progress {
    std::atomic<std::uint64_t> count{0};
};

// Every worker catches exceptions. Even partial thread creation is joined safely.
template<class Work>
void parallelWork(int count, std::vector<Progress>& progress, std::uint64_t total, const char* title, bool byteProgress, Work work) {
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
        if (interactiveOutput) {
            std::uint64_t current = 0;
            for (auto& p : progress) current += p.count.load();
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double rate = elapsed > 0.01 ? current / elapsed : 0;
            std::ostringstream line;
            line << '\r' << title << ": " << (total ? std::min<std::uint64_t>(100, current * 100 / total) : 0)
                 << "%  " << std::fixed << std::setprecision(1);
            if (byteProgress) line << current / 1048576.0 << "/" << total / 1048576.0 << " MiB  " << rate / 1048576.0 << " MiB/s";
            else line << current << "/" << total << " files  " << rate << " files/s";
            if (rate > 0 && current < total) line << "  ETA " << static_cast<unsigned long long>((total - current) / rate) << "s";
            std::cout << line.str() << "                    " << std::flush;
        }
        std::this_thread::sleep_for(100ms);
    }
    for (auto& t : workers) t.join();
    if (interactiveOutput) std::cout << '\r' << title << ": finished                                                            \n";
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
    { std::ofstream f(path, std::ios::binary | std::ios::trunc); require(f.good(), "Cannot create download file"); }
    fs::resize_file(path, size);
    const int count = static_cast<int>(std::min<std::uint64_t>(requestedThreads, std::max<std::uint64_t>(1, size / (256 * 1024))));
    std::vector<Progress> progress(count);
    parallelWork(count, progress, size, "下载", true, [&](int id, const std::atomic<bool>& cancel) {
        const auto first = size * id / count;
        const auto length = size * (id + 1) / count - first;
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                require(!cancel, "Download cancelled because another worker failed");
                progress[id].count = 0;
                auto headers = L"Range: bytes=" + std::to_wstring(first) + L"-" + std::to_wstring(first + length - 1) + L"\r\n";
                if (!etag.empty()) headers += L"If-Match: " + wide(etag) + L"\r\n";
                http::Request request(remote.url, headers);
                updater::validateRange(request.status(), request.header(HTTP_QUERY_CONTENT_RANGE), first, length, size);
                const auto contentLength = request.header(HTTP_QUERY_CONTENT_LENGTH);
                require(contentLength.empty() || updater::number(contentLength) == length, "Segment Content-Length mismatch");
                std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
                require(f.good(), "Cannot open segment output file");
                f.seekp(static_cast<std::streamoff>(first));
                require(f.good(), "Cannot seek segment output file");
                updater::copyResponse([&](char* p, std::size_t n) {
                    require(!cancel, "Download cancelled");
                    return request.read(p, n);
                }, [&](const char* p, std::size_t n) {
                    f.write(p, n);
                    require(f.good(), "Segment write failed");
                    progress[id].count += n;
                }, length, length);
                f.flush();
                require(f.good(), "Segment flush failed");
                f.close();
                require(!f.fail(), "Segment close failed");
                return;
            } catch (...) {
                if (attempt == 2 || cancel.load()) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(300 * (1 << attempt)));
            }
        }
    });
    require(fs::file_size(path) == size, "Final archive size mismatch");
}

void download(const Remote& remote, const fs::path& file, int threads) {
    // Segment retries preserve completed ranges. An overall failure gets one fresh retry.
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            downloadOnce(remote, file, threads);
            std::cout << "校验 SHA-256...\n";
            require(sha256(file) == remote.sha256, "SHA-256 mismatch; refusing to install this archive");
            return;
        } catch (const std::exception& e) {
            if (attempt == 1) throw;
            std::cerr << "下载失败，将重新尝试一次: " << e.what() << '\n';
            std::this_thread::sleep_for(1s);
        }
    }
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

struct Entry { mz_uint index; fs::path relative; std::uint64_t size; bool directory; };
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
        plan.entries.push_back({i, relative, st.m_uncomp_size, directory});
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
    for (const auto& entry : plan.entries) {
        const auto output = target / entry.relative;
        require(pathWithin(output, target), "ZIP output escaped staging directory");
        fs::create_directories(entry.directory ? output : output.parent_path());
        if (!entry.directory) jobs.push_back(&entry);
    }
    require(!jobs.empty(), "ZIP contains no files");
    std::sort(jobs.begin(), jobs.end(), [](const Entry* a, const Entry* b) { return a->size > b->size; });
    const int count = static_cast<int>(std::min<std::size_t>(threadCount, jobs.size()));
    std::atomic<std::size_t> next{0};
    std::vector<Progress> progress(count);
    parallelWork(count, progress, jobs.size(), "解压", false, [&](int id, const std::atomic<bool>& cancel) {
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
    std::cout << "VS Code 更新器 | " << options.arch << " | " << options.quality << "\n目标: " << pathText(target) << '\n';
    std::cout << "正在查询官方更新信息...\n";
    const auto remote = fetchRemote(options); // fail closed: no unverified fallback download
    const auto local = localIdentity(target);
    const bool current = updater::sameBuild(local, remote.id);
    std::cout << "远程版本: " << remote.id.version << " (" << remote.id.commit.substr(0, 12) << ")\n";
    if (options.check) {
        std::cout << (current ? "已是目标架构/渠道的最新构建。\n" : "需要安装或更新目标构建。\n");
        return 0;
    }
    if (current && !options.force) { std::cout << "无需更新。\n"; return 0; }
    fs::create_directories(target.parent_path());
    safeAncestors(target);
    const auto lockPath = target.parent_path() / (L"." + target.filename().native() + L".updater.lock");
    rejectReparse(lockPath);
    WinHandle lock(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    require(lock.value != INVALID_HANDLE_VALUE, "Another updater is using this target, or the parent directory is not writable");
    ensureNotRunning(target);
    Workspace workspace(target);
    const auto zip = workspace.root / "vscode.zip", staged = workspace.root / "staged", backup = workspace.root / "previous";
    fs::create_directory(staged);
    const int automatic = static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    const int threads = options.threads ? options.threads : automatic;
    std::cout << "下载并验证安装包...\n";
    download(remote, zip, threads);
    const auto plan = inspectZip(zip);
    const auto portableBytes = dataSize(target / "data");
    const auto freeBytes = fs::space(workspace.root).available;
    const std::uint64_t reserve = 64ULL * 1024 * 1024;
    require(freeBytes > reserve && plan.expanded <= freeBytes - reserve && portableBytes <= freeBytes - reserve - plan.expanded,
            "Not enough disk space for extraction and a copy of portable user data");
    extractZip(zip, staged, plan, threads);
    const auto stagedIdentity = localIdentity(staged);
    require(updater::sameBuild(stagedIdentity, remote.id), "Extracted VS Code version/commit/channel/architecture does not match official metadata");
    ensureNotRunning(target);
    std::cout << "保留便携版 data 目录...\n";
    copyData(target / "data", staged / "data");
    writeJson(staged / ".vscode-updater.json", {{"version", remote.id.version}, {"commit", remote.id.commit},
              {"arch", remote.id.arch}, {"quality", remote.id.quality}, {"sha256", remote.sha256}});
    // Revalidate immediately before changing the installation. A process could still
    // start after this check; Windows rename failure then takes the rollback path.
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
        std::cerr << "切换失败，已尝试回滚。请检查恢复目录: " << pathText(workspace.root) << '\n';
        throw;
    }
    // Installation is committed. Cleanup failures must not be reported as a failed installation.
    try {
        writeJson(workspace.root / "recovery.json", {{"target", pathText(target)}, {"backup", pathText(backup)}, {"state", "installed"}});
        if (!options.keepZip) fs::remove(zip);
        if (!hasOld && !options.keepZip) { fs::remove(workspace.root / "recovery.json"); fs::remove(workspace.root); }
    } catch (const std::exception& e) { std::cerr << "更新成功，但清理/记录失败: " << e.what() << '\n'; }
    std::cout << "更新成功: " << remote.id.version << '\n';
    if (hasOld) std::cout << "旧版本及其原始 data 已备份，确认新版本正常后可手动清理: " << pathText(backup) << '\n';
    if (options.keepZip) std::cout << "已保留安装包: " << pathText(zip) << '\n';
    std::cout << "启动: " << pathText(target / (options.quality == "insider" ? "Code - Insiders.exe" : "Code.exe")) << '\n';
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
        "  --keep-zip         保留已验证的安装包\n"
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
