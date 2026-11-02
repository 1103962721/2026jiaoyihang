#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_MSC_VER) && (!defined(_MSVC_LANG) || _MSVC_LANG < 201703L)
#error UimOcr.cpp requires ISO C++17 (/std:c++17) for C++/WinRT JSON support.
#endif
#include "UimOcr.h"

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <roapi.h>

#include <atomic>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

namespace
{
constexpr int DefaultPort = 1224;
constexpr DWORD HttpTimeoutMs = 5000;
std::atomic<int> g_port{ 0 };
std::mutex g_initMutex;
std::mutex g_requestMutex;

struct GdiPlusSession
{
    ULONG_PTR token = 0;
    GdiPlusSession()
    {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiPlusSession() { if (token) Gdiplus::GdiplusShutdown(token); }
};

struct RuntimeSession
{
    HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
    ~RuntimeSession() { if (SUCCEEDED(result)) RoUninitialize(); }
    bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

std::wstring ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& file)
{
    return directory + L'\\' + file;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring OcrExecutable(const std::wstring& directory, const std::wstring& iniPath)
{
    wchar_t configured[32768]{};
    GetPrivateProfileStringW(L"OCR", L"UmiOcrPath", L"", configured,
        static_cast<DWORD>(std::size(configured)), iniPath.c_str());
    if (*configured)
    {
        std::wstring path(configured);
        if (path.size() < 2 || path[1] != L':') path = JoinPath(directory, path);
        if (FileExists(path)) return path;
    }
    for (const wchar_t* candidate : {
             L"Umi-OCR.exe", L"Umi-OCR_Paddle_v2.1.5\\Umi-OCR.exe", L"Umi-OCR\\Umi-OCR.exe" })
    {
        const std::wstring path = JoinPath(directory, candidate);
        if (FileExists(path)) return path;
    }
    return {};
}

bool StartOcr(const std::wstring& path)
{
    const size_t separator = path.find_last_of(L"\\/");
    const std::wstring directory = path.substr(0, separator);
    std::wstring command = L'"' + path + L'"';
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), &command[0], nullptr, nullptr, FALSE, 0,
            nullptr, directory.c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool PostOcr(int port, const std::string& body, std::string& response)
{
    response.clear();
    if (body.size() > std::numeric_limits<DWORD>::max()) return false;
    HINTERNET session = WinHttpOpen(L"DF_PlayClick_UimOcr/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, HttpTimeoutMs, HttpTimeoutMs, HttpTimeoutMs, HttpTimeoutMs);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    HINTERNET request = connection
        ? WinHttpOpenRequest(connection, L"POST", L"/api/ocr", nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
        : nullptr;
    bool ok = false;
    if (request)
    {
        const DWORD length = static_cast<DWORD>(body.size());
        ok = WinHttpSendRequest(request, L"Content-Type: application/json\r\n", -1,
                const_cast<char*>(body.data()), length, length, 0)
            && WinHttpReceiveResponse(request, nullptr);
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        ok = ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)
            && status == 200;
        while (ok)
        {
            char chunk[4096];
            DWORD received = 0;
            if (!WinHttpReadData(request, chunk, sizeof(chunk), &received)) { ok = false; break; }
            if (!received) break;
            response.append(chunk, received);
            if (response.size() > 16 * 1024 * 1024) { ok = false; break; }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool ParseResponse(const std::string& response, winrt::Windows::Data::Json::JsonObject& json)
{
    try
    {
        json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(response));
        return json.HasKey(L"code") && json.HasKey(L"data")
            && json.GetNamedValue(L"code").ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Number;
    }
    catch (const winrt::hresult_error&) { return false; }
}

std::string RequestBody(const std::string& base64)
{
    return "{\"base64\":\"" + base64 + "\",\"options\":{"
        "\"ocr.angle\":false,\"ocr.language\":\"models/config_chinese.txt\","
        "\"ocr.maxSideLen\":2048,\"ocr.cpuThreads\":12,"
        "\"tbpu.parser\":\"single_line\",\"data.format\":\"json\"}}";
}

bool ProbeOcr(int port)
{
    RuntimeSession runtime;
    if (!runtime.ready()) return false;
    // A valid 1x1 PNG distinguishes Umi-OCR from another listener on the same port.
    constexpr char BlankPng[] =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/kZkAAAAASUVORK5CYII=";
    std::string response;
    if (!PostOcr(port, RequestBody(BlankPng), response)) return false;
    winrt::Windows::Data::Json::JsonObject json;
    return ParseResponse(response, json);
}

bool PngEncoder(CLSID& id)
{
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !count || !bytes)
        return false;
    std::vector<ULONG_PTR> storage((bytes + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT index = 0; index < count; ++index)
    {
        if (wcscmp(encoders[index].MimeType, L"image/png") == 0)
        {
            id = encoders[index].Clsid;
            return true;
        }
    }
    return false;
}

bool CaptureBase64(HWND hwnd, int x, int y, int width, int height, std::string& base64)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd) || x < 0 || y < 0 || width <= 0 || height <= 0)
        return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client) || x > client.right || y > client.bottom
        || width > client.right - x || height > client.bottom - y)
        return false;
    POINT screenPoint{ x, y };
    if (!ClientToScreen(hwnd, &screenPoint)) return false;

    static const GdiPlusSession gdiplus;
    if (!gdiplus.token) return false;
    HDC desktop = GetDC(nullptr);
    if (!desktop) return false;
    HDC memory = CreateCompatibleDC(desktop);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(desktop, width, height) : nullptr;
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool copied = previous && BitBlt(memory, 0, 0, width, height,
        desktop, screenPoint.x, screenPoint.y, SRCCOPY | CAPTUREBLT);
    if (previous) SelectObject(memory, previous);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, desktop);
    if (!copied) { if (bitmap) DeleteObject(bitmap); return false; }

    CLSID encoder{};
    IStream* stream = nullptr;
    bool ok = PngEncoder(encoder) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    if (ok)
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        ok = image.Save(stream, &encoder) == Gdiplus::Ok;
    }
    DeleteObject(bitmap);
    if (ok)
    {
        STATSTG stat{};
        HGLOBAL global = nullptr;
        ok = SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))
            && stat.cbSize.QuadPart <= std::numeric_limits<DWORD>::max()
            && SUCCEEDED(GetHGlobalFromStream(stream, &global));
        if (ok)
        {
            const void* bytes = GlobalLock(global);
            const DWORD size = static_cast<DWORD>(stat.cbSize.QuadPart);
            DWORD encodedSize = 0;
            ok = bytes && CryptBinaryToStringA(static_cast<const BYTE*>(bytes), size,
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encodedSize);
            if (ok)
            {
                base64.resize(encodedSize);
                ok = CryptBinaryToStringA(static_cast<const BYTE*>(bytes), size,
                    CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &base64[0], &encodedSize) != FALSE;
                if (ok)
                {
                    base64.resize(encodedSize);
                    if (!base64.empty() && base64.back() == '\0') base64.pop_back();
                }
            }
            if (bytes) GlobalUnlock(global);
        }
    }
    if (stream) stream->Release();
    return ok;
}

bool ReadBlocks(const winrt::Windows::Data::Json::JsonObject& json,
    int offsetX, int offsetY, UimType& result)
{
    using namespace winrt::Windows::Data::Json;
    try
    {
        const JsonArray blocks = json.GetNamedArray(L"data");
        for (const IJsonValue& value : blocks)
        {
            const JsonObject block = value.GetObject();
            const JsonArray box = block.GetNamedArray(L"box");
            if (box.Size() < 4) return false;
            UimTextBlock entry;
            entry.textUtf8 = winrt::to_string(block.GetNamedString(L"text"));
            int minX = std::numeric_limits<int>::max();
            int minY = std::numeric_limits<int>::max();
            int maxX = std::numeric_limits<int>::min();
            int maxY = std::numeric_limits<int>::min();
            for (unsigned index = 0; index < 4; ++index)
            {
                const JsonArray point = box.GetAt(index).GetArray();
                if (point.Size() < 2) return false;
                const int px = static_cast<int>(point.GetAt(0).GetNumber()) + offsetX;
                const int py = static_cast<int>(point.GetAt(1).GetNumber()) + offsetY;
                entry.corners[index] = POINT{ px, py };
                minX = std::min(minX, px); minY = std::min(minY, py);
                maxX = std::max(maxX, px); maxY = std::max(maxY, py);
            }
            entry.bounds = RECT{ minX, minY, maxX + 1, maxY + 1 };
            if (!result.textUtf8.empty()) result.textUtf8 += '\n';
            result.textUtf8 += entry.textUtf8;
            result.blocks.push_back(std::move(entry));
        }
        return true;
    }
    catch (const winrt::hresult_error&) { return false; }
}
} // namespace

BOOL OnUimOcr()
{
    std::lock_guard<std::mutex> lock(g_initMutex);
    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return FALSE; }
    const std::wstring ini = JoinPath(directory, L"DF_PlayClick_Client.ini");
    const int port = GetPrivateProfileIntW(L"OCR", L"Port", DefaultPort, ini.c_str());
    if (port < 1 || port > 65535) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    {
        std::lock_guard<std::mutex> requestLock(g_requestMutex);
        if (ProbeOcr(port)) { g_port.store(port); return TRUE; }
    }
    const std::wstring executable = OcrExecutable(directory, ini);
    if (executable.empty()) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (!StartOcr(executable)) return FALSE;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Sleep(250);
        std::lock_guard<std::mutex> requestLock(g_requestMutex);
        if (ProbeOcr(port)) { g_port.store(port); return TRUE; }
    }
    SetLastError(WAIT_TIMEOUT);
    return FALSE;
}

PCHAR UimOcrReadText(HWND hwnd, int pointX, int pointY, int width, int height, UimType* result)
{
    if (!result) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    const ULONGLONG started = GetTickCount64();
    *result = UimType{};
    const auto fail = [&](const char* message) -> PCHAR {
        result->textUtf8.clear();
        result->blocks.clear();
        result->errorUtf8 = message;
        result->elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        return nullptr;
    };
    const int port = g_port.load();
    if (!port) { SetLastError(ERROR_NOT_READY); return fail("Call OnUimOcr first"); }
    std::string base64;
    if (!CaptureBase64(hwnd, pointX, pointY, width, height, base64))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return fail("Window capture failed or region is outside the client area");
    }
    RuntimeSession runtime;
    if (!runtime.ready()) return fail("Windows Runtime initialization failed");
    std::string response;
    winrt::Windows::Data::Json::JsonObject json;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        if (!PostOcr(port, RequestBody(base64), response))
            return fail("Umi-OCR HTTP request failed");
        if (!ParseResponse(response, json))
            return fail("Umi-OCR returned invalid JSON");
    }
    try
    {
        const int code = static_cast<int>(json.GetNamedNumber(L"code"));
        if (code != 100)
        {
            const std::string detail = winrt::to_string(json.GetNamedString(L"data", L""));
            if (detail.find("No text found in image") == std::string::npos)
            {
                result->errorUtf8 = detail.empty() ? "Umi-OCR recognition failed" : detail;
                result->elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
                return nullptr;
            }
        }
        else if (!ReadBlocks(json, pointX, pointY, *result))
            return fail("Umi-OCR text blocks or coordinates are invalid");
    }
    catch (const winrt::hresult_error&) { return fail("Umi-OCR response fields are invalid"); }
    result->elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
    return result->textUtf8.data();
}

BOOL HwdnClick(HWND hwnd, int x, int y)
{
    RECT client{};
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd) || !GetClientRect(hwnd, &client))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (x < 0 || y < 0 || x >= client.right || y >= client.bottom)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    POINT point{ x, y };
    if (!ClientToScreen(hwnd, &point))
        return FALSE;

    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int desktopWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int desktopHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (desktopWidth <= 1 || desktopHeight <= 1
        || point.x < originX || point.y < originY
        || point.x - originX >= desktopWidth || point.y - originY >= desktopHeight)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const HWND topLevel = GetAncestor(hwnd, GA_ROOT);
    SetForegroundWindow(topLevel ? topLevel : hwnd);
    const HWND atPoint = WindowFromPoint(point);
    if (!atPoint || GetAncestor(atPoint, GA_ROOT) != (topLevel ? topLevel : hwnd))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    INPUT inputs[3]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[0].mi.dx = MulDiv(point.x - originX, 65535, desktopWidth - 1);
    inputs[0].mi.dy = MulDiv(point.y - originY, 65535, desktopHeight - 1);
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(3, inputs, sizeof(INPUT));
    if (sent != 3)
    {
        const DWORD error = GetLastError();
        if (sent == 2)
        {
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &release, sizeof(INPUT));
        }
        SetLastError(error ? error : ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}
