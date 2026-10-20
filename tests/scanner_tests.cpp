#include "scanner.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using stringsx64dbg::Encoding;
using stringsx64dbg::ScanOptions;
using stringsx64dbg::StringRecord;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

const StringRecord* findRecord(const std::vector<StringRecord>& records, std::uintptr_t address, Encoding encoding)
{
    for (const auto& record : records) {
        if (record.address == address && record.encoding == encoding) {
            return &record;
        }
    }
    return nullptr;
}

void testAnsiAndAddresses()
{
    const std::vector<std::uint8_t> bytes = {0x00, 'H', 'e', 'l', 'l', 'o', 0x00, 'x', 'y'};
    ScanOptions options;
    options.scanUtf16Le = false;

    const auto records = stringsx64dbg::scanBytes(bytes.data(), bytes.size(), 0x1000, options);
    const StringRecord* hello = findRecord(records, 0x1001, Encoding::Ansi);
    require(hello && hello->value == L"Hello", "ANSI string or address was not detected");
}

void testUtf16Le()
{
    const std::vector<std::uint8_t> bytes = {0x01, 0x00, 'W', 0, 'o', 0, 'r', 0, 'l', 0, 'd', 0, 0, 0};
    ScanOptions options;
    options.scanAnsi = false;
    options.minimumLength = 5;

    const auto records = stringsx64dbg::scanBytes(bytes.data(), bytes.size(), 0x2000, options);
    const StringRecord* world = findRecord(records, 0x2002, Encoding::Utf16Le);
    require(world && world->value == L"World", "UTF-16LE string or address was not detected");
}

void testNullTerminationAndMinimumLength()
{
    const std::vector<std::uint8_t> bytes = {'s', 'h', 'o', 'r', 't', 0, 'u', 'n', 't', 'e', 'r', 'm'};
    ScanOptions options;
    options.scanUtf16Le = false;
    options.requireNullTerminator = true;

    auto records = stringsx64dbg::scanBytes(bytes.data(), bytes.size(), 0x3000, options);
    require(records.size() == 1 && records.front().value == L"short", "Null-termination filtering failed");

    options.requireNullTerminator = false;
    options.minimumLength = 6;
    records = stringsx64dbg::scanBytes(bytes.data(), bytes.size(), 0x3000, options);
    require(records.size() == 1 && records.front().value == L"unterm", "Minimum string length was not applied");
}

void testDisabledEncodingsAndEmptyInput()
{
    const std::vector<std::uint8_t> bytes = {'H', 'e', 'l', 'l', 'o', 0};
    ScanOptions options;
    options.scanAnsi = false;
    options.scanUtf16Le = false;
    require(stringsx64dbg::scanBytes(bytes.data(), bytes.size(), 0, options).empty(), "Disabled encodings produced results");
    require(stringsx64dbg::scanBytes(nullptr, 0, 0, options).empty(), "Empty input produced results");
}

}

int main()
{
    testAnsiAndAddresses();
    testUtf16Le();
    testNullTerminationAndMinimumLength();
    testDisabledEncodingsAndEmptyInput();
    return 0;
}
