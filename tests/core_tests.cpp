#include "../updater_core.hpp"
#include <cstring>
#include <iostream>
#include <set>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F fn, const char* message) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
}

int main() {
    using namespace updater;
    try {
        check(number("123") == 123, "integer parsing");
        for (const auto* s : {"", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616"})
            rejects([&] { number(s); }, "invalid number accepted");
        check(parseOptions({}).threads == 0, "automatic threads");
        check(parseOptions({}, "arm64").arch == "arm64", "native arm64");
        check(!parseOptions({}).pause, "must not pause by default");
        check(parseOptions({"--threads", "16", "--check", "--quality", "insider"}).threads == 16, "CLI options");
        check(!parseOptions({"--pause", "--no-pause"}).pause, "explicit no-pause");
        for (const auto* arg : {"0", "17", "-1", "abc", "8x", "99999999999999999999"})
            rejects([&] { parseOptions({"--threads", arg}); }, "bad thread count accepted");
        rejects([] { parseOptions({"--threads"}); }, "missing threads");
        rejects([] { parseOptions({"--dir", "--force"}); }, "flag used as directory");
        rejects([] { parseOptions({"--unknown"}); }, "unknown option");
        rejects([] { parseOptions({"--arch", "x86"}); }, "bad arch");
        rejects([] { parseOptions({"--quality", "beta"}); }, "bad quality");
        rejects([] { parseOptions({"--dir", ""}); }, "empty directory");

        Identity a{"1.100.0", std::string(40, 'a'), "x64", "stable"};
        check(sameBuild(a, a), "same build not detected");
        auto b = a; b.arch = "arm64"; check(!sameBuild(a, b), "architecture switch skipped");
        b = a; b.quality = "insider"; check(!sameBuild(a, b), "channel switch skipped");
        b = a; b.commit = std::string(40, 'b'); check(!sameBuild(a, b), "new commit with same version skipped");
        b = a; b.version += "-insider"; check(!sameBuild(a, b), "insider suffix ignored");
        b = a; b.commit.clear(); check(!sameBuild(b, b), "unknown build treated as current");

        check(parseContentRange("bytes 0-0/100").total == 100, "range parsing");
        validateRange(206, "bytes 10-19/100", 10, 10, 100); ++checks;
        for (const auto* s : {"bytes */100", "bytes 9-1/100", "bytes 0-100/100", "items 0-1/2", "bytes 0-1/*", "bytes 0-1/2junk"})
            rejects([&] { parseContentRange(s); }, "bad content range accepted");
        rejects([] { validateRange(200, "bytes 0-9/10", 0, 10, 10); }, "ignored range accepted");
        rejects([] { validateRange(404, "bytes 0-9/10", 0, 10, 10); }, "HTTP error accepted");
        rejects([] { validateRange(206, "bytes 0-8/10", 0, 10, 10); }, "short range accepted");
        rejects([] { validateRange(206, "bytes 1-9/10", 0, 9, 10); }, "wrong offset accepted");
        rejects([] { validateRange(206, "bytes 0-9/11", 0, 10, 10); }, "resource size change accepted");

        auto reader = [](std::string data) {
            return [data, used = false](char* out, std::size_t) mutable -> std::size_t {
                if (used) return 0;
                used = true;
                std::memcpy(out, data.data(), data.size());
                return data.size();
            };
        };
        std::string output;
        auto write = [&](const char* p, std::size_t n) { output.append(p, n); };
        check(copyResponse(reader("hello"), write, 5, 5) == 5 && output == "hello", "complete response");
        rejects([&] { copyResponse(reader("abc"), write, 5, 5); }, "truncated response accepted");
        output.clear();
        rejects([&] { copyResponse(reader("abcdef"), write, 5, 5); }, "oversized response accepted");
        check(output.empty(), "oversized chunk written across a boundary");
        rejects([&] { copyResponse(reader(""), write, 5); }, "empty response accepted");
        int reads = 0;
        rejects([&] {
            copyResponse([&](char* p, std::size_t) -> std::size_t {
                if (reads++) throw std::runtime_error("simulated network reset");
                p[0] = 'x'; return 1;
            }, write, 10);
        }, "transport error treated as EOF");
        rejects([&] { copyResponse(reader("abc"), [](const char*, std::size_t) { throw std::runtime_error("disk full"); }, 3, 3); },
                "write failure ignored");

        check(safeArchivePath("resources/app/package.json") == "resources/app/package.json", "safe path rejected");
        check(safeArchivePath("folder\\file.txt") == "folder/file.txt", "backslash normalization");
        check(safeArchivePath("folder/") == "folder", "directory entry");
        check(safeArchivePath(u8"目录/文件.txt") == u8"目录/文件.txt", "UTF-8 path");
        const std::vector<std::string> badPaths = {"", "../escape", "a/../../escape", "a/./file", "/absolute", "\\rooted", "C:/escape",
            "C:escape", "\\\\server\\share", "a//b", "a///", "file:stream", "CON", "nul.txt", "COM1.txt", "LPT9", "aux.log", "CONOUT$",
            "a./b", "a /b", "foo?", "a|b", "data/settings.json", ".vscode-updater.json", std::string("a\0b", 3), "a\nb",
            std::string(256, 'x'), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), u8"COM¹.txt"};
        for (const auto& s : badPaths) rejects([&] { safeArchivePath(s); }, "unsafe archive filename accepted");
        validateZipAttributes(0100644u << 16, false); ++checks;
        validateZipAttributes(0040755u << 16, true); ++checks;
        rejects([] { validateZipAttributes(0120777u << 16, false); }, "ZIP symlink accepted");
        rejects([] { validateZipAttributes(0020600u << 16, false); }, "ZIP device accepted");
        rejects([] { validateZipAttributes(0x400, false); }, "ZIP reparse point accepted");

        // Fault injection for both rename steps and rollback, with no filesystem dependencies.
        for (int failStep : {0, 1, 2}) {
            std::set<std::string> present{"target", "staged"};
            int step = 0;
            auto rename = [&](const char* from, const char* to) {
                if (++step == failStep) throw std::runtime_error("simulated rename failure");
                check(present.count(from) == 1 && present.count(to) == 0, "invalid transaction transition");
                present.erase(from); present.insert(to);
            };
            if (!failStep) {
                activate(true, rename);
                check(present == std::set<std::string>{"target", "backup"}, "successful switch lost backup");
            } else {
                rejects([&] { activate(true, rename); }, "rename failure ignored");
                check(present.count("target") == 1 && present.count("staged") == 1, "old install not restored");
            }
        }
        std::set<std::string> present{"target", "staged"};
        rejects([&] {
            activate(true, [&](const char* from, const char* to) {
                if (std::string(from) != "target") throw std::runtime_error("activation and rollback failure");
                present.erase(from); present.insert(to);
            });
        }, "failed rollback not reported");
        check(present.count("backup") && present.count("staged"), "failed rollback lost recovery data");
        int moves = 0;
        activate(false, [&](const char* from, const char* to) { ++moves; check(std::string(from) == "staged" && std::string(to) == "target", "fresh install transition"); });
        check(moves == 1, "fresh install moved nonexistent old install");
        std::cout << "PASS: " << checks << " core checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
