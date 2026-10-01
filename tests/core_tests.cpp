#include <map>
#include <fstream>
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
        check(parseOptions({}).stream, "must stream by default");
        check(!parseOptions({"--no-stream"}).stream, "explicit no-stream");
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

        // Central Directory and EOCD parsing tests (embedded standalone ZIP buffer)
        {
            static const std::uint8_t sampleZip[] = {
                0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x40, 0x2d, 0x41, 0x5d, 0x92, 0x0e,
                0x53, 0x62, 0x12, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x74, 0x65,
                0x73, 0x74, 0x31, 0x2e, 0x74, 0x78, 0x74, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x08, 0xcf, 0x2f,
                0xca, 0x49, 0x51, 0x54, 0xf0, 0x18, 0x99, 0x1c, 0x00, 0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00,
                0x00, 0x08, 0x00, 0x40, 0x2d, 0x41, 0x5d, 0xcc, 0xb7, 0x3c, 0xda, 0x15, 0x00, 0x00, 0x00, 0x13,
                0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x73, 0x75, 0x62, 0x2f, 0x74, 0x65, 0x73, 0x74, 0x32,
                0x2e, 0x74, 0x78, 0x74, 0x73, 0x49, 0x4d, 0xce, 0xcf, 0x2d, 0x28, 0x4a, 0x2d, 0x2e, 0xce, 0xcc,
                0xcf, 0x53, 0x28, 0x49, 0x2d, 0x2e, 0x51, 0x04, 0x00, 0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00,
                0x00, 0x08, 0x00, 0x40, 0x2d, 0x41, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x73, 0x75, 0x62, 0x2f, 0x65, 0x6d, 0x70, 0x74, 0x79,
                0x2f, 0x03, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x40,
                0x2d, 0x41, 0x5d, 0x92, 0x0e, 0x53, 0x62, 0x12, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x09,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00,
                0x00, 0x74, 0x65, 0x73, 0x74, 0x31, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03,
                0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x40, 0x2d, 0x41, 0x5d, 0xcc, 0xb7, 0x3c, 0xda, 0x15, 0x00,
                0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x80, 0x01, 0x39, 0x00, 0x00, 0x00, 0x73, 0x75, 0x62, 0x2f, 0x74, 0x65, 0x73, 0x74,
                0x32, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x08,
                0x00, 0x40, 0x2d, 0x41, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xfd, 0x41, 0x79,
                0x00, 0x00, 0x00, 0x73, 0x75, 0x62, 0x2f, 0x65, 0x6d, 0x70, 0x74, 0x79, 0x2f, 0x50, 0x4b, 0x05,
                0x06, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0xaa, 0x00, 0x00, 0x00, 0xa3, 0x00, 0x00,
                0x00, 0x00, 0x00
            };
            const std::size_t zipSize = sizeof(sampleZip);
            auto eocd = findEocd(sampleZip, zipSize, zipSize);
            check(eocd.totalEntries == 3, "sample EOCD entries");
            check(eocd.cdSize == 170, "sample EOCD CD size");
            check(eocd.cdOffset == 163, "sample EOCD CD offset");
            auto plan = parseCentralDirectory(sampleZip + eocd.cdOffset, eocd.cdSize, eocd.totalEntries, zipSize);
            check(plan.entries.size() == 3, "sample CD entries count");
            check(plan.isSequential, "sample CD sequential layout");
            check(plan.expanded == 279, "sample CD expanded bytes");
            check(plan.entries[0].safePath == "test1.txt", "sample entry 0 path");
            check(plan.entries[1].safePath == "sub/test2.txt", "sample entry 1 path");
            check(plan.entries[2].safePath == "sub/empty", "sample entry 2 path");
            check(plan.entries[2].isDirectory, "sample entry 2 is directory");

            // Rejections on corrupt EOCD
            rejects([&] { findEocd(sampleZip, 10, zipSize); }, "truncated tail accepted");
            std::vector<std::uint8_t> badEocd(sampleZip, sampleZip + zipSize);
            for (std::size_t i = zipSize - 22; i < zipSize; ++i) badEocd[i] = 0;
            rejects([&] { findEocd(badEocd.data(), badEocd.size(), badEocd.size()); }, "missing EOCD accepted");

            // Out of bounds CD offset
            std::vector<std::uint8_t> oobData(sampleZip, sampleZip + zipSize);
            const std::size_t eocdPos = zipSize - 22;
            oobData[eocdPos + 16] = 0xff;
            oobData[eocdPos + 17] = 0xff;
            rejects([&] { findEocd(oobData.data(), oobData.size(), zipSize); }, "out of bounds CD accepted");
        }
std::cout << "PASS: " << checks << " core checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
