#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "ClientBindingRules.h"
#include "ClientCountdownRules.h"
#include "../Shared/Protocol.h"
#include "../Shared/SocketUtil.h"
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace
{
    constexpr int IDC_IP_EDIT = 2001;
    constexpr int IDC_PORT_EDIT = 2002;
    constexpr int IDC_CONNECT_BUTTON = 2003;
    constexpr int IDC_REFRESH_WINDOWS_BUTTON = 2004;
    constexpr int IDC_WINDOW_LIST = 2005;
    constexpr int IDC_BOUND_TEXT = 2006;
    constexpr int IDC_X_EDIT = 2007;
    constexpr int IDC_Y_EDIT = 2008;
    constexpr int IDC_PICK_BUTTON = 2009;
    constexpr int IDC_AUTO_POINT_BUTTON = 2010;
    constexpr int IDC_READY_BUTTON = 2011;
    constexpr int IDC_COUNTDOWN_TEXT = 2012;
    constexpr int IDC_STATUS_TEXT = 2013;

    constexpr UINT WM_SOCKET_DISCONNECTED = WM_APP + 10;
    constexpr UINT WM_TRIGGER_RECEIVED = WM_APP + 11;
    constexpr int PICK_HOTKEY_ID = 3001;
    constexpr UINT PICK_HOTKEY_MODIFIERS = MOD_CONTROL | MOD_SHIFT;
    constexpr UINT PICK_HOTKEY_KEY = 'P';
    constexpr UINT TIMER_COUNTDOWN = 1;
    constexpr UINT TIMER_TOPMOST_RESTORE = 2;
    constexpr DWORD CLICK_QUEUE_WAIT_MS = 10000;
    constexpr DWORD CLICK_QUEUE_RELEASE_DELAY_MS = 220;
    const wchar_t LOCAL_CLICK_QUEUE_MUTEX[] = L"Local\\DF_PlayClick_Client_ClickQueue";

    struct LocalClickGate
    {
        HANDLE mutex = nullptr;
        bool owns = false;

        ~LocalClickGate()
        {
            if (mutex)
            {
                if (owns)
                {
                    ReleaseMutex(mutex);
                }
                CloseHandle(mutex);
            }
        }
    };

    struct WindowItem
    {
        HWND hwnd = nullptr;
        std::wstring title;
        std::wstring className;
        DWORD processId = 0;
        std::wstring processPath;
        std::wstring processName;
        int clientWidth = 0;
        int clientHeight = 0;
        int windowWidth = 0;
        int windowHeight = 0;
    };

    HINSTANCE g_instance = nullptr;
    HWND g_mainWindow = nullptr;
    HWND g_ipEdit = nullptr;
    HWND g_portEdit = nullptr;
    HWND g_connectButton = nullptr;
    HWND g_windowList = nullptr;
    HWND g_boundText = nullptr;
    HWND g_xEdit = nullptr;
    HWND g_yEdit = nullptr;
    HWND g_pickButton = nullptr;
    HWND g_autoPointButton = nullptr;
    HWND g_readyButton = nullptr;
    HWND g_countdownText = nullptr;
    HWND g_statusText = nullptr;

    SocketUtil::WsaSession g_wsa;
    SOCKET g_socket = INVALID_SOCKET;
    std::thread g_receiveThread;
    std::atomic_bool g_connected = false;
    bool g_ready = false;
    HWND g_boundHwnd = nullptr;
    std::vector<WindowItem> g_windows;
    bool g_pickingPoint = false;
    int g_countdown = 0;

    std::wstring GetConfigPath()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring configPath = path;
        size_t slash = configPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return L"DF_PlayClick_Client.ini";
        }
        return configPath.substr(0, slash + 1) + L"DF_PlayClick_Client.ini";
    }

    std::wstring LoadLastIp()
    {
        wchar_t buffer[128]{};
        GetPrivateProfileStringW(L"Connection", L"LastIp", L"127.0.0.1", buffer, 128, GetConfigPath().c_str());
        return buffer;
    }

    void SaveLastIp(const wchar_t* ipText)
    {
        WritePrivateProfileStringW(L"Connection", L"LastIp", ipText, GetConfigPath().c_str());
    }

    void SetStatus(const std::wstring& text)
    {
        SetWindowTextW(g_statusText, text.c_str());
    }

    std::wstring HwndToText(HWND hwnd)
    {
        return FormatHwndLow8Hex(hwnd);
    }

    int ReadEditInt(HWND edit)
    {
        wchar_t buffer[32]{};
        GetWindowTextW(edit, buffer, 32);
        return _wtoi(buffer);
    }

    void SetReady(bool ready)
    {
        g_ready = ready;
        SetWindowTextW(g_readyButton, ready ? L"取消准备" : L"准备");
        if (g_connected)
        {
            SocketUtil::SendLine(g_socket, Protocol::BuildReadyMessage(ready));
        }
    }

    void Disconnect()
    {
        if (g_socket != INVALID_SOCKET)
        {
            shutdown(g_socket, SD_BOTH);
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
        }
        g_connected = false;
        SetWindowTextW(g_connectButton, L"连接");
        SetReady(false);
        SetStatus(L"未连接");
    }

    std::wstring GetFileNameFromPath(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return path;
        }
        return path.substr(slash + 1);
    }

    bool IsRunningAsAdministrator()
    {
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        PSID adminGroup = nullptr;
        BOOL isMember = FALSE;
        if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup))
        {
            return false;
        }

        // 启动阶段只检查当前进程令牌是否属于管理员组，不尝试提升权限。
        BOOL result = CheckTokenMembership(nullptr, adminGroup, &isMember);
        FreeSid(adminGroup);
        return result && isMember;
    }

    bool IsDesktopScale100Percent()
    {
        HDC dc = GetDC(nullptr);
        if (!dc)
        {
            return false;
        }

        // 96 DPI 对应 Windows 桌面缩放 100%，非 100% 会影响手动与自动坐标一致性。
        int dpiX = GetDeviceCaps(dc, LOGPIXELSX);
        int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        return dpiX == 96 && dpiY == 96;
    }

    std::wstring GetWindowTitle(HWND hwnd)
    {
        int length = GetWindowTextLengthW(hwnd);
        if (length <= 0)
        {
            return L"";
        }

        std::vector<wchar_t> titleBuffer(static_cast<size_t>(length) + 1);
        GetWindowTextW(hwnd, titleBuffer.data(), static_cast<int>(titleBuffer.size()));
        return titleBuffer.data();
    }

    std::wstring GetWindowClassName(HWND hwnd)
    {
        wchar_t buffer[256]{};
        GetClassNameW(hwnd, buffer, 256);
        return buffer;
    }

    std::wstring GetProcessPath(DWORD processId)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
        {
            return L"";
        }

        wchar_t buffer[MAX_PATH]{};
        DWORD size = MAX_PATH;
        std::wstring path;
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        {
            path.assign(buffer, size);
        }
        CloseHandle(process);
        return path;
    }

    WindowItem BuildWindowItem(HWND hwnd)
    {
        WindowItem item{};
        item.hwnd = hwnd;
        item.title = GetWindowTitle(hwnd);
        item.className = GetWindowClassName(hwnd);
        GetWindowThreadProcessId(hwnd, &item.processId);
        item.processPath = GetProcessPath(item.processId);
        item.processName = GetFileNameFromPath(item.processPath);

        RECT clientRect{};
        if (GetClientRect(hwnd, &clientRect))
        {
            item.clientWidth = clientRect.right - clientRect.left;
            item.clientHeight = clientRect.bottom - clientRect.top;
        }

        RECT windowRect{};
        if (GetWindowRect(hwnd, &windowRect))
        {
            item.windowWidth = windowRect.right - windowRect.left;
            item.windowHeight = windowRect.bottom - windowRect.top;
        }

        return item;
    }

    bool IsDeltaForceGameBodyCandidate(const WindowItem& item)
    {
        // 游戏启动器可能和本体同标题，必须用本体进程名进一步确认。
        return IsDeltaForceGameBodyTitle(item.title.c_str())
            && IsDeltaForceGameBodyProcessName(item.processName.c_str());
    }

    const WindowItem* FindDeltaForceGameBodyWindow()
    {
        for (const auto& item : g_windows)
        {
            if (IsDeltaForceGameBodyCandidate(item))
            {
                return &item;
            }
        }
        return nullptr;
    }

    BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM)
    {
        if (!IsWindowVisible(hwnd) || hwnd == g_mainWindow)
        {
            return TRUE;
        }

        WindowItem item = BuildWindowItem(hwnd);
        if (item.title.empty())
        {
            return TRUE;
        }

        g_windows.push_back(item);
        return TRUE;
    }

    void RefreshWindowList()
    {
        g_windows.clear();
        ListView_DeleteAllItems(g_windowList);
        EnumWindows(EnumWindowsProc, 0);

        for (int index = 0; index < static_cast<int>(g_windows.size()); ++index)
        {
            const WindowItem& item = g_windows[index];
            std::wstring processName = item.processName.empty() ? L"未知进程" : item.processName;
            std::wstring clientSize = std::to_wstring(item.clientWidth) + L"x" + std::to_wstring(item.clientHeight);
            std::wstring hwndText = HwndToText(item.hwnd);

            LVITEMW row{};
            row.mask = LVIF_TEXT | LVIF_PARAM;
            row.iItem = index;
            row.iSubItem = 0;
            row.pszText = const_cast<LPWSTR>(item.title.c_str());
            row.lParam = index;
            ListView_InsertItem(g_windowList, &row);
            ListView_SetItemText(g_windowList, index, 1, const_cast<LPWSTR>(processName.c_str()));
            ListView_SetItemText(g_windowList, index, 2, const_cast<LPWSTR>(clientSize.c_str()));
            ListView_SetItemText(g_windowList, index, 3, const_cast<LPWSTR>(item.className.c_str()));
            ListView_SetItemText(g_windowList, index, 4, const_cast<LPWSTR>(hwndText.c_str()));
        }
        SetStatus(L"已刷新窗口列表：" + std::to_wstring(g_windows.size()));
    }

    void BindWindow(HWND hwnd, const std::wstring& displayName, bool bringToFront)
    {
        g_boundHwnd = hwnd;
        if (bringToFront)
        {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(hwnd);
            SetTimer(g_mainWindow, TIMER_TOPMOST_RESTORE, 800, nullptr);
        }

        SetWindowTextW(g_boundText, (L"已绑定：" + displayName + L"  " + HwndToText(hwnd)).c_str());
        SetStatus(L"窗口已绑定");
    }

    void BindSelectedWindow()
    {
        int index = ListView_GetNextItem(g_windowList, -1, LVNI_SELECTED);
        if (index < 0 || index >= static_cast<int>(g_windows.size()))
        {
            return;
        }

        const WindowItem& item = g_windows[index];
        if (!IsWindow(item.hwnd))
        {
            MessageBoxW(g_mainWindow, L"该窗口已失效，请刷新列表", L"提示", MB_ICONINFORMATION);
            return;
        }

        BindWindow(item.hwnd, item.title, true);
    }

    bool ApplyAutoClickPointFromBoundHwnd(bool showMessage)
    {
        if (!g_boundHwnd || !IsWindow(g_boundHwnd))
        {
            if (showMessage)
            {
                MessageBoxW(g_mainWindow, L"请先绑定有效窗口", L"提示", MB_ICONINFORMATION);
            }
            return false;
        }

        RECT rect{};
        GetClientRect(g_boundHwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        POINT point{};
        if (!TryGetAutoClickPoint(width, height, point))
        {
            if (showMessage)
            {
                MessageBoxW(g_mainWindow, L"当前窗口分辨率暂不支持自动取点，请使用热键手动取点", L"提示", MB_ICONINFORMATION);
            }
            SetStatus(L"当前窗口分辨率暂不支持自动取点");
            return false;
        }

        SetWindowTextW(g_xEdit, std::to_wstring(point.x).c_str());
        SetWindowTextW(g_yEdit, std::to_wstring(point.y).c_str());
        SetStatus(L"已自动取点：" + std::to_wstring(width) + L"x" + std::to_wstring(height));
        return true;
    }

    bool AutoBindDeltaForceGameWindow()
    {
        const WindowItem* item = FindDeltaForceGameBodyWindow();
        if (!item || !IsWindow(item->hwnd))
        {
            MessageBoxW(g_mainWindow, L"请先运行游戏再启动软件", L"提示", MB_ICONINFORMATION);
            return false;
        }

        BindWindow(item->hwnd, item->title, false);
        ApplyAutoClickPointFromBoundHwnd(true);
        return true;
    }

    bool PointInsideClient(HWND hwnd, POINT point)
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        return point.x >= 0 && point.y >= 0 && point.x < rect.right && point.y < rect.bottom;
    }

    void StopPointPicking()
    {
        if (g_pickingPoint)
        {
            UnregisterHotKey(g_mainWindow, PICK_HOTKEY_ID);
            g_pickingPoint = false;
        }
        SetWindowTextW(g_pickButton, L"热键取点");
    }

    void FinishPointPicking(POINT screenPoint)
    {
        StopPointPicking();

        if (g_boundHwnd && IsWindow(g_boundHwnd))
        {
            POINT point = screenPoint;
            // 取点统一以绑定窗口客户区坐标保存，避免窗口位置变化影响后续点击。
            ScreenToClient(g_boundHwnd, &point);
            if (PointInsideClient(g_boundHwnd, point))
            {
                SetWindowTextW(g_xEdit, std::to_wstring(point.x).c_str());
                SetWindowTextW(g_yEdit, std::to_wstring(point.y).c_str());
                SetStatus(L"取点完成");
            }
            else
            {
                MessageBoxW(g_mainWindow, L"鼠标位置不在绑定窗口客户区内", L"提示", MB_ICONINFORMATION);
                SetStatus(L"取点失败");
            }
        }
    }

    void CaptureCurrentCursorPoint()
    {
        POINT point{};
        GetCursorPos(&point);
        FinishPointPicking(point);
    }

    void StartPointPicking()
    {
        if (!g_boundHwnd || !IsWindow(g_boundHwnd))
        {
            MessageBoxW(g_mainWindow, L"请先绑定有效窗口", L"提示", MB_ICONINFORMATION);
            return;
        }

        StopPointPicking();
        if (!RegisterHotKey(g_mainWindow, PICK_HOTKEY_ID, PICK_HOTKEY_MODIFIERS, PICK_HOTKEY_KEY))
        {
            MessageBoxW(g_mainWindow, L"注册取点热键 Ctrl+Shift+P 失败", L"错误", MB_ICONERROR);
            SetStatus(L"启动取点失败");
            return;
        }

        g_pickingPoint = true;
        SetWindowTextW(g_pickButton, L"等待热键...");
        // 游戏会消费鼠标点击，因此改用全局热键读取当前鼠标位置，不再拦截游戏点击。
        SetStatus(L"移动鼠标到目标点，按 Ctrl+Shift+P");
    }

    bool AcquireLocalClickGate(LocalClickGate& gate)
    {
        gate.mutex = CreateMutexW(nullptr, FALSE, LOCAL_CLICK_QUEUE_MUTEX);
        if (!gate.mutex)
        {
            SetStatus(L"本机点击队列创建失败");
            return false;
        }

        // 多个客户端进程共享同一个本机互斥锁，确保真实鼠标点击不会互相抢前台窗口。
        DWORD waitResult = WaitForSingleObject(gate.mutex, CLICK_QUEUE_WAIT_MS);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
        {
            gate.owns = true;
            return true;
        }

        SetStatus(L"等待本机点击队列超时");
        return false;
    }

    void ClickBoundPoint()
    {
        if (!g_boundHwnd || !IsWindow(g_boundHwnd))
        {
            SetReady(false);
            MessageBoxW(g_mainWindow, L"绑定窗口已失效，请重新绑定", L"提示", MB_ICONINFORMATION);
            return;
        }

        POINT point{ ReadEditInt(g_xEdit), ReadEditInt(g_yEdit) };
        if (!PointInsideClient(g_boundHwnd, point))
        {
            MessageBoxW(g_mainWindow, L"点击坐标不在绑定窗口客户区内", L"提示", MB_ICONINFORMATION);
            return;
        }

        LocalClickGate gate;
        if (!AcquireLocalClickGate(gate))
        {
            return;
        }

        // 点击坐标始终按绑定窗口的客户区坐标计算，再转换为屏幕坐标发送鼠标输入。
        ClientToScreen(g_boundHwnd, &point);
        SetForegroundWindow(g_boundHwnd);

        INPUT inputs[3]{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs[0].mi.dx = static_cast<LONG>(point.x * 65535 / GetSystemMetrics(SM_CXSCREEN));
        inputs[0].mi.dy = static_cast<LONG>(point.y * 65535 / GetSystemMetrics(SM_CYSCREEN));
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[2].type = INPUT_MOUSE;
        inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(3, inputs, sizeof(INPUT));
        Sleep(CLICK_QUEUE_RELEASE_DELAY_MS);
        SetStatus(L"已排队点击绑定窗口坐标");
    }

    void StartCountdown(int seconds)
    {
        g_countdown = seconds;
        if (ShouldClickImmediatelyForCountdown(seconds))
        {
            SetWindowTextW(g_countdownText, L"倒计时：点击");
            ClickBoundPoint();
            return;
        }

        SetWindowTextW(g_countdownText, (L"倒计时：" + std::to_wstring(g_countdown)).c_str());
        SetTimer(g_mainWindow, TIMER_COUNTDOWN, 1000, nullptr);
    }

    void ReceiveLoop()
    {
        std::string buffer;
        char chunk[512]{};
        while (g_connected)
        {
            int received = recv(g_socket, chunk, sizeof(chunk), 0);
            if (received <= 0)
            {
                break;
            }

            buffer.append(chunk, received);
            size_t lineEnd = std::string::npos;
            while ((lineEnd = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, lineEnd);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                buffer.erase(0, lineEnd + 1);

                Protocol::ParsedMessage message{};
                if (Protocol::ParseLine(line, message) && message.type == Protocol::MessageType::Trigger)
                {
                    PostMessageW(g_mainWindow, WM_TRIGGER_RECEIVED, static_cast<WPARAM>(message.value), 0);
                }
            }
        }
        PostMessageW(g_mainWindow, WM_SOCKET_DISCONNECTED, 0, 0);
    }

    void ConnectToServer()
    {
        if (!g_wsa.Ok())
        {
            MessageBoxW(g_mainWindow, L"Winsock 初始化失败", L"错误", MB_ICONERROR);
            return;
        }

        wchar_t ipText[128]{};
        GetWindowTextW(g_ipEdit, ipText, 128);
        int port = ReadEditInt(g_portEdit);
        if (port <= 0 || port > 65535)
        {
            MessageBoxW(g_mainWindow, L"端口号无效", L"错误", MB_ICONERROR);
            return;
        }

        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle == INVALID_SOCKET)
        {
            MessageBoxW(g_mainWindow, L"创建 Socket 失败", L"错误", MB_ICONERROR);
            return;
        }

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(static_cast<u_short>(port));
        if (InetPtonW(AF_INET, ipText, &serverAddress.sin_addr) != 1)
        {
            closesocket(socketHandle);
            MessageBoxW(g_mainWindow, L"服务端 IP 地址无效", L"错误", MB_ICONERROR);
            return;
        }

        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR)
        {
            closesocket(socketHandle);
            MessageBoxW(g_mainWindow, L"连接服务端失败", L"错误", MB_ICONERROR);
            return;
        }

        g_socket = socketHandle;
        g_connected = true;
        SaveLastIp(ipText);
        SocketUtil::SendLine(g_socket, Protocol::BuildHelloMessage());
        g_receiveThread = std::thread(ReceiveLoop);
        g_receiveThread.detach();
        SetWindowTextW(g_connectButton, L"断开");
        SetStatus(L"已连接服务端");
    }

    void ToggleReady()
    {
        if (!g_connected)
        {
            MessageBoxW(g_mainWindow, L"请先连接服务端", L"提示", MB_ICONINFORMATION);
            return;
        }
        if (!g_boundHwnd || !IsWindow(g_boundHwnd))
        {
            MessageBoxW(g_mainWindow, L"请先绑定有效窗口", L"提示", MB_ICONINFORMATION);
            return;
        }
        SetReady(!g_ready);
    }

    void InsertWindowListColumn(int index, int width, const wchar_t* title)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.iSubItem = index;
        column.cx = width;
        column.pszText = const_cast<LPWSTR>(title);
        ListView_InsertColumn(g_windowList, index, &column);
    }

    void CreateControls(HWND window)
    {
        CreateWindowW(L"STATIC", L"连接设置", WS_CHILD | WS_VISIBLE, 16, 14, 90, 22, window, nullptr, g_instance, nullptr);
        CreateWindowW(L"STATIC", L"服务端 IP：", WS_CHILD | WS_VISIBLE, 16, 46, 78, 24, window, nullptr, g_instance, nullptr);
        std::wstring lastIp = LoadLastIp();
        g_ipEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", lastIp.c_str(), WS_CHILD | WS_VISIBLE, 96, 42, 150, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_IP_EDIT)), g_instance, nullptr);
        CreateWindowW(L"STATIC", L"端口：", WS_CHILD | WS_VISIBLE, 270, 46, 48, 24, window, nullptr, g_instance, nullptr);
        g_portEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(Protocol::DefaultPort).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER, 320, 42, 86, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PORT_EDIT)), g_instance, nullptr);
        g_connectButton = CreateWindowW(L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE, 426, 40, 96, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT_BUTTON)), g_instance, nullptr);

        CreateWindowW(L"STATIC", L"目标窗口", WS_CHILD | WS_VISIBLE, 16, 88, 90, 22, window, nullptr, g_instance, nullptr);
        g_windowList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 16, 116, 900, 230, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_WINDOW_LIST)), g_instance, nullptr);
        ListView_SetExtendedListViewStyle(g_windowList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        InsertWindowListColumn(0, 300, L"窗口标题");
        InsertWindowListColumn(1, 250, L"进程名");
        InsertWindowListColumn(2, 90, L"客户区");
        InsertWindowListColumn(3, 170, L"窗口类名");
        InsertWindowListColumn(4, 86, L"HWND");

        CreateWindowW(L"BUTTON", L"刷新窗口列表", WS_CHILD | WS_VISIBLE, 16, 362, 120, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH_WINDOWS_BUTTON)), g_instance, nullptr);
        g_boundText = CreateWindowW(L"STATIC", L"未绑定窗口", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 152, 364, 764, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BOUND_TEXT)), g_instance, nullptr);

        CreateWindowW(L"STATIC", L"点击坐标", WS_CHILD | WS_VISIBLE, 16, 416, 90, 22, window, nullptr, g_instance, nullptr);
        CreateWindowW(L"STATIC", L"X：", WS_CHILD | WS_VISIBLE, 16, 448, 24, 24, window, nullptr, g_instance, nullptr);
        g_xEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1756", WS_CHILD | WS_VISIBLE | ES_NUMBER, 42, 444, 82, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_X_EDIT)), g_instance, nullptr);
        CreateWindowW(L"STATIC", L"Y：", WS_CHILD | WS_VISIBLE, 144, 448, 24, 24, window, nullptr, g_instance, nullptr);
        g_yEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"968", WS_CHILD | WS_VISIBLE | ES_NUMBER, 170, 444, 82, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_Y_EDIT)), g_instance, nullptr);
        g_pickButton = CreateWindowW(L"BUTTON", L"热键取点", WS_CHILD | WS_VISIBLE, 272, 442, 112, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PICK_BUTTON)), g_instance, nullptr);
        g_autoPointButton = CreateWindowW(L"BUTTON", L"自动取点", WS_CHILD | WS_VISIBLE, 400, 442, 112, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AUTO_POINT_BUTTON)), g_instance, nullptr);

        CreateWindowW(L"STATIC", L"准备状态", WS_CHILD | WS_VISIBLE, 548, 416, 90, 22, window, nullptr, g_instance, nullptr);
        g_readyButton = CreateWindowW(L"BUTTON", L"准备", WS_CHILD | WS_VISIBLE, 548, 442, 96, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_READY_BUTTON)), g_instance, nullptr);
        g_countdownText = CreateWindowW(L"STATIC", L"倒计时：-", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 664, 442, 252, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COUNTDOWN_TEXT)), g_instance, nullptr);

        CreateWindowW(L"STATIC", L"提示信息", WS_CHILD | WS_VISIBLE, 16, 500, 90, 22, window, nullptr, g_instance, nullptr);
        g_statusText = CreateWindowW(L"STATIC", L"未连接", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 96, 498, 820, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_TEXT)), g_instance, nullptr);

        RefreshWindowList();
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_mainWindow = window;
            CreateControls(window);
            if (!AutoBindDeltaForceGameWindow())
            {
                DestroyWindow(window);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_CONNECT_BUTTON:
                g_connected ? Disconnect() : ConnectToServer();
                return 0;
            case IDC_REFRESH_WINDOWS_BUTTON:
                RefreshWindowList();
                return 0;
            case IDC_PICK_BUTTON:
                StartPointPicking();
                return 0;
            case IDC_AUTO_POINT_BUTTON:
                ApplyAutoClickPointFromBoundHwnd(true);
                return 0;
            case IDC_READY_BUTTON:
                ToggleReady();
                return 0;
            }
            break;
        case WM_NOTIFY:
        {
            LPNMHDR header = reinterpret_cast<LPNMHDR>(lParam);
            if (header && header->idFrom == IDC_WINDOW_LIST && (header->code == NM_CLICK || header->code == NM_DBLCLK || header->code == LVN_ITEMCHANGED))
            {
                BindSelectedWindow();
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLOREDIT:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_TIMER:
            if (wParam == TIMER_TOPMOST_RESTORE && g_boundHwnd)
            {
                KillTimer(window, TIMER_TOPMOST_RESTORE);
                SetWindowPos(g_boundHwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                return 0;
            }
            if (wParam == TIMER_COUNTDOWN)
            {
                --g_countdown;
                if (g_countdown > 0)
                {
                    SetWindowTextW(g_countdownText, (L"倒计时：" + std::to_wstring(g_countdown)).c_str());
                }
                else
                {
                    KillTimer(window, TIMER_COUNTDOWN);
                    SetWindowTextW(g_countdownText, L"倒计时：点击");
                    ClickBoundPoint();
                }
                return 0;
            }
            break;
        case WM_HOTKEY:
            if (wParam == PICK_HOTKEY_ID && g_pickingPoint)
            {
                CaptureCurrentCursorPoint();
                return 0;
            }
            break;
        case WM_TRIGGER_RECEIVED:
            if (g_ready)
            {
                // 收到服务端触发后，本轮准备状态立即结束并同步给服务端。
                SetReady(false);
                StartCountdown(static_cast<int>(wParam));
            }
            return 0;
        case WM_SOCKET_DISCONNECTED:
            if (g_connected)
            {
                Disconnect();
                MessageBoxW(window, L"与服务端连接已断开", L"提示", MB_ICONINFORMATION);
            }
            return 0;
        case WM_DESTROY:
            StopPointPicking();
            Disconnect();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;

    if (!IsRunningAsAdministrator())
    {
        MessageBoxW(nullptr, L"请以管理员身份运行软件", L"提示", MB_ICONINFORMATION);
        return 0;
    }

    if (!IsDesktopScale100Percent())
    {
        MessageBoxW(nullptr, L"请将桌面缩放设置为 100% 后再启动软件", L"提示", MB_ICONINFORMATION);
        return 0;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    const wchar_t className[] = L"DF_PlayClick_Client_Window";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"DF_PlayClick 客户端", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 620, nullptr, nullptr, instance, nullptr);
    if (!window)
    {
        return 0;
    }

    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR)));
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON))));

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
