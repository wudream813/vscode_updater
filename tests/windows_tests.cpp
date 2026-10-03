// Include the real implementation so tests exercise the exact SHA/ZIP/path/data
// code shipped in the executable, rather than a second test-only implementation.
#define main updater_program_main
#include "../update.cpp"
#undef main

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
std::string contents(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv) {
    Console console;
    interactiveOutput = false;
    try {
        require(argc == 2, "Usage: windows_tests.exe <fixture directory>");
        const auto fixtures = fs::absolute(fs::path(argv[1]));
        Workspace work(fixtures / "test-install");
        const auto hashFile = work.root / "hash.txt";
        { std::ofstream f(hashFile, std::ios::binary); f << "abc"; }
        check(sha256(hashFile) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
        const auto valid = inspectZip(fixtures / "valid.zip");
        check(valid.entries.size() == 3, "ZIP entry count");
        const auto output = work.root / fs::path(L"Unicode-目录");
        fs::create_directory(output);
        extractZip(fixtures / "valid.zip", output, valid, 2);
        check(contents(output / "folder/hello.txt") == "hello", "ZIP contents");
        check(fs::file_size(output / "empty.txt") == 0, "empty file extraction");
        check(contents(output / fs::path(L"中文/说明.txt")) == reinterpret_cast<const char*>(u8"安全测试"), "Unicode path extraction");
        for (const auto* name : {"traversal.zip", "absolute.zip", "case-collision.zip", "conflict.zip", "data.zip", "reserved.zip", "symlink.zip", "truncated.zip"})
            rejects([&] { inspectZip(fixtures / name); }, "unsafe ZIP accepted");
        check(!fs::exists(work.root / "escaped.txt") && !fs::exists(fixtures / "escaped.txt"), "ZIP traversal escaped");
        const auto corruptPlan = inspectZip(fixtures / "corrupt.zip");
        const auto corruptOut = work.root / "corrupt-output";
        fs::create_directory(corruptOut);
        rejects([&] { extractZip(fixtures / "corrupt.zip", corruptOut, corruptPlan, 2); }, "CRC error ignored");

        const auto oldData = work.root / "old-data", newData = work.root / "new-data";
        fs::create_directories(oldData / "user-data");
        fs::create_directories(oldData / "extensions");
        { std::ofstream f(oldData / "user-data/settings.json"); f << "{\"preserve\":true}"; }
        const auto expected = contents(oldData / "user-data/settings.json");
        check(dataSize(oldData) == expected.size(), "data size calculation");
        copyData(oldData, newData);
        check(contents(newData / "user-data/settings.json") == expected, "portable settings not copied");
        check(contents(oldData / "user-data/settings.json") == expected, "old data was modified");
        check(fs::is_directory(newData / "extensions"), "empty extension directory not preserved");

        rejects([&] { validateTarget(pathText(fs::current_path())); }, "working directory accepted as target");
        rejects([&] { validateTarget(pathText(fixtures.root_path())); }, "drive root accepted as target");
        rejects([&] { validateTarget(pathText(oldData)); }, "unrelated non-empty directory accepted");
        check(validateTarget(pathText(work.root / "new-install")) == work.root / "new-install", "fresh target rejected");
        check(!pathWithin(fs::path(L"C:/app-other/file"), fs::path(L"C:/app")), "prefix confusion");
        check(pathWithin(fs::path(L"C:/APP/file"), fs::path(L"c:/app")), "Windows case insensitive containment");
        check(executableArch(executablePath()) == "x64", "PE architecture detection");

        // Real filesystem rollback with a deliberately missing stage directory.
        const auto target = work.root / "target", backup = work.root / "backup", missing = work.root / "missing";
        fs::create_directory(target);
        { std::ofstream f(target / "original.txt"); f << "old version"; }
        std::map<std::string, fs::path> locations{{"target", target}, {"backup", backup}, {"staged", missing}};
        rejects([&] { updater::activate(true, [&](const char* from, const char* to) { fs::rename(locations.at(from), locations.at(to)); }); },
                "activation failure not reported");
        check(contents(target / "original.txt") == "old version" && !fs::exists(backup), "real filesystem rollback failed");

        // Callback cannot overrun an entry or silently accept an out-of-order write.
        std::atomic<bool> cancel{false};
        ExtractOutput sink{std::ofstream(work.root / "callback.txt", std::ios::binary), 0, 2, false, &cancel};
        check(extractWrite(&sink, 0, "abc", 3) == 0 && sink.failed, "ZIP entry write crossed size boundary");

        // Test streaming unpacking in Windows tests using offline fixture stream.zip
        {
            std::ifstream fstream(fixtures / "stream.zip", std::ios::binary);
            check(fstream.good(), "stream.zip fixture missing");
            std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(fstream)), std::istreambuf_iterator<char>());
            auto eocd = updater::findEocd(data.data(), data.size(), data.size());
            const auto streamPlan = updater::parseCentralDirectory(data.data() + eocd.cdOffset, eocd.cdSize, eocd.totalEntries, data.size());
            check(streamPlan.isSequential, "streamPlan sequential");
            const auto streamOut = work.root / "stream-staged";
            fs::create_directories(streamOut);

            ZipStreamUnpacker unpacker(streamPlan);
            std::ofstream currentOut;
            fs::path curPath;

            auto onDir = [&](const std::string& d) {
                fs::create_directories(streamOut / fs::path(wide(d)));
            };
            auto onStart = [&](const updater::PlannedEntry& e) {
                curPath = streamOut / fs::path(wide(e.safePath));
                fs::create_directories(curPath.parent_path());
                currentOut.open(curPath, std::ios::binary | std::ios::trunc);
                check(currentOut.good(), "stream test file open");
            };
            auto onData = [&](const std::uint8_t* p, std::size_t n) {
                currentOut.write(reinterpret_cast<const char*>(p), n);
                check(currentOut.good(), "stream test file write");
            };
            auto onEnd = [&](const updater::PlannedEntry&) {
                if (currentOut.is_open()) finishFile(currentOut);
            };

            std::size_t pos = 0;
            while (pos < eocd.cdOffset) {
                std::size_t chunk = std::min<std::size_t>(11, eocd.cdOffset - pos);
                unpacker.feed(pos, data.data() + pos, chunk, onDir, onStart, onData, onEnd);
                pos += chunk;
            }
            check(unpacker.isComplete(), "stream unpack complete in Windows integration");
            check(contents(streamOut / "test1.txt").size() == 260, "stream test1 size");
            check(contents(streamOut / "sub/test2.txt") == "Decompression test!", "stream test2 content");
        }

        // Test locateProductJson and versioned update layout (win32VersionedUpdate)
        {
            const auto versionedDir = work.root / "versioned-app";
            fs::create_directories(versionedDir / "07f806f999/resources/app");
            {
                std::ofstream f(versionedDir / "07f806f999/resources/app/product.json");
                f << "{\"version\":\"1.140.0\",\"commit\":\"07f806f999227108933c2e30515b26eecc1fda74\",\"quality\":\"stable\"}";
            }
            {
                std::ofstream f(versionedDir / "07f806f999/resources/app/package.json");
                f << "{\"version\":\"1.140.0\",\"name\":\"Code\"}";
            }
            // Copy executable from current test binary to act as Code.exe
            fs::copy_file(executablePath(), versionedDir / "Code.exe");
            check(looksLikeVSCode(versionedDir), "versioned VS Code directory recognized");
            const auto id = localIdentity(versionedDir);
            check(id.version == "1.140.0", "versioned identity version");
            check(id.commit == "07f806f999227108933c2e30515b26eecc1fda74", "versioned identity commit");
            check(id.quality == "stable", "versioned identity quality");
            check(id.arch == "x64", "versioned identity arch");
        }
std::cout << "PASS: " << checks << " Windows integration checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
