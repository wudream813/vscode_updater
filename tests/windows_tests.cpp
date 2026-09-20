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
        check(contents(output / fs::path(L"中文/说明.txt")) == u8"安全测试", "Unicode path extraction");
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
        std::cout << "PASS: " << checks << " Windows integration checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
