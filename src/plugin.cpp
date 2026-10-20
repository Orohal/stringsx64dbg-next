#include <Windows.h>
#include <CommCtrl.h>
#include <Commdlg.h>
#include <ShlObj.h>

#include "_plugins.h"
#include "_scriptapi_module.h"
#include "scanner.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:pluginit=_pluginit")
#pragma comment(linker, "/EXPORT:plugstop=_plugstop")
#pragma comment(linker, "/EXPORT:plugsetup=_plugsetup")
#endif

namespace {

constexpr int kMenuSearchStrings = 0;
constexpr int kControlAnsi = 1001;
constexpr int kControlUtf16 = 1002;
constexpr int kControlNullTerminated = 1003;
constexpr int kControlMinimumLength = 1004;
constexpr int kControlSearch = 1005;
constexpr int kControlFilter = 1006;
constexpr int kControlCopy = 1007;
constexpr int kControlSave = 1008;
constexpr int kControlList = 1009;
constexpr int kControlStatus = 1010;
constexpr UINT kScanCompleteMessage = WM_APP + 23;
constexpr std::size_t kReadChunkSize = 1024 * 1024;
constexpr std::size_t kReadOverlap = stringsx64dbg::kMaxStringCharacters * 2 + 2;
constexpr std::size_t kMaximumResults = 1000000;
constexpr wchar_t kWindowClassName[] = L"StringsX64DbgNextWindow";
constexpr UINT kDebugSessionStoppedMessage = WM_APP + 24;

struct ScanCompletion {
    std::vector<stringsx64dbg::StringRecord> records;
    std::wstring error;
    std::size_t unreadableBytes = 0;
    bool cancelled = false;
    bool truncated = false;
};

HINSTANCE g_instance = nullptr;
int g_pluginHandle = 0;
HWND g_hostWindow = nullptr;
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
HWND g_searchButton = nullptr;
std::uintptr_t g_moduleBase = 0;
std::size_t g_moduleSize = 0;
std::vector<stringsx64dbg::StringRecord> g_records;
std::vector<std::size_t> g_visibleRecords;
std::atomic_bool g_cancelScan = false;
std::thread g_scanThread;
std::mutex g_completionMutex;
std::unique_ptr<ScanCompletion> g_completion;

bool refreshMainModuleInfo()
{
    Script::Module::ModuleInfo moduleInfo = {};
    if (!Script::Module::GetMainModuleInfo(&moduleInfo) || !moduleInfo.base || !moduleInfo.size) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_moduleBase = static_cast<std::uintptr_t>(moduleInfo.base);
    g_moduleSize = static_cast<std::size_t>(moduleInfo.size);
    return true;
}

void setStatus(const std::wstring& text)
{
    if (g_status) {
        SetWindowTextW(g_status, text.c_str());
    }
}

bool containsIgnoreCase(const std::wstring& value, const std::wstring& filter)
{
    if (filter.empty()) {
        return true;
    }
    if (filter.size() > value.size()) {
        return false;
    }

    for (std::size_t i = 0; i <= value.size() - filter.size(); i++) {
        if (CompareStringOrdinal(value.data() + i, static_cast<int>(filter.size()), filter.data(), static_cast<int>(filter.size()), TRUE) ==
            CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

std::wstring currentFilter()
{
    const int length = GetWindowTextLengthW(GetDlgItem(g_window, kControlFilter));
    if (length <= 0) {
        return {};
    }

    std::wstring filter(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(GetDlgItem(g_window, kControlFilter), filter.data(), length + 1);
    filter.resize(static_cast<std::size_t>(copied));
    return filter;
}

void refreshVisibleRecords()
{
    const std::wstring filter = currentFilter();
    g_visibleRecords.clear();
    g_visibleRecords.reserve(g_records.size());

    for (std::size_t i = 0; i < g_records.size(); i++) {
        if (containsIgnoreCase(g_records[i].value, filter)) {
            g_visibleRecords.push_back(i);
        }
    }

    if (g_list) {
        ListView_SetItemCountEx(g_list, static_cast<int>(g_visibleRecords.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        InvalidateRect(g_list, nullptr, TRUE);
    }

    if (!g_records.empty() && g_status) {
        wchar_t status[128] = {};
        swprintf_s(status, L"%zu strings (%zu shown)", g_records.size(), g_visibleRecords.size());
        SetWindowTextW(g_status, status);
    }
}

std::wstring addressText(std::uintptr_t address)
{
    wchar_t buffer[2 + sizeof(address) * 2 + 1] = {};
    swprintf_s(buffer, L"0x%llX", static_cast<unsigned long long>(address));
    return buffer;
}

std::wstring itemText(const stringsx64dbg::StringRecord& record, int column)
{
    if (column == 0) {
        return addressText(record.address);
    }
    if (column == 1) {
        return record.encoding == stringsx64dbg::Encoding::Ansi ? L"ANSI" : L"UTF-16LE";
    }
    return record.value;
}

std::wstring selectedValue()
{
    const int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (selected < 0 || static_cast<std::size_t>(selected) >= g_visibleRecords.size()) {
        return {};
    }
    return g_records[g_visibleRecords[static_cast<std::size_t>(selected)]].value;
}

void copySelectedValue()
{
    const std::wstring value = selectedValue();
    if (value.empty()) {
        MessageBoxW(g_window, L"Select a string first.", L"Strings", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!OpenClipboard(g_window)) {
        MessageBoxW(g_window, L"Could not open the clipboard.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    EmptyClipboard();
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        MessageBoxW(g_window, L"Could not allocate clipboard memory.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(g_window, L"Could not lock clipboard memory.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    std::memcpy(destination, value.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        MessageBoxW(g_window, L"Could not set clipboard text.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }
    CloseClipboard();
}

std::string toUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return (converted == required) ? result : std::string();
}

std::wstring csvField(const std::wstring& value)
{
    std::wstring result;
    const bool quote = value.find_first_of(L",\"\r\n") != std::wstring::npos;
    if (quote) {
        result.push_back(L'"');
    }
    for (wchar_t character : value) {
        if (character == L'"') {
            result.push_back(L'"');
        }
        result.push_back(character);
    }
    if (quote) {
        result.push_back(L'"');
    }
    return result;
}

void saveResults()
{
    if (g_records.empty()) {
        MessageBoxW(g_window, L"There are no strings to save.", L"Strings", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t path[MAX_PATH] = L"strings.csv";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    dialog.lpstrDefExt = L"csv";
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            MessageBoxW(g_window, L"Could not open the save dialog.", L"Strings", MB_OK | MB_ICONERROR);
        }
        return;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        MessageBoxW(g_window, L"Could not create the CSV file.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    file.write("\xef\xbb\xbf", 3);
    const std::string header = "Address,Encoding,String\r\n";
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (const auto& record : g_records) {
        const std::wstring row = addressText(record.address) + L"," +
                                 (record.encoding == stringsx64dbg::Encoding::Ansi ? L"ANSI" : L"UTF-16LE") + L"," +
                                 csvField(record.value) + L"\r\n";
        const std::string utf8 = toUtf8(row);
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }

    if (!file) {
        MessageBoxW(g_window, L"An error occurred while writing the CSV file.", L"Strings", MB_OK | MB_ICONERROR);
    }
}

void cancelScan()
{
    g_cancelScan.store(true);
    if (g_scanThread.joinable()) {
        g_scanThread.join();
    }

    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completion.reset();
}

void startScan()
{
    if (!DbgIsDebugging()) {
        MessageBoxW(g_window, L"Start or attach to a debuggee before scanning.", L"Strings", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!refreshMainModuleInfo()) {
        MessageBoxW(g_window, L"Could not retrieve the main module information from x64dbg.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    const bool scanAnsi = SendDlgItemMessageW(g_window, kControlAnsi, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool scanUtf16Le = SendDlgItemMessageW(g_window, kControlUtf16, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool requireNull = SendDlgItemMessageW(g_window, kControlNullTerminated, BM_GETCHECK, 0, 0) == BST_CHECKED;
    BOOL validLength = FALSE;
    const UINT minimumLength = GetDlgItemInt(g_window, kControlMinimumLength, &validLength, FALSE);

    if (!scanAnsi && !scanUtf16Le) {
        MessageBoxW(g_window, L"Select at least one string encoding.", L"Strings", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!validLength || minimumLength == 0 || minimumLength > stringsx64dbg::kMaxStringCharacters) {
        MessageBoxW(g_window, L"Minimum length must be between 1 and 4096.", L"Strings", MB_OK | MB_ICONWARNING);
        return;
    }

    std::uintptr_t base = 0;
    std::size_t size = 0;
    {
        std::lock_guard<std::mutex> lock(g_completionMutex);
        base = g_moduleBase;
        size = g_moduleSize;
    }
    if (!base || !size || base > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        MessageBoxW(g_window, L"The main module image size is unavailable.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    HANDLE process = DbgGetProcessHandle();
    if (!process) {
        MessageBoxW(g_window, L"Could not get the debuggee process handle.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    cancelScan();
    g_cancelScan.store(false);
    g_records.clear();
    g_visibleRecords.clear();
    ListView_SetItemCountEx(g_list, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    setStatus(L"Scanning main module...");
    EnableWindow(g_searchButton, FALSE);

    stringsx64dbg::ScanOptions options;
    options.scanAnsi = scanAnsi;
    options.scanUtf16Le = scanUtf16Le;
    options.requireNullTerminator = requireNull;
    options.minimumLength = minimumLength;

    const HWND targetWindow = g_window;
    try {
        g_scanThread = std::thread([targetWindow, process, base, size, options]() {
            auto completion = std::make_unique<ScanCompletion>();
            try {
                const std::size_t overlap = options.scanUtf16Le ? kReadOverlap : stringsx64dbg::kMaxStringCharacters + 1;
                for (std::size_t offset = 0; offset < size && !g_cancelScan.load(); offset += kReadChunkSize) {
                    const std::size_t coreSize = (std::min)(kReadChunkSize, size - offset);
                    const std::size_t readSize = (std::min)(size - offset, coreSize + overlap);
                    std::vector<std::uint8_t> buffer(readSize, 0);
                    SIZE_T bytesRead = 0;
                    ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), buffer.data(), readSize, &bytesRead);
                    if (bytesRead < readSize) {
                        completion->unreadableBytes += readSize - bytesRead;
                    }
                    if (!bytesRead) {
                        continue;
                    }

                    const auto chunkRecords = stringsx64dbg::scanBytes(buffer.data(), bytesRead, base + offset, options);
                    const std::uintptr_t coreEnd = base + offset + coreSize;
                    for (const auto& record : chunkRecords) {
                        if (record.address < coreEnd) {
                            if (completion->records.size() == kMaximumResults) {
                                completion->truncated = true;
                                break;
                            }
                            completion->records.push_back(record);
                        }
                    }
                    if (completion->truncated) {
                        break;
                    }
                }
            } catch (const std::exception& error) {
                const std::string message = error.what();
                completion->error.assign(message.begin(), message.end());
            } catch (...) {
                completion->error = L"Unexpected error while scanning the debuggee.";
            }

            completion->cancelled = g_cancelScan.load();
            if (completion->cancelled) {
                return;
            }
            std::sort(completion->records.begin(), completion->records.end(), [](const auto& left, const auto& right) {
                if (left.address != right.address) {
                    return left.address < right.address;
                }
                return left.encoding < right.encoding;
            });
            {
                std::lock_guard<std::mutex> lock(g_completionMutex);
                g_completion = std::move(completion);
            }
            PostMessageW(targetWindow, kScanCompleteMessage, 0, 0);
        });
    } catch (const std::system_error&) {
        EnableWindow(g_searchButton, TRUE);
        setStatus(L"Could not start the scan worker thread.");
    }
}

void showScanCompletion()
{
    std::unique_ptr<ScanCompletion> completion;
    {
        std::lock_guard<std::mutex> lock(g_completionMutex);
        completion = std::move(g_completion);
    }

    EnableWindow(g_searchButton, TRUE);
    if (!completion) {
        return;
    }
    if (!completion->error.empty()) {
        setStatus(completion->error);
        return;
    }

    g_records = std::move(completion->records);
    refreshVisibleRecords();

    std::wstring status = std::to_wstring(g_records.size()) + L" strings found";
    if (completion->truncated) {
        status += L" (result limit reached)";
    }
    if (completion->unreadableBytes) {
        status += L"; " + std::to_wstring(completion->unreadableBytes) + L" bytes unreadable";
    }
    setStatus(status);
}

void activateSelectedRecord(int row)
{
    if (row < 0 || static_cast<std::size_t>(row) >= g_visibleRecords.size()) {
        return;
    }

    const auto& record = g_records[g_visibleRecords[static_cast<std::size_t>(row)]];
    GuiDumpAt(static_cast<duint>(record.address));
}

void layoutControls(HWND window)
{
    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int secondRowY = 40;
    const int bottomHeight = 26;

    MoveWindow(GetDlgItem(window, kControlAnsi), 12, 8, 82, 24, TRUE);
    MoveWindow(GetDlgItem(window, kControlUtf16), 100, 8, 106, 24, TRUE);
    MoveWindow(GetDlgItem(window, kControlNullTerminated), 210, 8, 145, 24, TRUE);
    MoveWindow(GetDlgItem(window, 1011), 365, 8, 102, 24, TRUE);
    MoveWindow(GetDlgItem(window, kControlMinimumLength), 469, 7, 52, 25, TRUE);
    MoveWindow(GetDlgItem(window, kControlSearch), 530, 6, 100, 27, TRUE);
    MoveWindow(GetDlgItem(window, 1012), 12, secondRowY + 4, 48, 22, TRUE);
    MoveWindow(GetDlgItem(window, kControlFilter), 62, secondRowY, (std::max)(150, width - 370), 26, TRUE);
    MoveWindow(GetDlgItem(window, kControlCopy), width - 292, secondRowY, 88, 26, TRUE);
    MoveWindow(GetDlgItem(window, kControlSave), width - 196, secondRowY, 184, 26, TRUE);
    MoveWindow(g_list, 8, secondRowY + 32, (std::max)(0, width - 16), (std::max)(0, height - secondRowY - bottomHeight - 40), TRUE);
    MoveWindow(g_status, 12, height - bottomHeight, (std::max)(0, width - 24), 20, TRUE);
}

HWND createControl(DWORD style, const wchar_t* className, const wchar_t* text, int id, HWND parent)
{
    return CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        case WM_CREATE: {
            createControl(BS_AUTOCHECKBOX, L"BUTTON", L"ANSI", kControlAnsi, window);
            createControl(BS_AUTOCHECKBOX, L"BUTTON", L"UTF-16LE", kControlUtf16, window);
            createControl(BS_AUTOCHECKBOX, L"BUTTON", L"Null terminated", kControlNullTerminated, window);
            createControl(SS_LEFT, L"STATIC", L"Min length:", 1011, window);
            createControl(ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER, L"EDIT", L"5", kControlMinimumLength, window);
            g_searchButton = createControl(BS_PUSHBUTTON, L"BUTTON", L"Search", kControlSearch, window);
            createControl(SS_LEFT, L"STATIC", L"Filter:", 1012, window);
            createControl(ES_AUTOHSCROLL | WS_BORDER, L"EDIT", L"", kControlFilter, window);
            createControl(BS_PUSHBUTTON, L"BUTTON", L"Copy selected", kControlCopy, window);
            createControl(BS_PUSHBUTTON, L"BUTTON", L"Save CSV...", kControlSave, window);
            g_status = createControl(SS_LEFT | SS_CENTERIMAGE, L"STATIC", L"Start or attach to a debuggee, then search the main module.",
                                     kControlStatus, window);
            g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 0, 0, 0, 0, window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlList)), g_instance, nullptr);
            if (!g_list) {
                return -1;
            }
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.cx = 150;
            column.pszText = const_cast<wchar_t*>(L"Address");
            ListView_InsertColumn(g_list, 0, &column);
            column.cx = 100;
            column.pszText = const_cast<wchar_t*>(L"Encoding");
            ListView_InsertColumn(g_list, 1, &column);
            column.cx = 600;
            column.pszText = const_cast<wchar_t*>(L"String");
            ListView_InsertColumn(g_list, 2, &column);

            SendDlgItemMessageW(window, kControlAnsi, BM_SETCHECK, BST_CHECKED, 0);
            SendDlgItemMessageW(window, kControlUtf16, BM_SETCHECK, BST_CHECKED, 0);
            layoutControls(window);
            return 0;
        }
        case WM_SIZE:
            if (g_list && g_status) {
                layoutControls(window);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kControlSearch:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        startScan();
                    }
                    return 0;
                case kControlFilter:
                    if (HIWORD(wParam) == EN_CHANGE) {
                        refreshVisibleRecords();
                    }
                    return 0;
                case kControlCopy:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        copySelectedValue();
                    }
                    return 0;
                case kControlSave:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        saveResults();
                    }
                    return 0;
            }
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (!header || header->hwndFrom != g_list) {
                break;
            }
            if (header->code == LVN_GETDISPINFOW) {
                auto* displayInfo = reinterpret_cast<NMLVDISPINFOW*>(lParam);
                if ((displayInfo->item.mask & LVIF_TEXT) && displayInfo->item.pszText && displayInfo->item.cchTextMax > 0 &&
                    displayInfo->item.iItem >= 0 && static_cast<std::size_t>(displayInfo->item.iItem) < g_visibleRecords.size()) {
                    const auto& record = g_records[g_visibleRecords[static_cast<std::size_t>(displayInfo->item.iItem)]];
                    const std::wstring text = itemText(record, displayInfo->item.iSubItem);
                    wcsncpy_s(displayInfo->item.pszText, static_cast<std::size_t>(displayInfo->item.cchTextMax), text.c_str(), _TRUNCATE);
                }
                return 0;
            }
            if (header->code == NM_DBLCLK) {
                const auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                activateSelectedRecord(activate->iItem);
                return 0;
            }
            break;
        }
        case kScanCompleteMessage:
            showScanCompletion();
            return 0;
        case kDebugSessionStoppedMessage:
            g_records.clear();
            g_visibleRecords.clear();
            ListView_SetItemCountEx(g_list, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
            EnableWindow(g_searchButton, TRUE);
            setStatus(L"Debug session stopped.");
            return 0;
        case WM_CLOSE:
            cancelScan();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            cancelScan();
            g_window = nullptr;
            g_list = nullptr;
            g_status = nullptr;
            g_searchButton = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool createWindowClass()
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void showWindow()
{
    if (!createWindowClass()) {
        MessageBoxW(g_hostWindow, L"Could not register the Strings window class.", L"Strings", MB_OK | MB_ICONERROR);
        return;
    }

    if (!g_window) {
        g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClassName, L"Strings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, g_hostWindow, nullptr, g_instance, nullptr);
        if (!g_window) {
            MessageBoxW(g_hostWindow, L"Could not create the Strings window.", L"Strings", MB_OK | MB_ICONERROR);
            return;
        }
    }

    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
}

void handlePluginCallback(CBTYPE callbackType, void* callbackInfo)
{
    if (callbackType == CB_CREATEPROCESS) {
        const auto* info = static_cast<const PLUG_CB_CREATEPROCESS*>(callbackInfo);
        if (!info || !info->CreateProcessInfo || !info->modInfo) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_completionMutex);
        g_moduleBase = reinterpret_cast<std::uintptr_t>(info->CreateProcessInfo->lpBaseOfImage);
        g_moduleSize = info->modInfo->ImageSize;
        return;
    }

    if (callbackType == CB_STOPDEBUG) {
        cancelScan();
        {
            std::lock_guard<std::mutex> lock(g_completionMutex);
            g_moduleBase = 0;
            g_moduleSize = 0;
        }
        if (g_window) {
            PostMessageW(g_window, kDebugSessionStoppedMessage, 0, 0);
        }
        return;
    }

    if (callbackType == CB_MENUENTRY) {
        const auto* info = static_cast<const PLUG_CB_MENUENTRY*>(callbackInfo);
        if (info && info->hEntry == kMenuSearchStrings) {
            showWindow();
        }
    }
}

}

extern "C" __declspec(dllexport) bool pluginit(PLUG_INITSTRUCT* initStruct)
{
    if (!initStruct) {
        return false;
    }

    initStruct->sdkVersion = PLUG_SDKVERSION;
    initStruct->pluginVersion = 1;
    strcpy_s(initStruct->pluginName, "Strings");
    g_pluginHandle = initStruct->pluginHandle;

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        return false;
    }

    _plugin_registercallback(g_pluginHandle, CB_CREATEPROCESS, handlePluginCallback);
    _plugin_registercallback(g_pluginHandle, CB_STOPDEBUG, handlePluginCallback);
    _plugin_registercallback(g_pluginHandle, CB_MENUENTRY, handlePluginCallback);
    return true;
}

extern "C" __declspec(dllexport) bool plugstop()
{
    _plugin_unregistercallback(g_pluginHandle, CB_CREATEPROCESS);
    _plugin_unregistercallback(g_pluginHandle, CB_STOPDEBUG);
    _plugin_unregistercallback(g_pluginHandle, CB_MENUENTRY);
    cancelScan();
    if (g_window) {
        DestroyWindow(g_window);
    }
    return true;
}

extern "C" __declspec(dllexport) void plugsetup(PLUG_SETUPSTRUCT* setupStruct)
{
    if (!setupStruct) {
        return;
    }
    g_hostWindow = setupStruct->hwndDlg;
    if (!_plugin_menuaddentry(setupStruct->hMenu, kMenuSearchStrings, "&Strings")) {
        _plugin_logputs("Strings: could not add the Plugins menu entry.");
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
