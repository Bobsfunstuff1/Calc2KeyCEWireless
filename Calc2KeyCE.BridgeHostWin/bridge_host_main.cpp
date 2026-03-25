#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <sstream>
#include <vector>

#include "compression.h"
#include "bridge_protocol.h"
#include "leptonica/allheaders.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <d3d11.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
constexpr uint16_t kDefaultPort = 28400;
constexpr int kCalcWidth = 320;
constexpr int kCalcHeight = 240;
constexpr int kBitsPerPixel = 32;
constexpr int kPaletteBytes = 512;
constexpr int kFrameBytes = kPaletteBytes + (kCalcWidth * kCalcHeight);
constexpr std::size_t kBridgeChunkBytes = 16384;
constexpr int kTargetFrameDelayMs = 16;
constexpr int kMouseTickMs = 8;
constexpr int kKeyRepeatInitialDelayMs = 400;
constexpr int kKeyRepeatIntervalMs = 33;
constexpr int kHeaderBytes = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint32_t);
constexpr uint8_t kMouseMoveFlag = 0x01;
constexpr double kColorMult = 31.0 / 255.0;
constexpr wchar_t kVirtualDisplayInstanceId[] = L"ROOT\\DISPLAY\\0000";

std::atomic<bool> gStop(false);
std::mutex gSendMutex;
std::mutex gBindingsMutex;
std::mutex gKeyRepeatMutex;
uint32_t gKeyBindings[128] = { 0 };
uint8_t gPrevCalcData[7] = { 0 };
std::atomic<uint8_t> gMoveState(0);
int gMouseSpeed = 10;
uint32_t gFrameId = 1;
std::atomic<bool> gSessionStop(false);
std::atomic<int> gBridgeState(0);
std::atomic<float> gLatestFrameMs(0.0f);
std::array<float, 64> gFrameTimes{};
int gFrameTimesIndex = 0;
std::mutex gLogMutex;
std::mutex gRelayUiMutex;
std::string gRelayStatusLog = "Ready.\n";
std::atomic<bool> gRelayActionRunning(false);
bool gRelayAutoScroll = true;
bool gAutoManageVirtualDisplay = true;
bool gManagedVirtualDisplayThisRun = false;
bool gMakeVirtualPrimaryWhileRunning = true;
bool gChangedPrimaryDisplayThisRun = false;
bool gPrimaryRestoreInProgress = false;
std::wstring gOriginalPrimaryDisplayDevice;
std::wstring gVirtualPrimaryDisplayDevice;
std::wstring gPreferredHostWindowDisplayDevice;
HWND gMainWindow = nullptr;
std::array<char, 128> gPiHost{};
std::array<char, 64> gPiUser{};
std::array<char, MAX_PATH> gSshKeyPath = {};
std::array<char, 128> gBridgeHostIp{};
std::array<char, 256> gPiRelayPath{};
std::array<char, 256> gPiRelayLogPath{};
std::array<char, MAX_PATH> gVddControlPath{};
std::array<char, MAX_PATH> gVddSettingsPath{};
std::array<char, MAX_PATH> gVddDriverInfPath{};
int gVddMonitorCount = 1;
std::array<char, 128> gVddGpuFriendlyName{};
std::array<char, 256> gVddRefreshRatesCsv{};
std::array<char, 2048> gVddResolutionsText{};
bool gVddSendLogsThroughPipe = true;
bool gVddFileLogging = false;
bool gVddDebugLogging = false;
bool gVddSdr10Bit = false;
bool gVddHdrPlus = false;
int gVddColourFormatIndex = 0;
bool gVddHardwareCursor = true;
int gVddCursorMaxX = 128;
int gVddCursorMaxY = 128;
bool gVddAlphaCursorSupport = true;
int gVddXorCursorSupportLevel = 2;
bool gVddCustomEdid = false;
bool gVddPreventSpoof = false;
bool gVddEdidCeaOverride = false;
bool gVddEdidIntegrationEnabled = false;
bool gVddEdidAutoConfigure = false;
std::array<char, 256> gVddEdidProfilePath{};
bool gVddEdidOverrideManual = false;
bool gVddEdidFallbackOnError = true;

bool gDone = false;
bool gBindingKeyState = false;
uint8_t gBindingKey = static_cast<uint8_t>(-1);
uint32_t gNewBinding = 0;
uint8_t gLastCalcKeyUp = static_cast<uint8_t>(-1);

ID3D11Device* gPd3dDevice = nullptr;
ID3D11DeviceContext* gPd3dDeviceContext = nullptr;
IDXGISwapChain* gPSwapChain = nullptr;
ID3D11RenderTargetView* gMainRenderTargetView = nullptr;
UINT gResizeWidth = 0;
UINT gResizeHeight = 0;

bool loadVddMonitorCount();
bool loadVddSettingsUi();

struct KeyRepeatState {
    bool active = false;
    WORD vk = 0;
    std::chrono::steady_clock::time_point nextRepeat{};
};

std::array<KeyRepeatState, 128> gKeyRepeatStates{};

void appendLog(const std::string& message) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::ofstream log("bridge_host.log", std::ios::app);
    if (!log) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    log << '['
        << st.wHour << ':'
        << st.wMinute << ':'
        << st.wSecond << '.'
        << st.wMilliseconds
        << "] " << message << '\n';
}

void appendRelayStatus(const std::string& message) {
    std::lock_guard<std::mutex> lock(gRelayUiMutex);
    gRelayStatusLog += message;
    if (gRelayStatusLog.empty() || gRelayStatusLog.back() != '\n') {
        gRelayStatusLog.push_back('\n');
    }
}

std::string trimCopy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string winQuote(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    out += '"';
    return out;
}

std::string shellSingleQuote(const std::string& value) {
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            out += "'\"'\"'";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string narrowUtf16(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) {
        return {};
    }

    std::string out(static_cast<std::size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), sizeNeeded, nullptr, nullptr);
    return out;
}

bool looksLikeVirtualDisplayDevice(const wchar_t* deviceName) {
    if (!deviceName || !*deviceName) {
        return false;
    }

    DISPLAY_DEVICEW displayDevice{};
    displayDevice.cb = sizeof(displayDevice);
    if (!EnumDisplayDevicesW(deviceName, 0, &displayDevice, 0)) {
        return false;
    }

    const std::wstring deviceId = displayDevice.DeviceID;
    const std::wstring deviceString = displayDevice.DeviceString;
    return deviceId.find(L"MTT1337") != std::wstring::npos
        || deviceId.find(L"MttVDD") != std::wstring::npos
        || deviceString.find(L"Virtual Display") != std::wstring::npos;
}

BOOL CALLBACK enumVirtualMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* foundVirtual = reinterpret_cast<bool*>(userData);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    if (looksLikeVirtualDisplayDevice(info.szDevice)) {
        *foundVirtual = true;
        return FALSE;
    }
    return TRUE;
}

bool hasVirtualMonitorPresent() {
    bool foundVirtual = false;
    EnumDisplayMonitors(nullptr, nullptr, enumVirtualMonitorProc, reinterpret_cast<LPARAM>(&foundVirtual));
    return foundVirtual;
}

struct DisplayInfo {
    std::wstring deviceName;
    RECT rect{};
    bool isPrimary = false;
    bool isVirtual = false;
};

BOOL CALLBACK enumDisplayInfoProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* displays = reinterpret_cast<std::vector<DisplayInfo>*>(userData);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    DisplayInfo display{};
    display.deviceName = info.szDevice;
    display.rect = info.rcMonitor;
    display.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    display.isVirtual = looksLikeVirtualDisplayDevice(info.szDevice);
    displays->push_back(display);
    return TRUE;
}

std::vector<DisplayInfo> enumerateDisplayInfos() {
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, enumDisplayInfoProc, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

std::string getXmlTagValue(const std::string& xml, const std::string& tag, std::size_t occurrence = 0) {
    const std::string openTag = "<" + tag + ">";
    const std::string closeTag = "</" + tag + ">";

    std::size_t searchPos = 0;
    for (std::size_t i = 0; i <= occurrence; ++i) {
        const std::size_t start = xml.find(openTag, searchPos);
        if (start == std::string::npos) {
            return {};
        }
        const std::size_t valueStart = start + openTag.size();
        const std::size_t end = xml.find(closeTag, valueStart);
        if (end == std::string::npos) {
            return {};
        }
        if (i == occurrence) {
            return trimCopy(xml.substr(valueStart, end - valueStart));
        }
        searchPos = end + closeTag.size();
    }
    return {};
}

bool replaceXmlTagValue(std::string& xml, const std::string& tag, const std::string& value, std::size_t occurrence = 0) {
    const std::string openTag = "<" + tag + ">";
    const std::string closeTag = "</" + tag + ">";

    std::size_t searchPos = 0;
    for (std::size_t i = 0; i <= occurrence; ++i) {
        const std::size_t start = xml.find(openTag, searchPos);
        if (start == std::string::npos) {
            return false;
        }
        const std::size_t valueStart = start + openTag.size();
        const std::size_t end = xml.find(closeTag, valueStart);
        if (end == std::string::npos) {
            return false;
        }
        if (i == occurrence) {
            xml.replace(valueStart, end - valueStart, value);
            return true;
        }
        searchPos = end + closeTag.size();
    }
    return false;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = trimCopy(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<std::string> splitCsv(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trimCopy(item);
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

std::string joinStrings(const std::vector<std::string>& values, const char* separator) {
    std::string result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result += separator;
        }
        result += values[i];
    }
    return result;
}

bool parseResolutionLine(const std::string& line, int& width, int& height, int& refreshRate) {
    width = height = refreshRate = 0;
    int matched = std::sscanf(line.c_str(), "%dx%d@%d", &width, &height, &refreshRate);
    if (matched != 3) {
        matched = std::sscanf(line.c_str(), "%d %d %d", &width, &height, &refreshRate);
    }
    return matched == 3 && width > 0 && height > 0 && refreshRate > 0;
}

bool loadVddSettingsUi() {
    const std::filesystem::path settingsPath = gVddSettingsPath.data();
    const std::string xml = readTextFile(settingsPath);
    if (xml.empty()) {
        return false;
    }

    gVddMonitorCount = std::max(1, std::atoi(getXmlTagValue(xml, "count").c_str()));
    std::snprintf(gVddGpuFriendlyName.data(), gVddGpuFriendlyName.size(), "%s", getXmlTagValue(xml, "friendlyname").c_str());

    {
        std::vector<std::string> refreshRates;
        for (std::size_t i = 0;; ++i) {
            const std::string value = getXmlTagValue(xml, "g_refresh_rate", i);
            if (value.empty()) {
                break;
            }
            refreshRates.push_back(value);
        }
        std::snprintf(gVddRefreshRatesCsv.data(), gVddRefreshRatesCsv.size(), "%s", joinStrings(refreshRates, ", ").c_str());
    }

    {
        std::vector<std::string> resolutions;
        for (std::size_t i = 0;; ++i) {
            const std::string width = getXmlTagValue(xml, "width", i);
            const std::string height = getXmlTagValue(xml, "height", i);
            const std::string refresh = getXmlTagValue(xml, "refresh_rate", i);
            if (width.empty() || height.empty() || refresh.empty()) {
                break;
            }
            resolutions.push_back(width + "x" + height + "@" + refresh);
        }
        std::snprintf(gVddResolutionsText.data(), gVddResolutionsText.size(), "%s", joinStrings(resolutions, "\r\n").c_str());
    }

    gVddSendLogsThroughPipe = getXmlTagValue(xml, "SendLogsThroughPipe") == "true";
    gVddFileLogging = getXmlTagValue(xml, "logging", 1) == "true";
    gVddDebugLogging = getXmlTagValue(xml, "debuglogging") == "true";
    gVddSdr10Bit = getXmlTagValue(xml, "SDR10bit") == "true";
    gVddHdrPlus = getXmlTagValue(xml, "HDRPlus") == "true";

    const std::string colourFormat = getXmlTagValue(xml, "ColourFormat");
    constexpr const char* kColourFormats[] = { "RGB", "YCbCr444", "YCbCr422", "YCbCr420" };
    gVddColourFormatIndex = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kColourFormats); ++i) {
        if (_stricmp(colourFormat.c_str(), kColourFormats[i]) == 0) {
            gVddColourFormatIndex = i;
            break;
        }
    }

    gVddHardwareCursor = getXmlTagValue(xml, "HardwareCursor") != "false";
    gVddCursorMaxX = std::max(1, std::atoi(getXmlTagValue(xml, "CursorMaxX").c_str()));
    gVddCursorMaxY = std::max(1, std::atoi(getXmlTagValue(xml, "CursorMaxY").c_str()));
    gVddAlphaCursorSupport = getXmlTagValue(xml, "AlphaCursorSupport") != "false";
    gVddXorCursorSupportLevel = std::max(0, std::atoi(getXmlTagValue(xml, "XorCursorSupportLevel").c_str()));

    gVddCustomEdid = getXmlTagValue(xml, "CustomEdid") == "true";
    gVddPreventSpoof = getXmlTagValue(xml, "PreventSpoof") == "true";
    gVddEdidCeaOverride = getXmlTagValue(xml, "EdidCeaOverride") == "true";
    gVddEdidIntegrationEnabled = getXmlTagValue(xml, "enabled") == "true";
    gVddEdidAutoConfigure = getXmlTagValue(xml, "auto_configure_from_edid") == "true";
    std::snprintf(gVddEdidProfilePath.data(), gVddEdidProfilePath.size(), "%s", getXmlTagValue(xml, "edid_profile_path").c_str());
    gVddEdidOverrideManual = getXmlTagValue(xml, "override_manual_settings") == "true";
    const std::string fallbackOnError = getXmlTagValue(xml, "fallback_on_error");
    gVddEdidFallbackOnError = fallbackOnError.empty() || fallbackOnError == "true";
    return true;
}

bool saveVddSettingsUi() {
    const std::filesystem::path settingsPath = gVddSettingsPath.data();
    std::string xml = readTextFile(settingsPath);
    if (xml.empty()) {
        return false;
    }

    constexpr const char* kColourFormats[] = { "RGB", "YCbCr444", "YCbCr422", "YCbCr420" };
    if (gVddColourFormatIndex < 0 || gVddColourFormatIndex >= IM_ARRAYSIZE(kColourFormats)) {
        gVddColourFormatIndex = 0;
    }

    if (!replaceXmlTagValue(xml, "count", std::to_string(std::max(1, gVddMonitorCount)))) return false;
    if (!replaceXmlTagValue(xml, "friendlyname", trimCopy(gVddGpuFriendlyName.data()))) return false;
    if (!replaceXmlTagValue(xml, "SendLogsThroughPipe", gVddSendLogsThroughPipe ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "logging", gVddFileLogging ? "true" : "false", 1)) return false;
    if (!replaceXmlTagValue(xml, "debuglogging", gVddDebugLogging ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "SDR10bit", gVddSdr10Bit ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "HDRPlus", gVddHdrPlus ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "ColourFormat", kColourFormats[gVddColourFormatIndex])) return false;
    if (!replaceXmlTagValue(xml, "HardwareCursor", gVddHardwareCursor ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "CursorMaxX", std::to_string(std::max(1, gVddCursorMaxX)))) return false;
    if (!replaceXmlTagValue(xml, "CursorMaxY", std::to_string(std::max(1, gVddCursorMaxY)))) return false;
    if (!replaceXmlTagValue(xml, "AlphaCursorSupport", gVddAlphaCursorSupport ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "XorCursorSupportLevel", std::to_string(std::max(0, gVddXorCursorSupportLevel)))) return false;
    if (!replaceXmlTagValue(xml, "CustomEdid", gVddCustomEdid ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "PreventSpoof", gVddPreventSpoof ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "EdidCeaOverride", gVddEdidCeaOverride ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "enabled", gVddEdidIntegrationEnabled ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "auto_configure_from_edid", gVddEdidAutoConfigure ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "edid_profile_path", trimCopy(gVddEdidProfilePath.data()))) return false;
    if (!replaceXmlTagValue(xml, "override_manual_settings", gVddEdidOverrideManual ? "true" : "false")) return false;
    if (!replaceXmlTagValue(xml, "fallback_on_error", gVddEdidFallbackOnError ? "true" : "false")) return false;

    const auto refreshRates = splitCsv(gVddRefreshRatesCsv.data());
    {
        const std::size_t globalStart = xml.find("<global>");
        const std::size_t globalEnd = xml.find("</global>", globalStart);
        if (globalStart == std::string::npos || globalEnd == std::string::npos) {
            return false;
        }

        std::string block = "<global>\r\n";
        for (const auto& refreshRate : refreshRates) {
            block += "        <g_refresh_rate>" + refreshRate + "</g_refresh_rate>\r\n";
        }
        block += "    </global>";
        xml.replace(globalStart, (globalEnd + std::strlen("</global>")) - globalStart, block);
    }

    const auto resolutionLines = splitLines(gVddResolutionsText.data());
    {
        const std::size_t resolutionsStart = xml.find("<resolutions>");
        const std::size_t resolutionsEnd = xml.find("</resolutions>", resolutionsStart);
        if (resolutionsStart == std::string::npos || resolutionsEnd == std::string::npos) {
            return false;
        }

        std::string block = "<resolutions>\r\n";
        for (const auto& line : resolutionLines) {
            int width = 0, height = 0, refreshRate = 0;
            if (!parseResolutionLine(line, width, height, refreshRate)) {
                continue;
            }
            block += "        <resolution>\r\n";
            block += "            <width>" + std::to_string(width) + "</width>\r\n";
            block += "            <height>" + std::to_string(height) + "</height>\r\n";
            block += "            <refresh_rate>" + std::to_string(refreshRate) + "</refresh_rate>\r\n";
            block += "        </resolution>\r\n";
        }
        block += "    </resolutions>";
        xml.replace(resolutionsStart, (resolutionsEnd + std::strlen("</resolutions>")) - resolutionsStart, block);
    }

    if (!writeTextFile(settingsPath, xml)) {
        return false;
    }

    gVddMonitorCount = std::max(1, gVddMonitorCount);
    return true;
}

bool loadVddMonitorCount() {
    const std::filesystem::path settingsPath = gVddSettingsPath.data();
    const std::string xml = readTextFile(settingsPath);
    if (xml.empty()) {
        return false;
    }

    const std::string openTag = "<count>";
    const std::string closeTag = "</count>";
    const std::size_t start = xml.find(openTag);
    const std::size_t end = xml.find(closeTag, start == std::string::npos ? 0 : start + openTag.size());
    if (start == std::string::npos || end == std::string::npos || end <= start + openTag.size()) {
        return false;
    }

    const std::string countText = trimCopy(xml.substr(start + openTag.size(), end - (start + openTag.size())));
    const int parsed = std::atoi(countText.c_str());
    if (parsed <= 0) {
        return false;
    }

    gVddMonitorCount = parsed;
    return true;
}

bool saveVddMonitorCount(int count) {
    if (count <= 0) {
        return false;
    }

    const std::filesystem::path settingsPath = gVddSettingsPath.data();
    const std::string xml = readTextFile(settingsPath);
    if (xml.empty()) {
        return false;
    }

    const std::string openTag = "<count>";
    const std::string closeTag = "</count>";
    const std::size_t start = xml.find(openTag);
    const std::size_t end = xml.find(closeTag, start == std::string::npos ? 0 : start + openTag.size());
    if (start == std::string::npos || end == std::string::npos || end <= start + openTag.size()) {
        return false;
    }

    std::string updated = xml;
    updated.replace(start + openTag.size(), end - (start + openTag.size()), std::to_string(count));
    if (!writeTextFile(settingsPath, updated)) {
        return false;
    }

    gVddMonitorCount = count;
    return true;
}

bool launchPath(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    HINSTANCE result = ShellExecuteW(gMainWindow, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::filesystem::path getExecutableDir() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (!len) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path firstExistingPath(std::initializer_list<std::filesystem::path> candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

void copyPathToBuffer(std::array<char, MAX_PATH>& buffer, const std::filesystem::path& path) {
    const std::string text = path.string();
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}

void initializeVddDefaults(const char* userProfile) {
    const std::filesystem::path exeDir = getExecutableDir();
    const std::filesystem::path exeParent = exeDir.parent_path();
    const std::filesystem::path exeGrandparent = exeParent.parent_path();

    const std::filesystem::path repoVendorControl = exeGrandparent / "_vendor" / "Virtual-Driver-Control" / "VirtualDriverControl" / "dist" / "win-unpacked" / "Virtual Driver Control.exe";
    const std::filesystem::path repoVendorSettings = exeGrandparent / "_vendor" / "Virtual-Display-Driver" / "Virtual Display Driver (HDR)" / "vdd_settings.xml";
    const std::filesystem::path repoVendorInf = exeGrandparent / "_vendor" / "Virtual-Display-Driver" / "Virtual Display Driver (HDR)" / "MttVDD" / "MttVDD.inf";

    const std::filesystem::path packagedControl = exeDir / "VirtualDisplayDriver" / "control" / "VDD Control.exe";
    const std::filesystem::path packagedSettings = exeDir / "VirtualDisplayDriver" / "control" / "Dependencies" / "vdd_settings.xml";
    const std::filesystem::path packagedInf = exeDir / "VirtualDisplayDriver" / "control" / "SignedDrivers" / "x86" / "VDD" / "MttVDD.inf";

    std::filesystem::path downloadsBase;
    if (userProfile && *userProfile) {
        downloadsBase = std::filesystem::path(userProfile) / "Downloads" / "VirtualDisplayDriver" / "control";
    }

    const std::filesystem::path controlPath = firstExistingPath({
        packagedControl,
        exeParent / "VirtualDisplayDriver" / "control" / "VDD Control.exe",
        repoVendorControl,
        downloadsBase / "VDD Control.exe"
    });

    const std::filesystem::path settingsPath = firstExistingPath({
        std::filesystem::path("C:\\VirtualDisplayDriver\\vdd_settings.xml"),
        packagedSettings,
        exeParent / "VirtualDisplayDriver" / "control" / "Dependencies" / "vdd_settings.xml",
        repoVendorSettings,
        downloadsBase / "Dependencies" / "vdd_settings.xml"
    });

    const std::filesystem::path infPath = firstExistingPath({
        packagedInf,
        exeParent / "VirtualDisplayDriver" / "control" / "SignedDrivers" / "x86" / "VDD" / "MttVDD.inf",
        repoVendorInf,
        downloadsBase / "SignedDrivers" / "x86" / "VDD" / "MttVDD.inf"
    });

    copyPathToBuffer(gVddControlPath, controlPath);
    copyPathToBuffer(gVddSettingsPath, settingsPath);
    copyPathToBuffer(gVddDriverInfPath, infPath);
    loadVddMonitorCount();
    loadVddSettingsUi();
}

bool findDisplayInfoByName(const std::wstring& deviceName, DisplayInfo& displayInfo) {
    if (deviceName.empty()) {
        return false;
    }

    const auto displays = enumerateDisplayInfos();
    const auto it = std::find_if(displays.begin(), displays.end(), [&](const DisplayInfo& display) {
        return _wcsicmp(display.deviceName.c_str(), deviceName.c_str()) == 0;
    });
    if (it == displays.end()) {
        return false;
    }

    displayInfo = *it;
    return true;
}

RECT getInitialHostWindowRect(int clientWidth, int clientHeight) {
    constexpr int kDefaultX = 100;
    constexpr int kDefaultY = 100;

    RECT windowRect{ 0, 0, clientWidth, clientHeight };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;

    DisplayInfo targetDisplay{};
    bool foundTarget = false;
    if (!gOriginalPrimaryDisplayDevice.empty()) {
        foundTarget = findDisplayInfoByName(gOriginalPrimaryDisplayDevice, targetDisplay);
    }
    if (!foundTarget) {
        const auto displays = enumerateDisplayInfos();
        const auto it = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
            return !display.isVirtual;
        });
        if (it != displays.end()) {
            targetDisplay = *it;
            foundTarget = true;
        }
    }

    if (!foundTarget) {
        return RECT{ kDefaultX, kDefaultY, kDefaultX + windowWidth, kDefaultY + windowHeight };
    }

    const int displayWidth = targetDisplay.rect.right - targetDisplay.rect.left;
    const int displayHeight = targetDisplay.rect.bottom - targetDisplay.rect.top;
    const int x = targetDisplay.rect.left + std::max(0, (displayWidth - windowWidth) / 2);
    const int y = targetDisplay.rect.top + std::max(0, (displayHeight - windowHeight) / 2);
    return RECT{ x, y, x + windowWidth, y + windowHeight };
}

bool waitForPrimaryDisplay(const std::wstring& primaryDeviceName, int timeoutMs) {
    if (primaryDeviceName.empty()) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto displays = enumerateDisplayInfos();
        const auto it = std::find_if(displays.begin(), displays.end(), [&](const DisplayInfo& display) {
            return display.isPrimary && _wcsicmp(display.deviceName.c_str(), primaryDeviceName.c_str()) == 0;
        });
        if (it != displays.end()) {
            return true;
        }
        Sleep(100);
    }

    return false;
}

void moveHostWindowToDisplay(HWND hwnd, const std::wstring& displayDeviceName) {
    if (!hwnd || displayDeviceName.empty()) {
        return;
    }

    DisplayInfo targetDisplay{};
    if (!findDisplayInfoByName(displayDeviceName, targetDisplay)) {
        return;
    }

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) {
        return;
    }

    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    const int displayWidth = targetDisplay.rect.right - targetDisplay.rect.left;
    const int displayHeight = targetDisplay.rect.bottom - targetDisplay.rect.top;
    const int x = targetDisplay.rect.left + std::max(0, (displayWidth - windowWidth) / 2);
    const int y = targetDisplay.rect.top + std::max(0, (displayHeight - windowHeight) / 2);

    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool getPathSourceDeviceName(const DISPLAYCONFIG_PATH_INFO& path, std::wstring& deviceName) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
        return false;
    }

    deviceName = sourceName.viewGdiDeviceName;
    return true;
}

bool applyPrimaryDisplayDevice(const std::wstring& primaryDeviceName) {
    const auto displays = enumerateDisplayInfos();
    const auto targetIt = std::find_if(displays.begin(), displays.end(), [&](const DisplayInfo& display) {
        return _wcsicmp(display.deviceName.c_str(), primaryDeviceName.c_str()) == 0;
    });
    if (targetIt == displays.end()) {
        appendLog("applyPrimaryDisplayDevice target not found");
        return false;
    }

    DEVMODEW targetMode{};
    targetMode.dmSize = sizeof(targetMode);
    if (!EnumDisplaySettingsExW(targetIt->deviceName.c_str(), ENUM_CURRENT_SETTINGS, &targetMode, 0)) {
        appendLog("EnumDisplaySettingsExW failed for target display");
        return false;
    }

    targetMode.dmFields = DM_POSITION;
    targetMode.dmPosition.x = 0;
    targetMode.dmPosition.y = 0;

    LONG result = ChangeDisplaySettingsExW(
        targetIt->deviceName.c_str(),
        &targetMode,
        nullptr,
        CDS_SET_PRIMARY | CDS_UPDATEREGISTRY | CDS_NORESET,
        nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        std::ostringstream oss;
        oss << "ChangeDisplaySettingsExW target primary failed error=" << result;
        appendLog(oss.str());
        return false;
    }

    const LONG offsetX = -targetIt->rect.left;
    const LONG offsetY = -targetIt->rect.top;

    for (const auto& display : displays) {
        if (_wcsicmp(display.deviceName.c_str(), primaryDeviceName.c_str()) == 0) {
            continue;
        }

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(display.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
            appendLog("EnumDisplaySettingsExW failed for secondary display");
            return false;
        }

        mode.dmFields = DM_POSITION;
        mode.dmPosition.x = display.rect.left + offsetX;
        mode.dmPosition.y = display.rect.top + offsetY;

        result = ChangeDisplaySettingsExW(
            display.deviceName.c_str(),
            &mode,
            nullptr,
            CDS_UPDATEREGISTRY | CDS_NORESET,
            nullptr);
        if (result != DISP_CHANGE_SUCCESSFUL) {
            std::ostringstream oss;
            oss << "ChangeDisplaySettingsExW secondary failed error=" << result;
            appendLog(oss.str());
            return false;
        }
    }

    result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        std::ostringstream oss;
        oss << "ChangeDisplaySettingsExW final apply failed error=" << result;
        appendLog(oss.str());
        return false;
    }

    appendLog("Primary display switched successfully");
    return true;
}

std::wstring narrowArgToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (sizeNeeded <= 0) {
        return {};
    }

    std::wstring out(static_cast<std::size_t>(sizeNeeded), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), sizeNeeded);
    return out;
}

bool startPrimaryRestoreHelper(const std::wstring& exePath, DWORD parentPid, const std::wstring& originalPrimaryDevice) {
    if (exePath.empty() || originalPrimaryDevice.empty()) {
        return false;
    }

    std::ostringstream deviceArgUtf8;
    deviceArgUtf8 << narrowUtf16(originalPrimaryDevice);

    std::ostringstream cmd;
    cmd << winQuote(narrowUtf16(exePath))
        << " --restore-primary-helper"
        << " --parent-pid " << parentPid
        << " --primary-device " << winQuote(deviceArgUtf8.str());

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string commandLine = cmd.str();

    const BOOL ok = CreateProcessA(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);
    if (!ok) {
        appendLog("Failed to start primary restore helper");
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void switchVirtualDisplayToPrimaryForApp(const std::wstring& exePath) {
    if (!gMakeVirtualPrimaryWhileRunning) {
        return;
    }

    const auto displays = enumerateDisplayInfos();
    const auto primaryIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
        return display.isPrimary;
    });
    const auto virtualIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
        return display.isVirtual;
    });

    if (primaryIt == displays.end() || virtualIt == displays.end()) {
        appendLog("Primary/virtual display not found for primary switch");
        return;
    }

    if (_wcsicmp(primaryIt->deviceName.c_str(), virtualIt->deviceName.c_str()) == 0) {
        const auto nonVirtualIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
            return !display.isVirtual;
        });
        if (nonVirtualIt != displays.end()) {
            gOriginalPrimaryDisplayDevice = nonVirtualIt->deviceName;
            gPreferredHostWindowDisplayDevice = nonVirtualIt->deviceName;
        } else {
            gOriginalPrimaryDisplayDevice = primaryIt->deviceName;
        }
        gVirtualPrimaryDisplayDevice = virtualIt->deviceName;
        appendLog("Virtual display already primary");
        return;
    }

    gOriginalPrimaryDisplayDevice = primaryIt->deviceName;
    gVirtualPrimaryDisplayDevice = virtualIt->deviceName;

    if (!applyPrimaryDisplayDevice(gVirtualPrimaryDisplayDevice)) {
        appendLog("Failed to switch virtual display to primary");
        return;
    }

    gChangedPrimaryDisplayThisRun = true;
    appendLog("Switched virtual display to primary");
    appendRelayStatus("Virtual display is now primary.");
    startPrimaryRestoreHelper(exePath, GetCurrentProcessId(), gOriginalPrimaryDisplayDevice);
}

bool restoreOriginalPrimaryDisplay() {
    if (gPrimaryRestoreInProgress || !gChangedPrimaryDisplayThisRun || gOriginalPrimaryDisplayDevice.empty()) {
        return false;
    }

    gPrimaryRestoreInProgress = true;
    if (applyPrimaryDisplayDevice(gOriginalPrimaryDisplayDevice)) {
        appendLog("Restored original primary display");
        const bool restored = waitForPrimaryDisplay(gOriginalPrimaryDisplayDevice, 3000);
        if (restored) {
            gChangedPrimaryDisplayThisRun = false;
        }
        gPrimaryRestoreInProgress = false;
        return restored;
    }

    appendLog("Failed to restore original primary display");
    gPrimaryRestoreInProgress = false;
    return false;
}

bool runElevatedPnpUtil(const wchar_t* operation) {
    std::wstring parameters = operation;
    parameters += L" \"";
    parameters += kVirtualDisplayInstanceId;
    parameters += L"\"";

    SHELLEXECUTEINFOW execInfo{};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.hwnd = gMainWindow;
    execInfo.lpVerb = L"runas";
    execInfo.lpFile = L"pnputil.exe";
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execInfo)) {
        appendLog("pnputil launch failed");
        appendRelayStatus("Virtual display action was canceled or failed.");
        return false;
    }

    WaitForSingleObject(execInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(execInfo.hProcess, &exitCode);
    CloseHandle(execInfo.hProcess);

    std::ostringstream oss;
    oss << "pnputil " << narrowUtf16(operation) << " exit=" << exitCode;
    appendLog(oss.str());
    return exitCode == 0;
}

bool runElevatedInstallDriver(const std::wstring& infPath) {
    if (infPath.empty()) {
        return false;
    }

    std::wstring parameters = L"/add-driver \"";
    parameters += infPath;
    parameters += L"\" /install";

    SHELLEXECUTEINFOW execInfo{};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.hwnd = gMainWindow;
    execInfo.lpVerb = L"runas";
    execInfo.lpFile = L"pnputil.exe";
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execInfo)) {
        appendLog("driver install launch failed");
        appendRelayStatus("Driver reinstall was canceled or failed.");
        return false;
    }

    WaitForSingleObject(execInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(execInfo.hProcess, &exitCode);
    CloseHandle(execInfo.hProcess);

    std::ostringstream oss;
    oss << "pnputil add-driver exit=" << exitCode;
    appendLog(oss.str());
    return exitCode == 0;
}

void normalizePrimaryDisplayForAppStartup() {
    if (!gMakeVirtualPrimaryWhileRunning) {
        return;
    }

    const auto displays = enumerateDisplayInfos();
    const auto primaryIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
        return display.isPrimary;
    });
    const auto virtualIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
        return display.isVirtual;
    });
    const auto nonVirtualIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
        return !display.isVirtual;
    });

    if (primaryIt == displays.end() || virtualIt == displays.end() || nonVirtualIt == displays.end()) {
        return;
    }

    gPreferredHostWindowDisplayDevice = nonVirtualIt->deviceName;
    gOriginalPrimaryDisplayDevice = nonVirtualIt->deviceName;
    gVirtualPrimaryDisplayDevice = virtualIt->deviceName;

    if (_wcsicmp(primaryIt->deviceName.c_str(), virtualIt->deviceName.c_str()) != 0) {
        return;
    }

    appendLog("Normalizing startup primary display back to laptop");
    if (applyPrimaryDisplayDevice(nonVirtualIt->deviceName) && waitForPrimaryDisplay(nonVirtualIt->deviceName, 3000)) {
        appendLog("Startup primary display normalized");
    } else {
        appendLog("Failed to normalize startup primary display");
    }
}

bool waitForVirtualMonitorState(bool shouldExist, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasVirtualMonitorPresent() == shouldExist) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return hasVirtualMonitorPresent() == shouldExist;
}

void ensureVirtualDisplayEnabledForApp() {
    if (!gAutoManageVirtualDisplay) {
        return;
    }

    if (hasVirtualMonitorPresent()) {
        appendLog("virtual monitor already present");
        return;
    }

    appendRelayStatus("Enabling virtual display...");
    if (!runElevatedPnpUtil(L"/enable-device")) {
        return;
    }

    if (waitForVirtualMonitorState(true, 5000)) {
        gManagedVirtualDisplayThisRun = true;
        appendRelayStatus("Virtual display enabled.");
        appendLog("virtual monitor enabled");
    } else {
        appendRelayStatus("Virtual display did not appear in time.");
        appendLog("virtual monitor enable timed out");
    }
}

void disableVirtualDisplayForAppExit() {
    if (!gAutoManageVirtualDisplay) {
        return;
    }

    if (!hasVirtualMonitorPresent()) {
        return;
    }

    appendRelayStatus("Disabling virtual display...");
    if (!runElevatedPnpUtil(L"/disable-device")) {
        return;
    }

    if (waitForVirtualMonitorState(false, 5000)) {
        appendLog("virtual monitor disabled");
    } else {
        appendLog("virtual monitor disable timed out");
    }
}

void drawVirtualDriverControlUi() {
    ImGui::InputText("VDD Control Path", gVddControlPath.data(), gVddControlPath.size());
    ImGui::InputText("VDD Settings XML", gVddSettingsPath.data(), gVddSettingsPath.size());
    ImGui::InputText("VDD Driver INF", gVddDriverInfPath.data(), gVddDriverInfPath.size());

    const bool controlExists = std::filesystem::exists(std::filesystem::path(gVddControlPath.data()));
    const bool settingsExists = std::filesystem::exists(std::filesystem::path(gVddSettingsPath.data()));
    const bool driverInfExists = std::filesystem::exists(std::filesystem::path(gVddDriverInfPath.data()));
    const bool monitorPresent = hasVirtualMonitorPresent();

    ImGui::Text("Virtual monitor present: %s", monitorPresent ? "Yes" : "No");
    ImGui::Text("Control tool found: %s", controlExists ? "Yes" : "No");
    ImGui::Text("Settings XML found: %s", settingsExists ? "Yes" : "No");
    ImGui::Text("Driver INF found: %s", driverInfExists ? "Yes" : "No");

    if (ImGui::Button("Open VDD Control")) {
        if (!controlExists || !launchPath(narrowArgToWide(gVddControlPath.data()))) {
            appendRelayStatus("Failed to open VDD Control.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Settings Folder")) {
        const std::filesystem::path settingsPath = gVddSettingsPath.data();
        const std::wstring folderPath = settingsPath.parent_path().wstring();
        if (folderPath.empty() || !launchPath(folderPath)) {
            appendRelayStatus("Failed to open VDD settings folder.");
        }
    }

    if (ImGui::Button("Enable Virtual Display")) {
        ensureVirtualDisplayEnabledForApp();
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable Virtual Display")) {
        disableVirtualDisplayForAppExit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Driver")) {
        bool ok = runElevatedPnpUtil(L"/disable-device");
        ok = ok && waitForVirtualMonitorState(false, 5000);
        ok = ok && runElevatedPnpUtil(L"/enable-device");
        ok = ok && waitForVirtualMonitorState(true, 5000);
        appendRelayStatus(ok ? "Driver reload completed." : "Driver reload failed.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reinstall Driver Package")) {
        if (!driverInfExists || !runElevatedInstallDriver(narrowArgToWide(gVddDriverInfPath.data()))) {
            appendRelayStatus("Driver reinstall failed.");
        } else {
            appendRelayStatus("Driver reinstall command completed.");
        }
    }

    if (settingsExists && gVddMonitorCount <= 0) {
        loadVddMonitorCount();
    }

    ImGui::InputInt("Monitor Count", &gVddMonitorCount);
    if (gVddMonitorCount < 1) {
        gVddMonitorCount = 1;
    }
    if (ImGui::Button("Load Monitor Count")) {
        if (!loadVddMonitorCount()) {
            appendRelayStatus("Failed to load monitor count from vdd_settings.xml.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Monitor Count")) {
        if (!saveVddMonitorCount(gVddMonitorCount)) {
            appendRelayStatus("Failed to save monitor count to vdd_settings.xml.");
        } else {
            appendRelayStatus("Saved monitor count to vdd_settings.xml.");
        }
    }

    if (ImGui::Button("Load VDD Settings")) {
        if (!loadVddSettingsUi()) {
            appendRelayStatus("Failed to load VDD settings.");
        } else {
            appendRelayStatus("Loaded VDD settings.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save VDD Settings")) {
        if (!saveVddSettingsUi()) {
            appendRelayStatus("Failed to save VDD settings.");
        } else {
            appendRelayStatus("Saved VDD settings.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save && Reload Driver")) {
        bool ok = saveVddSettingsUi();
        if (ok) {
            ok = runElevatedPnpUtil(L"/disable-device");
            ok = ok && waitForVirtualMonitorState(false, 5000);
            ok = ok && runElevatedPnpUtil(L"/enable-device");
            ok = ok && waitForVirtualMonitorState(true, 5000);
        }
        appendRelayStatus(ok ? "Saved VDD settings and reloaded driver." : "Save/reload driver failed.");
    }

    ImGui::Separator();
    ImGui::InputText("GPU Friendly Name", gVddGpuFriendlyName.data(), gVddGpuFriendlyName.size());
    ImGui::InputText("Refresh Rates (CSV)", gVddRefreshRatesCsv.data(), gVddRefreshRatesCsv.size());
    ImGui::InputTextMultiline("Resolutions", gVddResolutionsText.data(), gVddResolutionsText.size(), ImVec2(-FLT_MIN, 90.0f));
    ImGui::TextDisabled("Use one resolution per line, for example: 1920x1080@60");

    if (ImGui::CollapsingHeader("Logging", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Send Logs Through Pipe", &gVddSendLogsThroughPipe);
        ImGui::Checkbox("File Logging", &gVddFileLogging);
        ImGui::Checkbox("Debug Logging", &gVddDebugLogging);
    }

    if (ImGui::CollapsingHeader("Display Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* kColourFormats[] = { "RGB", "YCbCr444", "YCbCr422", "YCbCr420" };
        ImGui::Checkbox("SDR 10-bit", &gVddSdr10Bit);
        ImGui::Checkbox("HDR Plus", &gVddHdrPlus);
        ImGui::Combo("Colour Format", &gVddColourFormatIndex, kColourFormats, IM_ARRAYSIZE(kColourFormats));
    }

    if (ImGui::CollapsingHeader("Cursor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Hardware Cursor", &gVddHardwareCursor);
        ImGui::Checkbox("Alpha Cursor Support", &gVddAlphaCursorSupport);
        ImGui::InputInt("Cursor Max X", &gVddCursorMaxX);
        ImGui::InputInt("Cursor Max Y", &gVddCursorMaxY);
        ImGui::InputInt("Xor Cursor Support Level", &gVddXorCursorSupportLevel);
        gVddCursorMaxX = std::max(1, gVddCursorMaxX);
        gVddCursorMaxY = std::max(1, gVddCursorMaxY);
        gVddXorCursorSupportLevel = std::max(0, gVddXorCursorSupportLevel);
    }

    if (ImGui::CollapsingHeader("EDID", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Custom EDID", &gVddCustomEdid);
        ImGui::Checkbox("Prevent Spoof", &gVddPreventSpoof);
        ImGui::Checkbox("EDID CEA Override", &gVddEdidCeaOverride);
        ImGui::Checkbox("EDID Integration Enabled", &gVddEdidIntegrationEnabled);
        ImGui::Checkbox("Auto Configure From EDID", &gVddEdidAutoConfigure);
        ImGui::InputText("EDID Profile Path", gVddEdidProfilePath.data(), gVddEdidProfilePath.size());
        ImGui::Checkbox("Override Manual Settings", &gVddEdidOverrideManual);
        ImGui::Checkbox("Fallback On Error", &gVddEdidFallbackOnError);
    }
}

std::string runCommandCapture(const std::string& command) {
    std::string output;
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) {
        return "Failed to start command.";
    }

    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    const int exitCode = _pclose(pipe);
    if (exitCode != 0) {
        if (!output.empty() && output.back() != '\n') {
            output.push_back('\n');
        }
        output += "Command exit code: " + std::to_string(exitCode);
    }
    return output;
}

std::string buildSshCommand(const std::string& userAtHost, const std::string& keyPath, const std::string& remoteCommand) {
    std::ostringstream cmd;
    cmd << "ssh ";
    if (!keyPath.empty()) {
        cmd << "-i " << winQuote(keyPath) << ' ';
    }
    cmd << userAtHost << ' ' << winQuote(remoteCommand);
    return cmd.str();
}

void launchRelayAction(bool startRelay, uint16_t listenPort) {
    if (gRelayActionRunning.exchange(true)) {
        return;
    }

    const std::string piHost = trimCopy(gPiHost.data());
    const std::string piUser = trimCopy(gPiUser.data());
    const std::string sshKeyPath = trimCopy(gSshKeyPath.data());
    const std::string bridgeHostIp = trimCopy(gBridgeHostIp.data());
    const std::string piRelayPath = trimCopy(gPiRelayPath.data());
    const std::string piRelayLogPath = trimCopy(gPiRelayLogPath.data());

    std::thread([=]() {
        auto finish = []() {
            gRelayActionRunning.store(false);
        };

        if (piHost.empty() || piUser.empty()) {
            appendRelayStatus("Relay action failed: Pi SSH host/user is empty.");
            finish();
            return;
        }

        if (startRelay && (bridgeHostIp.empty() || piRelayPath.empty() || piRelayLogPath.empty())) {
            appendRelayStatus("Relay action failed: bridge IP, relay path, or relay log path is empty.");
            finish();
            return;
        }

        const std::string userAtHost = piUser + "@" + piHost;
        const std::string stopRemote = "sudo pkill -f Calc2KeyPiRelay >/dev/null 2>&1 || true";
        const std::string stopCommand = buildSshCommand(userAtHost, sshKeyPath, stopRemote);

        appendRelayStatus(startRelay ? "Stopping any existing Pi relay..." : "Stopping Pi relay...");
        const std::string stopOutput = trimCopy(runCommandCapture(stopCommand));
        if (!stopOutput.empty()) {
            appendRelayStatus(stopOutput);
        }

        if (startRelay) {
            const std::string remoteStart =
                "bash -lc " + shellSingleQuote(
                    "setsid sudo " + piRelayPath + " --bridge " + bridgeHostIp + ":" + std::to_string(listenPort) +
                    " > " + piRelayLogPath + " 2>&1 < /dev/null & disown");
            appendRelayStatus("Starting Pi relay...");
            const std::string startOutput = trimCopy(runCommandCapture(buildSshCommand(userAtHost, sshKeyPath, remoteStart)));
            if (!startOutput.empty()) {
                appendRelayStatus(startOutput);
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
            const std::string verifyRemote = "pgrep -af Calc2KeyPiRelay || true; tail -n 5 " + piRelayLogPath + " || true";
            const std::string verifyOutput = trimCopy(runCommandCapture(buildSshCommand(userAtHost, sshKeyPath, verifyRemote)));
            appendRelayStatus(verifyOutput.empty() ? "Relay verification returned no output." : verifyOutput);
        } else {
            appendRelayStatus("Pi relay stop command sent.");
        }

        finish();
    }).detach();
}

struct ScreenCapture {
    HDC screenDc = nullptr;
    HDC memDc = nullptr;
    HDC srcMemDc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP srcBitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HGDIOBJ srcOldBitmap = nullptr;
    void* pixels = nullptr;
    void* srcPixels = nullptr;
    int srcX = 0;
    int srcY = 0;
    int srcW = 0;
    int srcH = 0;
    int srcBitmapW = 0;
    int srcBitmapH = 0;
    HMONITOR selectedMonitor = nullptr;
    bool selectedMonitorIsVirtual = false;
    struct MonitorChoice {
        HMONITOR handle = nullptr;
        RECT rect{};
        bool isPrimary = false;
        bool isVirtual = false;
        std::wstring deviceName;
    };

    static BOOL CALLBACK enumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
        auto* choices = reinterpret_cast<std::vector<MonitorChoice>*>(userData);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return TRUE;
        }

        MonitorChoice choice{};
        choice.handle = monitor;
        choice.rect = info.rcMonitor;
        choice.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        choice.isVirtual = looksLikeVirtualDisplayDevice(info.szDevice);
        choice.deviceName = info.szDevice;
        choices->push_back(choice);
        return TRUE;
    }

    bool refreshMonitorSelection() {
        std::vector<MonitorChoice> choices;
        EnumDisplayMonitors(nullptr, nullptr, enumMonitorProc, reinterpret_cast<LPARAM>(&choices));
        if (choices.empty()) {
            selectedMonitor = nullptr;
            selectedMonitorIsVirtual = false;
            return false;
        }

        auto best = choices.end();
        best = std::find_if(choices.begin(), choices.end(), [](const MonitorChoice& choice) {
            return choice.isVirtual;
        });
        if (best == choices.end()) {
            best = std::find_if(choices.begin(), choices.end(), [](const MonitorChoice& choice) {
                return choice.isPrimary;
            });
        }
        if (best == choices.end()) {
            best = choices.begin();
        }

        selectedMonitor = best->handle;
        selectedMonitorIsVirtual = best->isVirtual;
        srcX = best->rect.left;
        srcY = best->rect.top;
        srcW = best->rect.right - best->rect.left;
        srcH = best->rect.bottom - best->rect.top;
        return srcW > 0 && srcH > 0;
    }

    bool refreshSourceMetrics() {
        return refreshMonitorSelection();
    }

    bool initialize() {
        if (!refreshSourceMetrics()) {
            return false;
        }

        screenDc = GetDC(nullptr);
        memDc = CreateCompatibleDC(screenDc);
        srcMemDc = CreateCompatibleDC(screenDc);
        if (!screenDc || !memDc || !srcMemDc) {
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kCalcWidth;
        bmi.bmiHeader.biHeight = -kCalcHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        bitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) {
            return false;
        }

        oldBitmap = SelectObject(memDc, bitmap);
        // Sharper scaling works better for desktop text/UI than HALFTONE here.
        SetStretchBltMode(memDc, COLORONCOLOR);
        SetBrushOrgEx(memDc, 0, 0, nullptr);
        return true;
    }

    bool ensureSourceBitmap() {
        if (!selectedMonitorIsVirtual) {
            return true;
        }

        if (srcBitmap && srcBitmapW == srcW && srcBitmapH == srcH && srcPixels) {
            return true;
        }

        if (srcMemDc && srcOldBitmap) {
            SelectObject(srcMemDc, srcOldBitmap);
            srcOldBitmap = nullptr;
        }
        if (srcBitmap) {
            DeleteObject(srcBitmap);
            srcBitmap = nullptr;
        }
        srcPixels = nullptr;
        srcBitmapW = 0;
        srcBitmapH = 0;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = srcW;
        bmi.bmiHeader.biHeight = -srcH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        srcBitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &srcPixels, nullptr, 0);
        if (!srcBitmap || !srcPixels) {
            return false;
        }

        srcOldBitmap = SelectObject(srcMemDc, srcBitmap);
        srcBitmapW = srcW;
        srcBitmapH = srcH;
        SetStretchBltMode(srcMemDc, COLORONCOLOR);
        SetBrushOrgEx(srcMemDc, 0, 0, nullptr);
        return true;
    }

    void resampleVirtualDisplay() {
        const auto* src = static_cast<const uint8_t*>(srcPixels);
        auto* dst = static_cast<uint8_t*>(pixels);
        const size_t srcStride = static_cast<size_t>(srcW) * 4;
        const size_t dstStride = static_cast<size_t>(kCalcWidth) * 4;

        for (int y = 0; y < kCalcHeight; ++y) {
            const int srcYIdx = std::min(srcH - 1, (y * srcH) / kCalcHeight);
            const uint8_t* srcRow = src + static_cast<size_t>(srcYIdx) * srcStride;
            uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;
            for (int x = 0; x < kCalcWidth; ++x) {
                const int srcXIdx = std::min(srcW - 1, (x * srcW) / kCalcWidth);
                const uint8_t* srcPixel = srcRow + static_cast<size_t>(srcXIdx) * 4;
                uint8_t* dstPixel = dstRow + static_cast<size_t>(x) * 4;
                dstPixel[0] = srcPixel[0];
                dstPixel[1] = srcPixel[1];
                dstPixel[2] = srcPixel[2];
                dstPixel[3] = srcPixel[3];
            }
        }
    }

    void shutdown() {
        if (memDc && oldBitmap) {
            SelectObject(memDc, oldBitmap);
            oldBitmap = nullptr;
        }
        if (srcMemDc && srcOldBitmap) {
            SelectObject(srcMemDc, srcOldBitmap);
            srcOldBitmap = nullptr;
        }
        if (bitmap) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        if (srcBitmap) {
            DeleteObject(srcBitmap);
            srcBitmap = nullptr;
        }
        if (memDc) {
            DeleteDC(memDc);
            memDc = nullptr;
        }
        if (srcMemDc) {
            DeleteDC(srcMemDc);
            srcMemDc = nullptr;
        }
        if (screenDc) {
            ReleaseDC(nullptr, screenDc);
            screenDc = nullptr;
        }
        pixels = nullptr;
        srcPixels = nullptr;
    }

    bool capture(uint8_t* outBgra) {
        if (!screenDc || !memDc || !pixels) {
            return false;
        }

        if (!refreshSourceMetrics()) {
            return false;
        }

        if (selectedMonitorIsVirtual) {
            if (!ensureSourceBitmap()) {
                return false;
            }
            if (!BitBlt(srcMemDc, 0, 0, srcW, srcH, screenDc, srcX, srcY, SRCCOPY | CAPTUREBLT)) {
                return false;
            }
            resampleVirtualDisplay();
        } else {
            // Prefer the virtual display when present so the calculator can target a
            // dedicated low-resolution monitor without depending on the laptop panel.
            if (!StretchBlt(memDc, 0, 0, kCalcWidth, kCalcHeight, screenDc, srcX, srcY, srcW, srcH, SRCCOPY | CAPTUREBLT)) {
                return false;
            }
        }

        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) && cursorInfo.flags == CURSOR_SHOWING) {
            ICONINFO iconInfo{};
            if (GetIconInfo(cursorInfo.hCursor, &iconInfo)) {
                const int cursorScreenX = cursorInfo.ptScreenPos.x - static_cast<int>(iconInfo.xHotspot);
                const int cursorScreenY = cursorInfo.ptScreenPos.y - static_cast<int>(iconInfo.yHotspot);
                const int cursorX = static_cast<int>((static_cast<long long>(cursorScreenX - srcX) * kCalcWidth) / std::max(srcW, 1));
                const int cursorY = static_cast<int>((static_cast<long long>(cursorScreenY - srcY) * kCalcHeight) / std::max(srcH, 1));
                const int cursorWidth = std::max(4, static_cast<int>((static_cast<long long>(GetSystemMetrics(SM_CXCURSOR)) * kCalcWidth) / std::max(srcW, 1)));
                const int cursorHeight = std::max(4, static_cast<int>((static_cast<long long>(GetSystemMetrics(SM_CYCURSOR)) * kCalcHeight) / std::max(srcH, 1)));

                DrawIconEx(memDc, cursorX, cursorY, cursorInfo.hCursor, cursorWidth, cursorHeight, 0, nullptr, DI_NORMAL);

                if (iconInfo.hbmMask) {
                    DeleteObject(iconInfo.hbmMask);
                }
                if (iconInfo.hbmColor) {
                    DeleteObject(iconInfo.hbmColor);
                }
            }
        }

        std::memcpy(outBgra, pixels, static_cast<size_t>(kCalcWidth * kCalcHeight * 4));
        return true;
    }
};

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    gPSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    gPd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &gMainRenderTargetView);
    backBuffer->Release();
}

void cleanupRenderTarget() {
    if (gMainRenderTargetView) {
        gMainRenderTargetView->Release();
        gMainRenderTargetView = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &gPSwapChain,
        &gPd3dDevice, &featureLevel, &gPd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &gPSwapChain,
            &gPd3dDevice, &featureLevel, &gPd3dDeviceContext);
    }
    if (res != S_OK) {
        return false;
    }

    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (gPSwapChain) { gPSwapChain->Release(); gPSwapChain = nullptr; }
    if (gPd3dDeviceContext) { gPd3dDeviceContext->Release(); gPd3dDeviceContext = nullptr; }
    if (gPd3dDevice) { gPd3dDevice->Release(); gPd3dDevice = nullptr; }
}

const char* getCalcKeyName(uint8_t i) {
    const uint8_t dataIdx = (i >> 4) & 7;
    const uint8_t keyShift = i & 0xF;
    const uint8_t key = 1u << keyShift;
    switch (dataIdx) {
    case 0:
        if (key & 0x01) return "Graph";
        if (key & 0x02) return "Trace";
        if (key & 0x04) return "Zoom";
        if (key & 0x08) return "Window";
        if (key & 0x10) return "Yequ";
        if (key & 0x20) return "Second";
        if (key & 0x40) return "Mode";
        if (key & 0x80) return "Del";
        break;
    case 1:
        if (key & 0x01) return "Sto";
        if (key & 0x02) return "Ln";
        if (key & 0x04) return "Log";
        if (key & 0x08) return "Square";
        if (key & 0x10) return "Recip";
        if (key & 0x20) return "Math";
        if (key & 0x40) return "Alpha";
        break;
    case 2:
        if (key & 0x01) return "Zero";
        if (key & 0x02) return "One";
        if (key & 0x04) return "Four";
        if (key & 0x08) return "Seven";
        if (key & 0x10) return "Comma";
        if (key & 0x20) return "Sin";
        if (key & 0x40) return "Apps";
        if (key & 0x80) return "GraphVar";
        break;
    case 3:
        if (key & 0x01) return "DecPnt";
        if (key & 0x02) return "Two";
        if (key & 0x04) return "Five";
        if (key & 0x08) return "Eight";
        if (key & 0x10) return "LParen";
        if (key & 0x20) return "Cos";
        if (key & 0x40) return "Prgm";
        if (key & 0x80) return "Stat";
        break;
    case 4:
        if (key & 0x01) return "Chs";
        if (key & 0x02) return "Three";
        if (key & 0x04) return "Six";
        if (key & 0x08) return "Nine";
        if (key & 0x10) return "RParen";
        if (key & 0x20) return "Tan";
        if (key & 0x40) return "Vars";
        break;
    case 5:
        if (key & 0x01) return "Enter";
        if (key & 0x02) return "Add";
        if (key & 0x04) return "Sub";
        if (key & 0x08) return "Mul";
        if (key & 0x10) return "Div";
        if (key & 0x20) return "Power";
        if (key & 0x40) return "Clear";
        break;
    case 6:
        if (key & 0x01) return "Down";
        if (key & 0x02) return "Left";
        if (key & 0x04) return "Right";
        if (key & 0x08) return "Up";
        break;
    }
    return "";
}

void queueKeyboardInput(INPUT* inputs, int& inCount, WORD vk, bool pressedNow) {
    inputs[inCount].type = INPUT_KEYBOARD;
    inputs[inCount].ki.wVk = vk;
    inputs[inCount].ki.dwFlags = pressedNow ? 0 : KEYEVENTF_KEYUP;
    ++inCount;
}

void drawKeyboardKeyName(const char* format, uint16_t keyCode) {
    char buf[64];
    if ((keyCode >= 0x30 && keyCode <= 0x39) || (keyCode >= 0x41 && keyCode <= 0x5A)) {
        std::snprintf(buf, sizeof(buf), "%c", keyCode);
    } else {
        std::snprintf(buf, sizeof(buf), "Key Code: 0x%X", keyCode);
    }
    ImGui::Text(format, buf);
}

uint16_t convertColor(uint32_t color) {
    return static_cast<uint16_t>(
        (static_cast<int>(std::round((color & 0xFF) * kColorMult)) << 10) +
        (static_cast<int>(std::round(((color >> 8) & 0xFF) * kColorMult)) << 5) +
        (static_cast<int>(std::round(((color >> 16) & 0xFF) * kColorMult)) << 0));
}

uint16_t convertRgbToCalcColor(int red, int green, int blue) {
    return static_cast<uint16_t>(
        (static_cast<int>(std::round(blue * kColorMult)) << 10) +
        (static_cast<int>(std::round(green * kColorMult)) << 5) +
        (static_cast<int>(std::round(red * kColorMult)) << 0));
}

void buildIndexedFrame(PIX* picture, std::array<uint8_t, kFrameBytes>& outFrame) {
    PIX* qPic = pixFixedOctcubeQuant256(picture, 0);
    if (!qPic) {
        std::fill(outFrame.begin(), outFrame.end(), 0);
        return;
    }

    PIX* rPic = pixRotate90(qPic, 1);

    uint32_t* picData = pixGetData(rPic);
    PIXCMAP* colorMap = pixGetColormap(rPic);
    const int colorCount = colorMap ? pixcmapGetCount(colorMap) : 0;

    for (int i = 0; i < 256; ++i) {
        int red = 0;
        int green = 0;
        int blue = 0;
        if (i < colorCount) {
            pixcmapGetColor(colorMap, i, &red, &green, &blue);
        }
        *reinterpret_cast<uint16_t*>(outFrame.data() + 2 * i) = convertRgbToCalcColor(red, green, blue);
    }

    for (size_t i = 0; i < static_cast<size_t>(kCalcWidth * kCalcHeight / 4); ++i) {
        *reinterpret_cast<uint32_t*>(outFrame.data() + kPaletteBytes + 4 * i) =
            ((picData[i] & 0xFF) << 24) |
            (((picData[i] >> 8) & 0xFF) << 16) |
            (((picData[i] >> 16) & 0xFF) << 8) |
            ((picData[i] >> 24) & 0xFF);
    }

    pixDestroy(&qPic);
    pixDestroy(&rPic);
}

bool sendAll(SOCKET socketHandle, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len && !gStop.load() && !gSessionStop.load()) {
        const int rc = send(socketHandle, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
        if (rc <= 0) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return sent == len;
}

bool recvAll(SOCKET socketHandle, uint8_t* data, size_t len) {
    size_t received = 0;
    while (received < len && !gStop.load() && !gSessionStop.load()) {
        const int rc = recv(socketHandle, reinterpret_cast<char*>(data + received), static_cast<int>(len - received), 0);
        if (rc <= 0) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                continue;
            }
            return false;
        }
        received += static_cast<size_t>(rc);
    }
    return received == len;
}

bool sendPacket(SOCKET socketHandle, bridge::MessageType type, const std::vector<uint8_t>& payload) {
    bridge::PacketHeader header{};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    const std::vector<uint8_t> headerBytes = bridge::serializeHeader(header);

    std::lock_guard<std::mutex> lock(gSendMutex);
    if (!sendAll(socketHandle, headerBytes.data(), headerBytes.size())) {
        return false;
    }
    if (!payload.empty() && !sendAll(socketHandle, payload.data(), payload.size())) {
        return false;
    }
    return true;
}

bool sendHello(SOCKET socketHandle) {
    bridge::HelloPayload hello{};
    return sendPacket(socketHandle, bridge::MessageType::Hello, bridge::serializeHello(hello));
}

bool isValidPayloadSize(bridge::MessageType type, uint32_t payloadSize) {
    switch (type) {
    case bridge::MessageType::Hello:
        return payloadSize == sizeof(bridge::HelloPayload);
    case bridge::MessageType::CalcKeys:
        return payloadSize == bridge::kCalcKeyBytes;
    case bridge::MessageType::FrameStart:
        return payloadSize == sizeof(bridge::FrameStartPayload);
    case bridge::MessageType::FrameChunk:
        return payloadSize >= sizeof(uint32_t) + sizeof(uint32_t) &&
            payloadSize <= sizeof(uint32_t) + sizeof(uint32_t) + kBridgeChunkBytes;
    case bridge::MessageType::FrameEnd:
        return payloadSize == sizeof(bridge::FrameEndPayload);
    case bridge::MessageType::Heartbeat:
        return payloadSize == 0;
    default:
        return false;
    }
}

void loadKeyBindings(const std::string& path) {
    std::lock_guard<std::mutex> lock(gBindingsMutex);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("No keybind file at %s, input forwarding will be idle until you add one.\n", path.c_str());
        return;
    }

    file.read(reinterpret_cast<char*>(gKeyBindings), sizeof(gKeyBindings));
    if (file.gcount() == sizeof(gKeyBindings)) {
        int savedMouseSpeed = gMouseSpeed;
        file.read(reinterpret_cast<char*>(&savedMouseSpeed), sizeof(savedMouseSpeed));
        if (file.gcount() == sizeof(savedMouseSpeed) && savedMouseSpeed >= 1 && savedMouseSpeed <= 100) {
            gMouseSpeed = savedMouseSpeed;
        }
        std::printf("Loaded key bindings from %s\n", path.c_str());
    }
    else {
        std::printf("Keybind file size did not match expected format: %s\n", path.c_str());
        std::memset(gKeyBindings, 0, sizeof(gKeyBindings));
    }
}

std::string resolveDefaultKeybindPath() {
    namespace fs = std::filesystem;

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    fs::path exeDir = len ? fs::path(modulePath).parent_path() : fs::current_path();

    const std::array<fs::path, 8> candidates = {
        exeDir / "keybinds",
        exeDir / "keybinds.cfg",
        exeDir.parent_path() / "keybinds",
        exeDir.parent_path() / "keybinds.cfg",
        exeDir.parent_path().parent_path() / "keybinds",
        exeDir.parent_path().parent_path() / "keybinds.cfg",
        exeDir.parent_path().parent_path().parent_path() / "keybinds",
        exeDir.parent_path().parent_path().parent_path() / "keybinds.cfg"
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }

    return "keybinds";
}

void sendMouseMove() {
    const uint8_t moveState = gMoveState.load();
    if (!moveState) {
        return;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dy = ((moveState & 0b0001) ? -gMouseSpeed : 0) + ((moveState & 0b0010) ? gMouseSpeed : 0);
    input.mi.dx = ((moveState & 0b0100) ? -gMouseSpeed : 0) + ((moveState & 0b1000) ? gMouseSpeed : 0);
    SendInput(1, &input, sizeof(INPUT));
}

bool isRepeatableVirtualKey(WORD vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
        return false;
    default:
        return true;
    }
}

void clearKeyRepeats() {
    std::lock_guard<std::mutex> lock(gKeyRepeatMutex);
    for (auto& state : gKeyRepeatStates) {
        state = {};
    }
}

void queueKeyboardTap(INPUT* inputs, int& inCount, WORD vk) {
    if (inCount + 2 > 128) {
        return;
    }

    inputs[inCount].type = INPUT_KEYBOARD;
    inputs[inCount].ki.wVk = vk;
    ++inCount;

    inputs[inCount].type = INPUT_KEYBOARD;
    inputs[inCount].ki.wVk = vk;
    inputs[inCount].ki.dwFlags = KEYEVENTF_KEYUP;
    ++inCount;
}

void keyRepeatLoop() {
    while (!gStop.load() && !gSessionStop.load()) {
        INPUT inputs[64] = {};
        int inCount = 0;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(gKeyRepeatMutex);
            for (auto& state : gKeyRepeatStates) {
                if (!state.active || !isRepeatableVirtualKey(state.vk) || now < state.nextRepeat) {
                    continue;
                }

                if (inCount + 2 > static_cast<int>(std::size(inputs))) {
                    break;
                }

                queueKeyboardTap(inputs, inCount, state.vk);
                state.nextRepeat = now + std::chrono::milliseconds(kKeyRepeatIntervalMs);
            }
        }

        if (inCount > 0) {
            SendInput(inCount, inputs, sizeof(INPUT));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kMouseTickMs));
    }
}

void mouseMoveLoop() {
    while (!gStop.load() && !gSessionStop.load()) {
        sendMouseMove();
        std::this_thread::sleep_for(std::chrono::milliseconds(kMouseTickMs));
    }
}

void handleCalcKeys(const uint8_t* recData) {
    INPUT inputs[128] = {};
    int inCount = 0;
    std::lock_guard<std::mutex> lock(gBindingsMutex);

    for (uint16_t i = 0; i < 128; ++i) {
        const uint32_t binding = gKeyBindings[i];

        const uint8_t dataIdx = static_cast<uint8_t>((i >> 4) & 7);
        const uint8_t keyShift = static_cast<uint8_t>(i & 0xF);
        if (dataIdx >= 7 || keyShift >= 8) {
            continue;
        }

        const uint8_t keyMask = static_cast<uint8_t>(1u << keyShift);
        const bool pressedNow = (recData[dataIdx] & keyMask) != 0;
        const bool pressedBefore = (gPrevCalcData[dataIdx] & keyMask) != 0;
        if (pressedNow == pressedBefore) {
            continue;
        }

        if (!binding) {
            if (pressedBefore) {
                std::lock_guard<std::mutex> repeatLock(gKeyRepeatMutex);
                gKeyRepeatStates[i] = {};
            }
            continue;
        }

        if (binding & 0x10000) {
            const WORD vk = static_cast<WORD>(binding & 0xFFFF);
            if (isRepeatableVirtualKey(vk)) {
                std::lock_guard<std::mutex> repeatLock(gKeyRepeatMutex);
                if (pressedNow) {
                    queueKeyboardTap(inputs, inCount, vk);
                    gKeyRepeatStates[i].active = true;
                    gKeyRepeatStates[i].vk = vk;
                    gKeyRepeatStates[i].nextRepeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKeyRepeatInitialDelayMs);
                } else {
                    gKeyRepeatStates[i] = {};
                }
            } else {
                queueKeyboardInput(inputs, inCount, vk, pressedNow);
            }
        }
        else {
            const uint8_t mouseAction = static_cast<uint8_t>(binding & 0xFF);
            if (mouseAction == kMouseMoveFlag) {
                const uint8_t dir = static_cast<uint8_t>((binding >> 8) & 0xFF);
                if (pressedNow) {
                    gMoveState.fetch_or(static_cast<uint8_t>(1u << dir));
                }
                else {
                    gMoveState.fetch_and(static_cast<uint8_t>(~(1u << dir)));
                }
            }
            else {
                inputs[inCount].type = INPUT_MOUSE;
                inputs[inCount].mi.dwFlags = pressedNow ? mouseAction : static_cast<DWORD>(mouseAction << 1);
                ++inCount;
            }
        }
    }

    for (int i = 0; i < 7; ++i) {
        const uint8_t diff = gPrevCalcData[i] & static_cast<uint8_t>(~(gPrevCalcData[i] & recData[i]));
        if (!diff) {
            continue;
        }
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (diff == static_cast<uint8_t>(1u << bit)) {
                gLastCalcKeyUp = static_cast<uint8_t>((i << 4) | bit);
                break;
            }
        }
    }

    if (inCount > 0) {
        SendInput(inCount, inputs, sizeof(INPUT));
    }

    std::memcpy(gPrevCalcData, recData, sizeof(gPrevCalcData));
}

void recvLoop(SOCKET clientSocket) {
    while (!gStop.load() && !gSessionStop.load()) {
        uint8_t headerBytes[kHeaderBytes] = { 0 };
        if (!recvAll(clientSocket, headerBytes, sizeof(headerBytes))) {
            break;
        }

        bridge::PacketHeader header{};
        if (!bridge::deserializeHeader(headerBytes, sizeof(headerBytes), header)) {
            std::printf("Invalid packet header from bridge client.\n");
            appendLog("Invalid packet header from bridge client");
            break;
        }

        if (!isValidPayloadSize(header.type, header.payloadSize)) {
            std::printf("Invalid payload size %u for message type %u from bridge client.\n",
                header.payloadSize, static_cast<unsigned>(header.type));
            appendLog("Invalid payload size from bridge client");
            break;
        }

        std::vector<uint8_t> payload;
        payload.resize(header.payloadSize);
        if (header.payloadSize > 0 && !recvAll(clientSocket, payload.data(), payload.size())) {
            break;
        }

        if (header.type == bridge::MessageType::CalcKeys) {
            bridge::CalcKeysPayload calcKeys{};
            if (bridge::deserializeCalcKeys(payload.data(), payload.size(), calcKeys)) {
                handleCalcKeys(calcKeys.keyMatrix.data());
            }
        }
    }

    gSessionStop.store(true);
}

void sendLoop(SOCKET clientSocket) {
    struct EncodedFrame {
        uint32_t frameId = 0;
        std::vector<uint8_t> payload;
    };

    std::mutex pendingMutex;
    std::condition_variable pendingCv;
    EncodedFrame pendingFrame;
    bool hasPendingFrame = false;
    bool producerDone = false;

    std::thread producer([&]() {
        ScreenCapture capture;
        if (!capture.initialize()) {
            std::printf("Failed to initialize Windows screen capture.\n");
            gSessionStop.store(true);
            pendingCv.notify_all();
            return;
        }

        std::vector<uint8_t> bgra(static_cast<size_t>(kCalcWidth * kCalcHeight * 4));
        std::vector<uint8_t> pixData(static_cast<size_t>(kCalcWidth * kCalcHeight * 4 + 1), 0);
        PIX* picture = pixCreateHeader(kCalcWidth, kCalcHeight, kBitsPerPixel);
        pixSetData(picture, reinterpret_cast<uint32_t*>(pixData.data()));
        std::array<uint8_t, kFrameBytes> frameBuffer{};
        std::array<uint8_t, kFrameBytes> lastBuiltFrame{};
        bool haveLastBuiltFrame = false;
        uint32_t nextFrameId = gFrameId;

        while (!gStop.load() && !gSessionStop.load()) {
            const auto frameStartTime = std::chrono::steady_clock::now();
            if (!capture.capture(bgra.data())) {
                std::printf("Screen capture failed.\n");
                gSessionStop.store(true);
                pendingCv.notify_all();
                break;
            }

            std::memcpy(pixData.data() + 1, bgra.data(), bgra.size());
            buildIndexedFrame(picture, frameBuffer);

            if (haveLastBuiltFrame && std::memcmp(frameBuffer.data(), lastBuiltFrame.data(), frameBuffer.size()) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kTargetFrameDelayMs));
                continue;
            }

            int outSize = 0;
            int sendLen = kFrameBytes;
            uint8_t* compBuf = compress(
                optimize(frameBuffer.data() + kPaletteBytes, kFrameBytes - kPaletteBytes, 0, 1, 25),
                frameBuffer.data() + kPaletteBytes,
                kFrameBytes - kPaletteBytes,
                0, 0, 1,
                &outSize);

            if (compBuf != nullptr) {
                if (outSize <= 60032) {
                    sendLen = outSize + kPaletteBytes;
                    if (sendLen % 64 != 0) {
                        sendLen += 64 - (sendLen % 64);
                    }

                    std::memcpy(frameBuffer.data() + kPaletteBytes, compBuf, static_cast<size_t>(outSize));
                    std::memset(frameBuffer.data() + kPaletteBytes + outSize, 0, static_cast<size_t>(sendLen - outSize - kPaletteBytes));
                }
                std::free(compBuf);
            }

            EncodedFrame nextFrame;
            nextFrame.frameId = nextFrameId++;
            nextFrame.payload.assign(frameBuffer.begin(), frameBuffer.begin() + sendLen);

            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                pendingFrame = std::move(nextFrame);
                hasPendingFrame = true;
            }
            pendingCv.notify_one();

            gFrameId = nextFrameId;
            lastBuiltFrame = frameBuffer;
            haveLastBuiltFrame = true;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - frameStartTime);
            gLatestFrameMs.store(static_cast<float>(elapsed.count()));
            gFrameTimes[gFrameTimesIndex++ % gFrameTimes.size()] = static_cast<float>(elapsed.count());
            if (elapsed.count() < kTargetFrameDelayMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kTargetFrameDelayMs - elapsed.count()));
            }
        }

        // `picture` only borrows the storage from `pixData`; clear the data
        // pointer before destroying the header to avoid freeing vector-owned memory.
        if (picture) {
            pixSetData(picture, nullptr);
        }
        pixDestroy(&picture);
        capture.shutdown();
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            producerDone = true;
        }
        pendingCv.notify_all();
    });

    while (!gStop.load() && !gSessionStop.load()) {
        EncodedFrame frameToSend;
        {
            std::unique_lock<std::mutex> lock(pendingMutex);
            pendingCv.wait(lock, [&]() {
                return hasPendingFrame || producerDone || gStop.load() || gSessionStop.load();
            });

            if (!hasPendingFrame) {
                if (producerDone || gStop.load() || gSessionStop.load()) {
                    break;
                }
                continue;
            }

            frameToSend = std::move(pendingFrame);
            hasPendingFrame = false;
        }

        bridge::FrameStartPayload start{};
        start.frameId = frameToSend.frameId;
        start.calcPayloadSize = static_cast<int32_t>(frameToSend.payload.size());
        if (!sendPacket(clientSocket, bridge::MessageType::FrameStart, bridge::serializeFrameStart(start))) {
            gSessionStop.store(true);
            pendingCv.notify_all();
            break;
        }

        for (std::size_t offset = 0; offset < frameToSend.payload.size(); offset += kBridgeChunkBytes) {
            bridge::FrameChunkPayload chunk{};
            chunk.frameId = frameToSend.frameId;
            chunk.offset = static_cast<uint32_t>(offset);
            const std::size_t chunkSize = std::min(kBridgeChunkBytes, frameToSend.payload.size() - offset);
            chunk.bytes.assign(frameToSend.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                frameToSend.payload.begin() + static_cast<std::ptrdiff_t>(offset + chunkSize));
            if (!sendPacket(clientSocket, bridge::MessageType::FrameChunk, bridge::serializeFrameChunk(chunk))) {
                gSessionStop.store(true);
                pendingCv.notify_all();
                break;
            }
        }

        if (gSessionStop.load()) {
            break;
        }

        bridge::FrameEndPayload end{};
        end.frameId = frameToSend.frameId;
        if (!sendPacket(clientSocket, bridge::MessageType::FrameEnd, bridge::serializeFrameEnd(end))) {
            gSessionStop.store(true);
            pendingCv.notify_all();
            break;
        }
    }

    if (producer.joinable()) {
        producer.join();
    }
}

void drawStats() {
    float sum = 0.0f;
    int count = 0;
    std::array<float, 64> fps{};
    for (int i = 0; i < 64; ++i) {
        const float t = gFrameTimes[i];
        if (t > 0.0f) {
            sum += t;
            fps[i] = 1000.0f / t;
            ++count;
        }
    }
    if (count == 0) {
        count = 1;
    }

    char overlay[128];
    std::snprintf(overlay, sizeof(overlay), "Average: %.2f ms\nLatest: %.2f ms", sum / count, gLatestFrameMs.load());
    ImGui::PlotLines("##frame_ms", gFrameTimes.data(), static_cast<int>(gFrameTimes.size()), 0, overlay, 0.0f, 350.0f, ImVec2(0, 120));

    std::snprintf(overlay, sizeof(overlay), "Average: %.2f fps\nLatest: %.2f fps", 1000.0f * count / sum, gLatestFrameMs.load() > 0.0f ? 1000.0f / gLatestFrameMs.load() : 0.0f);
    ImGui::PlotLines("##frame_fps", fps.data(), static_cast<int>(fps.size()), 0, overlay, 0.0f, 60.0f, ImVec2(0, 120));
}

void saveKeyBindingsDialog() {
    OPENFILENAMEA file{};
    char buf[260] = { 0 };
    file.lStructSize = sizeof(file);
    file.lpstrFile = buf;
    file.nMaxFile = sizeof(buf);
    file.lpstrTitle = "Save Keybindings";
    file.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&file)) {
        return;
    }

    std::lock_guard<std::mutex> lock(gBindingsMutex);
    FILE* saveFile = nullptr;
    fopen_s(&saveFile, file.lpstrFile, "wb");
    if (saveFile) {
        fwrite(gKeyBindings, sizeof(uint32_t), 128, saveFile);
        fwrite(&gMouseSpeed, sizeof(gMouseSpeed), 1, saveFile);
        fclose(saveFile);
    }
}

void loadKeyBindingsDialog() {
    OPENFILENAMEA file{};
    char buf[260] = { 0 };
    file.lStructSize = sizeof(file);
    file.lpstrFile = buf;
    file.nMaxFile = sizeof(buf);
    file.lpstrTitle = "Load Keybindings";
    file.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&file)) {
        return;
    }
    loadKeyBindings(file.lpstrFile);
}

void drawKeyBindingsUi() {
    if (!ImGui::BeginTable("keybind_table", 2)) {
        return;
    }

    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("Bindings");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    if (ImGui::Button("Bind Key")) {
        gLastCalcKeyUp = static_cast<uint8_t>(-1);
        gBindingKeyState = true;
        gBindingKey = static_cast<uint8_t>(-1);
        gNewBinding = 0;
    }

    if (gBindingKeyState) {
        if (gLastCalcKeyUp == static_cast<uint8_t>(-1)) {
            ImGui::Text("Press a calculator key to bind");
        } else {
            if (gBindingKey == static_cast<uint8_t>(-1)) {
                gBindingKey = gLastCalcKeyUp;
                std::lock_guard<std::mutex> lock(gBindingsMutex);
                if (gNewBinding == 0) {
                    gNewBinding = gKeyBindings[gBindingKey];
                }
                if (gNewBinding == 0) {
                    gNewBinding = 0x10000;
                }
            }
            if (gBindingKey != static_cast<uint8_t>(-1)) {
                ImGui::Text("Binding Key: %s", getCalcKeyName(gBindingKey));
            }
        }

        if (gBindingKey != static_cast<uint8_t>(-1)) {
            ImGui::Text("Input Type:");
            if (ImGui::RadioButton("Keyboard", (gNewBinding & 0x10000) != 0)) {
                gNewBinding = 0x10000;
            }
            if (ImGui::RadioButton("Mouse", (gNewBinding & 0x10000) == 0)) {
                gNewBinding = MOUSEEVENTF_LEFTDOWN;
            }

            if (gNewBinding & 0x10000) {
                if (!(gNewBinding & 0xFFFF)) {
                    ImGui::Text("Press a keyboard key to bind");
                } else {
                    drawKeyboardKeyName("Selected Key: %s", static_cast<uint16_t>(gNewBinding));
                }
            } else {
                if (ImGui::RadioButton("Move Up", gNewBinding == (kMouseMoveFlag | (0 << 8)))) gNewBinding = kMouseMoveFlag | (0 << 8);
                if (ImGui::RadioButton("Move Down", gNewBinding == (kMouseMoveFlag | (1 << 8)))) gNewBinding = kMouseMoveFlag | (1 << 8);
                if (ImGui::RadioButton("Move Left", gNewBinding == (kMouseMoveFlag | (2 << 8)))) gNewBinding = kMouseMoveFlag | (2 << 8);
                if (ImGui::RadioButton("Move Right", gNewBinding == (kMouseMoveFlag | (3 << 8)))) gNewBinding = kMouseMoveFlag | (3 << 8);
                if (ImGui::RadioButton("Left Click", gNewBinding == MOUSEEVENTF_LEFTDOWN)) gNewBinding = MOUSEEVENTF_LEFTDOWN;
                if (ImGui::RadioButton("Right Click", gNewBinding == MOUSEEVENTF_RIGHTDOWN)) gNewBinding = MOUSEEVENTF_RIGHTDOWN;
            }

            if (((gNewBinding & 0x10000) && (gNewBinding & 0xFFFF)) || !(gNewBinding & 0x10000)) {
                if (ImGui::Button("Apply Binding")) {
                    std::lock_guard<std::mutex> lock(gBindingsMutex);
                    gKeyBindings[gBindingKey] = gNewBinding;
                    gBindingKeyState = false;
                }
            }
        }

        if (ImGui::Button("Cancel")) {
            gBindingKeyState = false;
        }
    } else {
        if (ImGui::Button("Save Keybindings")) {
            saveKeyBindingsDialog();
        }
        if (ImGui::Button("Load Keybindings")) {
            loadKeyBindingsDialog();
        }
    }

    ImGui::SliderInt("Mouse Speed", &gMouseSpeed, 1, 100);

    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginTable("bindings_list", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Calc Key");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lock(gBindingsMutex);
        for (uint8_t i = 0; i < 128; ++i) {
            if (!gKeyBindings[i]) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", getCalcKeyName(i));
            ImGui::TableSetColumnIndex(1);
            if (gKeyBindings[i] & 0x10000) {
                ImGui::Text("Keyboard");
                ImGui::TableSetColumnIndex(2);
                drawKeyboardKeyName("%s", static_cast<uint16_t>(gKeyBindings[i]));
            } else {
                ImGui::Text("Mouse");
                ImGui::TableSetColumnIndex(2);
                const uint8_t action = static_cast<uint8_t>(gKeyBindings[i]);
                if (action == kMouseMoveFlag) {
                    switch ((gKeyBindings[i] >> 8) & 0xFF) {
                    case 0: ImGui::Text("Move Up"); break;
                    case 1: ImGui::Text("Move Down"); break;
                    case 2: ImGui::Text("Move Left"); break;
                    case 3: ImGui::Text("Move Right"); break;
                    default: ImGui::Text("Move"); break;
                    }
                } else if (action == MOUSEEVENTF_LEFTDOWN) {
                    ImGui::Text("Left Click");
                } else if (action == MOUSEEVENTF_RIGHTDOWN) {
                    ImGui::Text("Right Click");
                } else {
                    ImGui::Text("Mouse 0x%02X", action);
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndTable();
}

void drawRelayControlUi(uint16_t listenPort) {
    ImGui::InputText("Pi SSH Host", gPiHost.data(), gPiHost.size());
    ImGui::InputText("SSH User", gPiUser.data(), gPiUser.size());
    ImGui::InputText("SSH Key Path", gSshKeyPath.data(), gSshKeyPath.size());
    ImGui::InputText("Bridge Host IP", gBridgeHostIp.data(), gBridgeHostIp.size());
    ImGui::InputText("Pi Relay Path", gPiRelayPath.data(), gPiRelayPath.size());
    ImGui::InputText("Pi Relay Log", gPiRelayLogPath.data(), gPiRelayLogPath.size());
    ImGui::Text("Bridge Port: %u", listenPort);
    ImGui::Checkbox("Manage virtual display with app", &gAutoManageVirtualDisplay);
    ImGui::Checkbox("Make virtual display primary while app runs", &gMakeVirtualPrimaryWhileRunning);

    const bool actionRunning = gRelayActionRunning.load();
    if (actionRunning) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Start Pi Relay")) {
        launchRelayAction(true, listenPort);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Pi Relay")) {
        launchRelayAction(false, listenPort);
    }
    if (actionRunning) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("Working...");
    }

    ImGui::Checkbox("Auto-scroll status", &gRelayAutoScroll);

    std::string statusCopy;
    {
        std::lock_guard<std::mutex> lock(gRelayUiMutex);
        statusCopy = gRelayStatusLog;
    }

    ImGui::BeginChild("Relay Status", ImVec2(0, 180), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(statusCopy.c_str());
    if (gRelayAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (::ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            gResizeWidth = static_cast<UINT>(LOWORD(lParam));
            gResizeHeight = static_cast<UINT>(HIWORD(lParam));
        }
        return 0;
    case WM_KEYUP:
        if (gBindingKeyState && (gNewBinding & 0x10000)) {
            gNewBinding = 0x10000 | static_cast<uint16_t>(wParam);
        }
        break;
    case WM_CLOSE:
        restoreOriginalPrimaryDisplay();
        gDone = true;
        gStop.store(true);
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool parsePort(const char* value, uint16_t& outPort) {
    if (!value) return false;
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0 || parsed > 65535) return false;
    outPort = static_cast<uint16_t>(parsed);
    return true;
}

void initializeRelayDefaults() {
    std::snprintf(gPiHost.data(), gPiHost.size(), "%s", "");
    std::snprintf(gBridgeHostIp.data(), gBridgeHostIp.size(), "%s", "");
    std::snprintf(gPiRelayPath.data(), gPiRelayPath.size(), "%s", "/usr/local/bin/Calc2KeyPiRelay");
    std::snprintf(gPiRelayLogPath.data(), gPiRelayLogPath.size(), "%s", "~/calc2key-bridge.log");

    const char* sshUser = std::getenv("CALC2KEY_PI_USER");
    if (!sshUser || !*sshUser) {
        sshUser = std::getenv("USERNAME");
    }
    if (sshUser && *sshUser) {
        std::snprintf(gPiUser.data(), gPiUser.size(), "%s", sshUser);
    }

    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        std::filesystem::path keyPath = std::filesystem::path(userProfile) / ".ssh" / "calc2key_pi_temp";
        const std::string keyPathString = keyPath.string();
        std::snprintf(gSshKeyPath.data(), gSshKeyPath.size(), "%s", keyPathString.c_str());
    }
    initializeVddDefaults(userProfile);
}

void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        auto setContext = reinterpret_cast<SetDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) {
            const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(-4);
            if (setContext(perMonitorV2)) {
                return;
            }
        }
    }

    SetProcessDPIAware();
}

int runBridgeHost(uint16_t listenPort) {
    gBridgeState.store(1);
    appendLog("runBridgeHost starting on port " + std::to_string(listenPort));

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::printf("WSAStartup failed.\n");
        appendLog("WSAStartup failed");
        gBridgeState.store(-1);
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::printf("Failed to create listen socket.\n");
        appendLog("socket() failed");
        WSACleanup();
        gBridgeState.store(-1);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listenPort);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::printf("Bind failed on port %u.\n", listenPort);
        appendLog("bind() failed on port " + std::to_string(listenPort) + " error=" + std::to_string(WSAGetLastError()));
        closesocket(listenSocket);
        WSACleanup();
        gBridgeState.store(-1);
        return 1;
    }

    if (listen(listenSocket, 1) == SOCKET_ERROR) {
        std::printf("Listen failed.\n");
        appendLog("listen() failed error=" + std::to_string(WSAGetLastError()));
        closesocket(listenSocket);
        WSACleanup();
        gBridgeState.store(-1);
        return 1;
    }

    std::printf("Calc2KeyCE Bridge Host listening on TCP port %u\n", listenPort);
    std::printf("Waiting for Pi bridge connection...\n");
    appendLog("listening on port " + std::to_string(listenPort));

    while (!gStop.load()) {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            std::printf("Accept failed.\n");
            appendLog("accept() failed error=" + std::to_string(WSAGetLastError()));
            break;
        }

        DWORD timeoutMs = 250;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        const int tcpNoDelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&tcpNoDelay), sizeof(tcpNoDelay));

        gSessionStop.store(false);
        gMoveState.store(0);
        clearKeyRepeats();
        std::memset(gPrevCalcData, 0, sizeof(gPrevCalcData));
        gBridgeState.store(3);

        std::printf("Bridge client connected.\n");
        appendLog("bridge client connected");
        if (!sendHello(clientSocket)) {
            std::printf("Failed to send hello to bridge client.\n");
            appendLog("sendHello failed");
            shutdown(clientSocket, SD_BOTH);
            closesocket(clientSocket);
            gBridgeState.store(1);
            continue;
        }

        std::thread recvThread(recvLoop, clientSocket);
        std::thread sendThread(sendLoop, clientSocket);
        std::thread mouseThread(mouseMoveLoop);
        std::thread keyRepeatThread(keyRepeatLoop);

        recvThread.join();
        gSessionStop.store(true);
        shutdown(clientSocket, SD_BOTH);
        sendThread.join();
        mouseThread.join();
        keyRepeatThread.join();
        clearKeyRepeats();

        closesocket(clientSocket);
        std::printf("Bridge client disconnected, waiting for reconnect...\n");
        appendLog("bridge client disconnected");
        gBridgeState.store(1);
    }

    closesocket(listenSocket);
    WSACleanup();
    std::printf("Bridge host stopped.\n");
    appendLog("bridge host stopped");
    gBridgeState.store(0);
    return 0;
}

} // namespace

int appMain(int argc, char* argv[]) {
    if (HWND consoleWindow = GetConsoleWindow()) {
        ShowWindow(consoleWindow, SW_HIDE);
    }

    {
        std::ofstream truncateLog("bridge_host.log", std::ios::trunc);
    }
    appendLog("main starting");
    uint16_t listenPort = kDefaultPort;
    std::string keybindPath = resolveDefaultKeybindPath();
    bool restorePrimaryHelperMode = false;
    DWORD restoreParentPid = 0;
    std::wstring restorePrimaryDevice;
    std::wstring exePath = std::filesystem::absolute(std::filesystem::path(argv[0])).wstring();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--restore-primary-helper") {
            restorePrimaryHelperMode = true;
        }
        else if (arg == "--parent-pid" && i + 1 < argc) {
            restoreParentPid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (arg == "--primary-device" && i + 1 < argc) {
            restorePrimaryDevice = narrowArgToWide(argv[++i]);
        }
        else if (arg == "--port" && i + 1 < argc) {
            parsePort(argv[++i], listenPort);
        }
        else if (arg == "--keybinds" && i + 1 < argc) {
            keybindPath = argv[++i];
        }
    }

    if (restorePrimaryHelperMode) {
        if (!restorePrimaryDevice.empty() && restoreParentPid != 0) {
            HANDLE parentHandle = OpenProcess(SYNCHRONIZE, FALSE, restoreParentPid);
            if (parentHandle) {
                WaitForSingleObject(parentHandle, INFINITE);
                CloseHandle(parentHandle);
            }
            applyPrimaryDisplayDevice(restorePrimaryDevice);
        }
        return 0;
    }

    loadKeyBindings(keybindPath);
    initializeRelayDefaults();
    enableDpiAwareness();
    ensureVirtualDisplayEnabledForApp();
    normalizePrimaryDisplayForAppStartup();
    {
        const auto displays = enumerateDisplayInfos();
        const auto nonVirtualIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
            return !display.isVirtual;
        });
        if (nonVirtualIt != displays.end()) {
            gPreferredHostWindowDisplayDevice = nonVirtualIt->deviceName;
        } else {
            const auto primaryIt = std::find_if(displays.begin(), displays.end(), [](const DisplayInfo& display) {
                return display.isPrimary;
            });
            if (primaryIt != displays.end()) {
                gPreferredHostWindowDisplayDevice = primaryIt->deviceName;
            }
        }
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Calc2KeyCEBridgeHostWin", nullptr };
    RegisterClassExW(&wc);
    const RECT initialWindowRect = getInitialHostWindowRect(960, 720);
    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Calc2KeyCE Bridge Host",
        WS_OVERLAPPEDWINDOW,
        initialWindowRect.left,
        initialWindowRect.top,
        initialWindowRect.right - initialWindowRect.left,
        initialWindowRect.bottom - initialWindowRect.top,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
    gMainWindow = hwnd;
    moveHostWindowToDisplay(hwnd, gPreferredHostWindowDisplayDevice);

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gPd3dDevice, gPd3dDeviceContext);

    std::thread bridgeThread([listenPort]() {
        runBridgeHost(listenPort);
    });
    bridgeThread.detach();

    int lastBridgeState = gBridgeState.load();
    auto virtualPrimaryReadyAt = std::chrono::steady_clock::time_point::max();
    if (lastBridgeState == 3) {
        virtualPrimaryReadyAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    }

    while (!gDone) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                gDone = true;
                gStop.store(true);
            }
        }
        if (gDone) {
            break;
        }

        const int currentBridgeState = gBridgeState.load();
        if (currentBridgeState != lastBridgeState) {
            if (currentBridgeState == 3) {
                virtualPrimaryReadyAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            } else {
                virtualPrimaryReadyAt = std::chrono::steady_clock::time_point::max();
                restoreOriginalPrimaryDisplay();
                moveHostWindowToDisplay(hwnd, gPreferredHostWindowDisplayDevice);
            }
            lastBridgeState = currentBridgeState;
        }

        if (currentBridgeState == 3 && std::chrono::steady_clock::now() >= virtualPrimaryReadyAt) {
            virtualPrimaryReadyAt = std::chrono::steady_clock::time_point::max();
            switchVirtualDisplayToPrimaryForApp(exePath);
            moveHostWindowToDisplay(hwnd, gPreferredHostWindowDisplayDevice);
        }

        if (gResizeWidth != 0 && gResizeHeight != 0) {
            cleanupRenderTarget();
            gPSwapChain->ResizeBuffers(0, gResizeWidth, gResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            gResizeWidth = gResizeHeight = 0;
            createRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags mainWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("MainContent", nullptr, mainWindowFlags);
        ImGui::PopStyleVar(2);

        ImGui::TextUnformatted("Calc2KeyCE Bridge Host");
        ImGui::Separator();
        const int bridgeState = gBridgeState.load();
        if (bridgeState == 3) {
            ImGui::Text("Bridge client connected.");
        } else if (bridgeState == 1) {
            ImGui::Text("Waiting for Raspberry Pi bridge connection...");
        } else if (bridgeState == 0) {
            ImGui::Text("Bridge host is starting...");
        } else {
            ImGui::Text("Bridge host state: %d", bridgeState);
        }

        if (ImGui::TreeNode("Relay Control")) {
            drawRelayControlUi(listenPort);
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Virtual Driver Control")) {
            drawVirtualDriverControlUi();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Stats")) {
            drawStats();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Keybindings")) {
            drawKeyBindingsUi();
            ImGui::TreePop();
        }

        ImGui::End();

        ImGui::Render();
        const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
        gPd3dDeviceContext->OMSetRenderTargets(1, &gMainRenderTargetView, nullptr);
        gPd3dDeviceContext->ClearRenderTargetView(gMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gPSwapChain->Present(1, 0);
    }

    gStop.store(true);
    restoreOriginalPrimaryDisplay();
    disableVirtualDisplayForAppExit();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

int main(int argc, char* argv[]) {
    return appMain(argc, argv);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return appMain(__argc, __argv);
}
