#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stringsx64dbg {

enum class Encoding {
    Ansi,
    Utf16Le
};

struct StringRecord {
    std::uintptr_t address;
    Encoding encoding;
    std::wstring value;
};

struct ScanOptions {
    bool scanAnsi = true;
    bool scanUtf16Le = true;
    bool requireNullTerminator = false;
    std::size_t minimumLength = 5;
};

constexpr std::size_t kMaxStringCharacters = 4096;

std::vector<StringRecord> scanBytes(const std::uint8_t* data, std::size_t size, std::uintptr_t baseAddress,
                                    const ScanOptions& options);

}
