#include "scanner.h"

#include <Windows.h>

#include <algorithm>
#include <limits>

namespace stringsx64dbg {
namespace {

bool isPrintableAnsi(std::uint8_t value)
{
    return (value >= 0x20 && value <= 0x7e) || value >= 0xa0;
}

bool isPrintableUtf16(std::uint16_t value)
{
    return value >= 0x20 && !(value >= 0x7f && value <= 0x9f) && value != 0xfffe && value != 0xffff &&
           !(value >= 0xd800 && value <= 0xdfff);
}

std::wstring decodeAnsi(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    if (converted != required) {
        return std::wstring(value.begin(), value.end());
    }
    return result;
}

void scanAnsiStrings(const std::uint8_t* data, std::size_t size, std::uintptr_t baseAddress, const ScanOptions& options,
                     std::vector<StringRecord>* results)
{
    std::string run;
    std::size_t runLength = 0;
    std::size_t runStart = 0;

    const auto flush = [&](bool terminated) {
        if (runLength >= options.minimumLength && (!options.requireNullTerminator || terminated)) {
            results->push_back({baseAddress + runStart, Encoding::Ansi, decodeAnsi(run)});
        }
        run.clear();
        runLength = 0;
    };

    for (std::size_t i = 0; i < size; i++) {
        const std::uint8_t value = data[i];
        if (isPrintableAnsi(value)) {
            if (runLength == 0) {
                runStart = i;
            }
            if (run.size() < kMaxStringCharacters) {
                run.push_back(static_cast<char>(value));
            }
            runLength++;
        } else {
            flush(value == 0);
        }
    }
    flush(false);
}

void scanUtf16Strings(const std::uint8_t* data, std::size_t size, std::uintptr_t baseAddress, const ScanOptions& options,
                      std::vector<StringRecord>* results)
{
    std::wstring run;
    std::size_t runLength = 0;
    std::size_t runStart = 0;

    const auto flush = [&](bool terminated) {
        if (runLength >= options.minimumLength && (!options.requireNullTerminator || terminated)) {
            results->push_back({baseAddress + runStart, Encoding::Utf16Le, run});
        }
        run.clear();
        runLength = 0;
    };

    for (std::size_t i = 0; i + 1 < size;) {
        const std::uint16_t value = static_cast<std::uint16_t>(data[i]) | (static_cast<std::uint16_t>(data[i + 1]) << 8);
        if (value >= 0xd800 && value <= 0xdbff && i + 3 < size) {
            const std::uint16_t low = static_cast<std::uint16_t>(data[i + 2]) | (static_cast<std::uint16_t>(data[i + 3]) << 8);
            if (low >= 0xdc00 && low <= 0xdfff) {
                if (runLength == 0) {
                    runStart = i;
                }
                if (run.size() + 2 <= kMaxStringCharacters) {
                    run.push_back(static_cast<wchar_t>(value));
                    run.push_back(static_cast<wchar_t>(low));
                }
                runLength++;
                i += 4;
                continue;
            }
        }

        if (isPrintableUtf16(value)) {
            if (runLength == 0) {
                runStart = i;
            }
            if (run.size() < kMaxStringCharacters) {
                run.push_back(static_cast<wchar_t>(value));
            }
            runLength++;
        } else {
            flush(value == 0);
        }
        i += 2;
    }

    flush(false);
}

}

std::vector<StringRecord> scanBytes(const std::uint8_t* data, std::size_t size, std::uintptr_t baseAddress, const ScanOptions& options)
{
    std::vector<StringRecord> results;
    if (!data || !size || !options.minimumLength || baseAddress > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return results;
    }

    if (options.scanAnsi) {
        scanAnsiStrings(data, size, baseAddress, options, &results);
    }
    if (options.scanUtf16Le) {
        scanUtf16Strings(data, size, baseAddress, options, &results);
    }

    return results;
}

}
