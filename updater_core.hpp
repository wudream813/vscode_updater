#include <cstring>
#pragma once

// Platform-independent validation shared by the Windows updater and regression tests.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace updater {
constexpr int maxThreads = 16;
constexpr std::uint64_t maxArchiveBytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t maxExpandedBytes = 20ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t maxEntryBytes = 4ULL * 1024 * 1024 * 1024;
constexpr std::uint32_t maxEntries = 200000;

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

inline std::string asciiLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}

inline std::uint64_t number(std::string_view s) {
    require(!s.empty(), "Missing numeric value");
    std::uint64_t n = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    require(r.ec == std::errc{} && r.ptr == s.data() + s.size(), "Invalid integer: " + std::string(s));
    return n;
}

inline bool hexString(const std::string& s, std::size_t length) {
    return s.size() == length && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

struct Options {
    std::string directory = "./Application";
    std::string arch = "x64";
    std::string quality = "stable";
    int threads = 0; // automatic
    bool force = false;
    bool check = false;
    bool keepZip = false;
    bool stream = true; // stream download and decompress on the fly by default
    bool plain = false; // plain text output without modern TUI
    bool pause = false; // scripts never block by default
    bool help = false;
};

inline Options parseOptions(const std::vector<std::string>& args, const std::string& nativeArch = "x64") {
    Options o;
    o.arch = nativeArch;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto value = [&]() {
            require(i + 1 < args.size() && !args[i + 1].empty() && args[i + 1].rfind("--", 0) != 0,
                    "Missing value for " + a);
            return args[++i];
        };
        if (a == "--dir") o.directory = value();
        else if (a == "--arch") o.arch = value();
        else if (a == "--quality") o.quality = value();
        else if (a == "--threads") {
            const auto n = number(value());
            require(n >= 1 && n <= maxThreads, "--threads must be between 1 and 16");
            o.threads = static_cast<int>(n);
        } else if (a == "--force") o.force = true;
        else if (a == "--check") o.check = true;
        else if (a == "--keep-zip") o.keepZip = true;
        else if (a == "--stream") o.stream = true;
        else if (a == "--no-stream") o.stream = false;
        else if (a == "--plain") o.plain = true;
        else if (a == "--pause") o.pause = true;
        else if (a == "--no-pause") o.pause = false;
        else if (a == "--help" || a == "-h") o.help = true;
        else throw std::runtime_error("Unknown argument: " + a);
    }
    require(!o.directory.empty(), "--dir cannot be empty");
    require(o.arch == "x64" || o.arch == "arm64", "--arch must be x64 or arm64");
    require(o.quality == "stable" || o.quality == "insider", "--quality must be stable or insider");
    return o;
}

struct Identity {
    std::string version;
    std::string commit;
    std::string arch;
    std::string quality;
};

inline bool sameBuild(const Identity& a, const Identity& b) {
    return !a.version.empty() && hexString(a.commit, 40) &&
           a.version == b.version && asciiLower(a.commit) == asciiLower(b.commit) &&
           a.arch == b.arch && a.quality == b.quality;
}

struct ContentRange {
    std::uint64_t first, last, total;
};

inline ContentRange parseContentRange(const std::string& s) {
    require(s.rfind("bytes ", 0) == 0, "Invalid Content-Range unit");
    const auto dash = s.find('-', 6), slash = s.find('/', 6);
    require(dash != std::string::npos && slash != std::string::npos && dash < slash,
            "Invalid Content-Range syntax");
    ContentRange r{number(std::string_view(s).substr(6, dash - 6)),
                   number(std::string_view(s).substr(dash + 1, slash - dash - 1)),
                   number(std::string_view(s).substr(slash + 1))};
    require(r.first <= r.last && r.last < r.total, "Invalid Content-Range bounds");
    return r;
}

inline void validateRange(unsigned status, const std::string& header, std::uint64_t first,
                          std::uint64_t length, std::uint64_t total) {
    require(status == 206, "Server did not honor the Range request (expected HTTP 206)");
    require(length > 0 && first < total && length <= total - first, "Invalid requested range");
    const auto r = parseContentRange(header);
    require(r.first == first && r.last == first + length - 1 && r.total == total,
            "Content-Range does not match the requested segment");
}

// A read callback must throw on a transport error and return zero only for clean EOF.
// A failed write must throw. This function never writes past the requested limit.
template<class Read, class Write>
std::uint64_t copyResponse(Read read, Write write, std::uint64_t limit,
                           std::uint64_t expected = std::numeric_limits<std::uint64_t>::max()) {
    char buffer[128 * 1024];
    std::uint64_t received = 0;
    while (true) {
        const std::size_t n = read(buffer, sizeof(buffer));
        require(n <= sizeof(buffer), "Invalid read size");
        if (!n) break;
        require(received <= limit && n <= limit - received, "Response exceeds its allowed size");
        write(buffer, n);
        received += n;
    }
    require(received > 0, "Empty response");
    require(expected == std::numeric_limits<std::uint64_t>::max() || received == expected,
            "Incomplete response");
    return received;
}

inline bool validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char a = static_cast<unsigned char>(s[i++]);
        if (a < 0x80) continue;
        unsigned need = 0;
        std::uint32_t cp = 0, minimum = 0;
        if (a >= 0xc2 && a <= 0xdf) { need = 1; cp = a & 0x1f; minimum = 0x80; }
        else if (a >= 0xe0 && a <= 0xef) { need = 2; cp = a & 0x0f; minimum = 0x800; }
        else if (a >= 0xf0 && a <= 0xf4) { need = 3; cp = a & 7; minimum = 0x10000; }
        else return false;
        if (i + need > s.size()) return false;
        while (need--) {
            const auto b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

// Deliberately enforce Windows path rules on every platform, including test hosts.
inline std::string safeArchivePath(std::string name) {
    require(!name.empty() && name.size() <= 4096 && validUtf8(name), "Invalid ZIP filename");
    std::replace(name.begin(), name.end(), '\\', '/');
    require(name.front() != '/', "Absolute ZIP paths are forbidden");
    if (name.back() == '/') name.pop_back();
    require(!name.empty(), "Empty ZIP path");
    std::size_t begin = 0;
    while (begin < name.size()) {
        const auto end = name.find('/', begin);
        const auto part = name.substr(begin, end == std::string::npos ? end : end - begin);
        require(!part.empty() && part != "." && part != ".." && part.size() <= 255,
                "Unsafe ZIP path component");
        require(part.back() != '.' && part.back() != ' ', "Ambiguous Windows ZIP filename");
        for (unsigned char c : part)
            require(c >= 32 && c != 127 && std::string(":<>\"|?*").find(static_cast<char>(c)) == std::string::npos,
                    "Forbidden character in ZIP path");
        const auto base = asciiLower(part.substr(0, part.find('.')));
        require(base != "con" && base != "prn" && base != "aux" && base != "nul" &&
                base != "clock$" && base != "conin$" && base != "conout$", "Reserved Windows filename");
        if (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) {
            const auto suffix = base.substr(3);
            require(!(suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9') &&
                    suffix != "\xc2\xb9" && suffix != "\xc2\xb2" && suffix != "\xc2\xb3",
                    "Reserved Windows device filename");
        }
        if (end == std::string::npos) break;
        begin = end + 1;
        require(begin < name.size(), "Empty ZIP path component");
    }
    const auto root = asciiLower(name.substr(0, name.find('/')));
    require(root != "data" && root != ".vscode-updater.json", "ZIP entry conflicts with preserved user data or metadata");
    return name;
}

inline void validateZipAttributes(std::uint32_t attributes, bool directory) {
    const auto type = (attributes >> 16) & 0170000;
    require(type == 0 || type == 0100000 || type == 0040000, "ZIP links and special files are forbidden");
    require(type != 0040000 || directory, "Inconsistent ZIP directory attributes");
    require((attributes & 0x400) == 0, "ZIP reparse points are forbidden");
}

// The caller prepares and validates staged files BEFORE entering this operation.
// Keep the backup after success. If activation fails, restore it; if restoration
// also fails, preserve everything and report the manual-recovery requirement.
template<class Rename>
void activate(bool hasOld, Rename rename) {
    if (hasOld) rename("target", "backup");
    try {
        rename("staged", "target");
    } catch (...) {
        if (hasOld) {
            try { rename("backup", "target"); }
            catch (...) { throw std::runtime_error("Activation AND rollback failed; preserve the backup and recover manually"); }
        }
        throw;
    }
}

// ZIP Central Directory structures and streaming pre-validation
struct EocdInfo {
    std::uint16_t diskNumber = 0;
    std::uint16_t cdStartDisk = 0;
    std::uint16_t recordsOnDisk = 0;
    std::uint16_t totalEntries = 0;
    std::uint32_t cdSize = 0;
    std::uint32_t cdOffset = 0;
    std::uint16_t commentLength = 0;
};

inline std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

inline std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0] |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24));
}

inline EocdInfo findEocd(const std::uint8_t* buffer, std::size_t bufferSize, std::uint64_t archiveTotalSize) {
    require(buffer != nullptr && bufferSize >= 22, "Tail buffer too small for ZIP EOCD");
    const std::uint32_t sig = 0x06054b50;
    const std::size_t maxComment = std::min<std::size_t>(65535, bufferSize - 22);
    const std::size_t searchStart = bufferSize - 22;
    const std::size_t searchEnd = bufferSize - 22 - maxComment;

    std::size_t pos = searchStart;
    bool found = false;
    while (true) {
        if (readU32(buffer + pos) == sig) {
            found = true;
            break;
        }
        if (pos == searchEnd) break;
        --pos;
    }
    require(found, "ZIP End of Central Directory (EOCD) signature not found");

    EocdInfo info{};
    info.diskNumber = readU16(buffer + pos + 4);
    info.cdStartDisk = readU16(buffer + pos + 6);
    info.recordsOnDisk = readU16(buffer + pos + 8);
    info.totalEntries = readU16(buffer + pos + 10);
    info.cdSize = readU32(buffer + pos + 12);
    info.cdOffset = readU32(buffer + pos + 16);
    info.commentLength = readU16(buffer + pos + 20);

    require(info.diskNumber == 0 && info.cdStartDisk == 0, "Multi-disk ZIP archives are not supported");
    require(info.recordsOnDisk == info.totalEntries, "Inconsistent ZIP directory record counts");
    require(info.totalEntries > 0 && info.totalEntries <= maxEntries, "Invalid ZIP entry count in EOCD");
    require(pos + 22 + info.commentLength <= bufferSize, "ZIP EOCD comment length overflow");
    require(static_cast<std::uint64_t>(info.cdOffset) + info.cdSize <= archiveTotalSize,
            "ZIP Central Directory extends beyond archive bounds");
    return info;
}

struct PlannedEntry {
    std::size_t index = 0;
    std::string safePath;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compSize = 0;
    std::uint32_t uncompSize = 0;
    std::uint32_t localOffset = 0;
    std::uint32_t externalAttr = 0;
    bool isDirectory = false;
};

struct ZipStreamPlan {
    std::vector<PlannedEntry> entries;
    std::uint64_t expanded = 0;
    bool isSequential = true;
};

inline ZipStreamPlan parseCentralDirectory(const std::uint8_t* cdBuffer, std::size_t cdSize,
                                          std::uint16_t expectedEntries, std::uint64_t archiveTotalSize) {
    require(cdBuffer != nullptr && cdSize >= 46, "Central Directory buffer too small");
    ZipStreamPlan plan;
    plan.entries.reserve(expectedEntries);

    std::size_t pos = 0;
    std::uint32_t lastOffset = 0;
    bool hasLastOffset = false;

    std::vector<std::pair<std::string, bool>> registeredNames;
    registeredNames.reserve(expectedEntries);

    for (std::size_t i = 0; i < expectedEntries; ++i) {
        require(pos + 46 <= cdSize, "Truncated Central Directory entry");
        require(readU32(cdBuffer + pos) == 0x02014b50, "Invalid Central Directory entry signature");

        const std::uint16_t flags = readU16(cdBuffer + pos + 8);
        const std::uint16_t method = readU16(cdBuffer + pos + 10);
        const std::uint32_t crc32 = readU32(cdBuffer + pos + 16);
        const std::uint32_t compSize = readU32(cdBuffer + pos + 20);
        const std::uint32_t uncompSize = readU32(cdBuffer + pos + 24);
        const std::uint16_t fnameLen = readU16(cdBuffer + pos + 28);
        const std::uint16_t extraLen = readU16(cdBuffer + pos + 30);
        const std::uint16_t commentLen = readU16(cdBuffer + pos + 32);
        const std::uint32_t extAttr = readU32(cdBuffer + pos + 38);
        const std::uint32_t localOffset = readU32(cdBuffer + pos + 42);

        require((flags & 1) == 0, "Encrypted ZIP entry");
        require(method == 0 || method == 8, "Unsupported compression method (only Store and Deflate supported)");
        if (method == 0) {
            require(compSize == uncompSize, "Stored ZIP entry compressed size mismatch");
        }

        require(pos + 46 + fnameLen + extraLen + commentLen <= cdSize, "Central Directory entry data overflow");
        std::string rawName(reinterpret_cast<const char*>(cdBuffer + pos + 46), fnameLen);
        const bool isDir = (!rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\')) ||
                           ((extAttr & 0x10) != 0);

        const auto safe = safeArchivePath(rawName);
        validateZipAttributes(extAttr, isDir);

        require(uncompSize <= maxEntryBytes && uncompSize <= maxExpandedBytes - plan.expanded,
                "ZIP expanded size limit exceeded");
        plan.expanded += uncompSize;

        require(static_cast<std::uint64_t>(localOffset) + 30 <= archiveTotalSize,
                "Local file header extends beyond archive bounds");

        if (hasLastOffset && localOffset <= lastOffset) {
            plan.isSequential = false;
        }
        lastOffset = localOffset;
        hasLastOffset = true;

        const auto lowerName = asciiLower(safe);
        for (const auto& existing : registeredNames) {
            require(existing.first != lowerName, "Duplicate/case-colliding ZIP entry: " + safe);
        }
        registeredNames.emplace_back(lowerName, isDir);

        PlannedEntry pe{};
        pe.index = i;
        pe.safePath = safe;
        pe.method = method;
        pe.flags = flags;
        pe.crc32 = crc32;
        pe.compSize = compSize;
        pe.uncompSize = uncompSize;
        pe.localOffset = localOffset;
        pe.externalAttr = extAttr;
        pe.isDirectory = isDir;

        plan.entries.push_back(std::move(pe));
        pos += 46 + fnameLen + extraLen + commentLen;
    }

    for (const auto& item : registeredNames) {
        std::string path = item.first;
        std::size_t slash = path.rfind('/');
        while (slash != std::string::npos) {
            std::string parent = path.substr(0, slash);
            for (const auto& other : registeredNames) {
                if (other.first == parent) {
                    require(other.second, "ZIP file and directory collision: " + parent);
                }
            }
            slash = parent.rfind('/');
        }
    }
    return plan;
}



} // namespace updater
