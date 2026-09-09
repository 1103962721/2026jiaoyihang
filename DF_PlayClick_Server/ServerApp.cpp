#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../Shared/Protocol.h"
#include "../Shared/SocketUtil.h"
#include "ServerRules.h"
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace
{
    constexpr int IDC_PORT_EDIT = 1001;
    constexpr int IDC_LISTEN_BUTTON = 1002;
    constexpr int IDC_CLIENT_LIST = 1003;
    constexpr int IDC_TRIGGER_BUTTON = 1004;
    constexpr int IDC_STATUS_TEXT = 1005;
    constexpr int IDC_REMARK_CLIENT = 1006;
    constexpr int IDC_KICK_CLIENT = 1007;
    constexpr int IDC_REMARK_EDIT = 1008;
    constexpr int IDC_REMARK_OK = 1009;
    constexpr int IDC_REMARK_CANCEL = 1010;
    constexpr int IDC_COUNTDOWN_EDIT = 1011;

    constexpr UINT WM_ADD_CLIENT = WM_APP + 1;
    constexpr UINT WM_REFRESH_CLIENTS = WM_APP + 2;

    struct ClientInfo
    {
        SOCKET socketHandle = INVALID_SOCKET;
        std::wstring ipText;
        std::wstring remark;
        bool ready = false;
        bool connected = true;
        std::thread receiveThread;
    };

    struct RemarkDialogState
    {
        std::wstring text;
        bool accepted = false;
        bool closed = false;
        HWND edit = nullptr;
    };

    HINSTANCE g_instance = nullptr;
    HWND g_mainWindow = nullptr;
    HWND g_portEdit = nullptr;
    HWND g_listenButton = nullptr;
    HWND g_clientList = nullptr;
    HWND g_triggerButton = nullptr;
    HWND g_countdownEdit = nullptr;
    HWND g_statusText = nullptr;

    enum class StatusTone
    {
        Neutral,
        Success,
        Error
    };

    constexpr COLORREF ColorHeader = RGB(30, 58, 138);
    constexpr COLORREF ColorHeaderSubtle = RGB(219, 234, 254);
    constexpr COLORREF ColorBody = RGB(248, 250, 252);
    constexpr COLORREF ColorCard = RGB(255, 255, 255);
    constexpr COLORREF ColorBorder = RGB(226, 232, 240);
    constexpr COLORREF ColorText = RGB(31, 41, 55);
    constexpr COLORREF ColorMuted = RGB(100, 116, 139);
    constexpr COLORREF ColorSuccessText = RGB(22, 101, 52);
    constexpr COLORREF ColorSuccessBg = RGB(220, 252, 231);
    constexpr COLORREF ColorErrorText = RGB(153, 27, 27);
    constexpr COLORREF ColorErrorBg = RGB(254, 226, 226);
    constexpr COLORREF ColorNeutralText = RGB(55, 65, 81);
    constexpr COLORREF ColorNeutralBg = RGB(229, 231, 235);

    HFONT g_font = nullptr;
    HFONT g_titleFont = nullptr;
    HFONT g_sectionFont = nullptr;
    HBRUSH g_bodyBrush = nullptr;
    HBRUSH g_cardBrush = nullptr;
    HBRUSH g_editBrush = nullptr;
    HBRUSH g_statusBrush = nullptr;
    COLORREF g_statusTextColor = ColorNeutralText;
    COLORREF g_statusBgColor = ColorNeutralBg;

    SocketUtil::WsaSession g_wsa;
    SOCKET g_listenSocket = INVALID_SOCKET;
    bool g_listening = false;
    std::thread g_acceptThread;
    std::mutex g_clientsMutex;
    std::vector<std::shared_ptr<ClientInfo>> g_clients;
    std::weak_ptr<ClientInfo> g_contextClient;

    void ResetStatusBrush(COLORREF background)
    {
        if (g_statusBrush)
        {
            DeleteObject(g_statusBrush);
        }
        g_statusBgColor = background;
        g_statusBrush = CreateSolidBrush(g_statusBgColor);
    }

    void SetStatus(const std::wstring& text, StatusTone tone = StatusTone::Neutral)
    {
        if (tone == StatusTone::Success)
        {
            g_statusTextColor = ColorSuccessText;
            ResetStatusBrush(ColorSuccessBg);
        }
        else if (tone == StatusTone::Error)
        {
            g_statusTextColor = ColorErrorText;
            ResetStatusBrush(ColorErrorBg);
        }
        else
        {
            g_statusTextColor = ColorNeutralText;
            ResetStatusBrush(ColorNeutralBg);
        }

        SetWindowTextW(g_statusText, text.c_str());
        InvalidateRect(g_statusText, nullptr, TRUE);
    }

    void InitUiResources()
    {
        g_font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        g_titleFont = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        g_sectionFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        g_bodyBrush = CreateSolidBrush(ColorBody);
        g_cardBrush = CreateSolidBrush(ColorCard);
        g_editBrush = CreateSolidBrush(RGB(255, 255, 255));
        ResetStatusBrush(ColorNeutralBg);
    }

    void ReleaseUiResources()
    {
        HGDIOBJ objects[] = { g_font, g_titleFont, g_sectionFont, g_bodyBrush, g_cardBrush, g_editBrush, g_statusBrush };
        for (HGDIOBJ object : objects)
        {
            if (object)
            {
                DeleteObject(object);
            }
        }
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height)
    {
        HWND label = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, x, y, width, height, parent, nullptr, g_instance, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        return label;
    }

    HWND CreateEdit(HWND parent, const wchar_t* text, int x, int y, int width, int height, int id)
    {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_NUMBER, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        return edit;
    }

    HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int width, int height, int id)
    {
        HWND button = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        return button;
    }

    void DrawCard(HDC hdc, const RECT& rect)
    {
        HPEN borderPen = CreatePen(PS_SOLID, 1, ColorBorder);
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, g_cardBrush);
        RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 18, 18);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);
    }

    void PaintWindow(HWND window)
    {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window, &paint);

        RECT client{};
        GetClientRect(window, &client);
        FillRect(hdc, &client, g_bodyBrush);

        RECT header{ 0, 0, client.right, 104 };
        HBRUSH headerBrush = CreateSolidBrush(ColorHeader);
        FillRect(hdc, &header, headerBrush);
        DeleteObject(headerBrush);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HGDIOBJ oldFont = SelectObject(hdc, g_titleFont);
        RECT titleRect{ 28, 20, client.right - 28, 52 };
        DrawTextW(hdc, L"DF_PlayClick 服务端", -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SetTextColor(hdc, ColorHeaderSubtle);
        SelectObject(hdc, g_font);
        RECT subtitleRect{ 30, 58, client.right - 28, 84 };
        DrawTextW(hdc, L"PC-PC 触发控制 · 客户端准备状态 · 备注与踢出管理", -1, &subtitleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT controlCard{ 20, 124, client.right - 20, 228 };
        RECT listCard{ 20, 246, client.right - 20, client.bottom - 24 };
        DrawCard(hdc, controlCard);
        DrawCard(hdc, listCard);

        SelectObject(hdc, g_sectionFont);
        SetTextColor(hdc, RGB(15, 23, 42));
        RECT controlTitle{ 42, 140, 220, 166 };
        DrawTextW(hdc, L"监听与触发", -1, &controlTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT listTitle{ 42, 262, 360, 288 };
        DrawTextW(hdc, L"客户端列表", -1, &listTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(hdc, g_font);
        SetTextColor(hdc, ColorMuted);
        RECT listHint{ 142, 263, 520, 288 };
        DrawTextW(hdc, L"右键可备注 / 踢出", -1, &listHint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(hdc, oldFont);
        EndPaint(window, &paint);
    }

    int ReadPort()
    {
        wchar_t buffer[32]{};
        GetWindowTextW(g_portEdit, buffer, 32);
        int port = _wtoi(buffer);
        if (port <= 0 || port > 65535)
        {
            return 0;
        }
        return port;
    }

    int ReadCountdownSeconds()
    {
        wchar_t buffer[32]{};
        GetWindowTextW(g_countdownEdit, buffer, 32);
        return ParseCountdownSeconds(buffer);
    }

    std::shared_ptr<ClientInfo> FindClient(ClientInfo* target)
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        auto found = std::find_if(g_clients.begin(), g_clients.end(), [target](const std::shared_ptr<ClientInfo>& client)
        {
            return client.get() == target;
        });
        return found == g_clients.end() ? nullptr : *found;
    }

    std::shared_ptr<ClientInfo> ClientFromListRow(int row)
    {
        if (row < 0)
        {
            return nullptr;
        }

        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(g_clientList, &item))
        {
            return nullptr;
        }
        return FindClient(reinterpret_cast<ClientInfo*>(item.lParam));
    }

    void RefreshClientList()
    {
        ListView_DeleteAllItems(g_clientList);
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        int row = 0;
        for (const auto& client : g_clients)
        {
            if (!client->connected)
            {
                continue;
            }

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<LPWSTR>(client->ipText.c_str());
            item.lParam = reinterpret_cast<LPARAM>(client.get());
            ListView_InsertItem(g_clientList, &item);

            std::wstring state = client->ready ? L"准备" : L"未准备";
            ListView_SetItemText(g_clientList, row, 1, const_cast<LPWSTR>(state.c_str()));
            ListView_SetItemText(g_clientList, row, 2, const_cast<LPWSTR>(client->remark.c_str()));
            ++row;
        }

        SetStatus(L"已连接客户端：" + std::to_wstring(row), g_listening ? StatusTone::Success : StatusTone::Neutral);
    }

    void MarkClientDisconnected(const std::shared_ptr<ClientInfo>& client)
    {
        SOCKET socketToClose = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            if (!client || !client->connected)
            {
                return;
            }
            client->connected = false;
            client->ready = false;
            socketToClose = client->socketHandle;
            client->socketHandle = INVALID_SOCKET;
        }

        if (socketToClose != INVALID_SOCKET)
        {
            shutdown(socketToClose, SD_BOTH);
            closesocket(socketToClose);
        }
        PostMessageW(g_mainWindow, WM_REFRESH_CLIENTS, 0, 0);
    }

    LRESULT CALLBACK RemarkDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<RemarkDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message)
        {
        case WM_CREATE:
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<RemarkDialogState*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            CreateWindowW(L"STATIC", L"备注：", WS_CHILD | WS_VISIBLE, 14, 18, 54, 24, window, nullptr, g_instance, nullptr);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 68, 14, 230, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REMARK_EDIT)), g_instance, nullptr);
            CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 110, 54, 80, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REMARK_OK)), g_instance, nullptr);
            CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 210, 54, 80, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REMARK_CANCEL)), g_instance, nullptr);
            SetFocus(state->edit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_REMARK_OK && state)
            {
                wchar_t buffer[256]{};
                GetWindowTextW(state->edit, buffer, 256);
                state->text = buffer;
                state->accepted = true;
                DestroyWindow(window);
                return 0;
            }
            if (LOWORD(wParam) == IDC_REMARK_CANCEL)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (state)
            {
                state->closed = true;
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool PromptRemark(HWND owner, std::wstring& remark)
    {
        const wchar_t className[] = L"DF_PlayClick_Remark_Dialog";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = RemarkDialogProc;
        windowClass.hInstance = g_instance;
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass);

        RemarkDialogState state{};
        state.text = remark;
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        int x = ownerRect.left + 120;
        int y = ownerRect.top + 120;

        EnableWindow(owner, FALSE);
        HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"客户端备注", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 330, 130, owner, nullptr, g_instance, &state);
        if (!dialog)
        {
            EnableWindow(owner, TRUE);
            return false;
        }

        MSG message{};
        while (!state.closed && GetMessageW(&message, nullptr, 0, 0))
        {
            if (!IsDialogMessageW(dialog, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        if (state.accepted)
        {
            remark = state.text;
        }
        return state.accepted;
    }

    void EditClientRemark(const std::shared_ptr<ClientInfo>& client)
    {
        if (!client || !client->connected)
        {
            return;
        }

        std::wstring remark;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            remark = client->remark;
        }

        if (!PromptRemark(g_mainWindow, remark))
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            if (client->connected)
            {
                client->remark = remark;
            }
        }
        RefreshClientList();
    }

    void KickClient(const std::shared_ptr<ClientInfo>& client)
    {
        if (!client || !client->connected)
        {
            return;
        }

        std::wstring text = L"确定踢出客户端 " + client->ipText + L" 吗？";
        if (MessageBoxW(g_mainWindow, text.c_str(), L"确认踢出", MB_ICONQUESTION | MB_YESNO) != IDYES)
        {
            return;
        }

        MarkClientDisconnected(client);
        SetStatus(L"已踢出客户端：" + client->ipText, StatusTone::Neutral);
    }

    void ShowClientContextMenu(HWND window, LPARAM lParam)
    {
        POINT screenPoint{};
        screenPoint.x = GET_X_LPARAM(lParam);
        screenPoint.y = GET_Y_LPARAM(lParam);
        if (screenPoint.x == -1 && screenPoint.y == -1)
        {
            int row = ListView_GetNextItem(g_clientList, -1, LVNI_SELECTED);
            if (row < 0)
            {
                return;
            }
            RECT rect{};
            ListView_GetItemRect(g_clientList, row, &rect, LVIR_BOUNDS);
            screenPoint.x = rect.left;
            screenPoint.y = rect.bottom;
            ClientToScreen(g_clientList, &screenPoint);
        }

        POINT clientPoint = screenPoint;
        ScreenToClient(g_clientList, &clientPoint);
        LVHITTESTINFO hit{};
        hit.pt = clientPoint;
        int row = ListView_HitTest(g_clientList, &hit);
        auto client = ClientFromListRow(row);
        if (!client || !client->connected)
        {
            return;
        }

        ListView_SetItemState(g_clientList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        g_contextClient = client;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDC_REMARK_CLIENT, L"备注");
        AppendMenuW(menu, MF_STRING, IDC_KICK_CLIENT, L"踢出");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, window, nullptr);
        DestroyMenu(menu);
    }

    void ClientReceiveLoop(std::shared_ptr<ClientInfo> client)
    {
        std::string buffer;
        char chunk[512]{};

        while (client->connected)
        {
            int received = recv(client->socketHandle, chunk, sizeof(chunk), 0);
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
                if (Protocol::ParseLine(line, message) && message.type == Protocol::MessageType::Ready)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_clientsMutex);
                        client->ready = message.value == 1;
                    }
                    PostMessageW(g_mainWindow, WM_REFRESH_CLIENTS, 0, 0);
                }
            }
        }

        MarkClientDisconnected(client);
    }

    void AcceptLoop()
    {
        while (g_listening)
        {
            sockaddr_in clientAddress{};
            int addressLength = sizeof(clientAddress);
            SOCKET clientSocket = accept(g_listenSocket, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength);
            if (clientSocket == INVALID_SOCKET)
            {
                if (g_listening)
                {
                    PostMessageW(g_mainWindow, WM_REFRESH_CLIENTS, 0, 0);
                }
                break;
            }

            wchar_t ipBuffer[64]{};
            InetNtopW(AF_INET, &clientAddress.sin_addr, ipBuffer, 64);

            auto client = std::make_shared<ClientInfo>();
            client->socketHandle = clientSocket;
            client->ipText = ipBuffer;

            {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                g_clients.push_back(client);
            }

            client->receiveThread = std::thread(ClientReceiveLoop, client);
            client->receiveThread.detach();
            PostMessageW(g_mainWindow, WM_REFRESH_CLIENTS, 0, 0);
        }
    }

    void StopListening()
    {
        g_listening = false;
        if (g_listenSocket != INVALID_SOCKET)
        {
            closesocket(g_listenSocket);
            g_listenSocket = INVALID_SOCKET;
        }
        if (g_acceptThread.joinable())
        {
            g_acceptThread.join();
        }
        SetWindowTextW(g_listenButton, L"启动监听");
        SetStatus(L"监听已停止", StatusTone::Neutral);
    }

    void StartListening()
    {
        if (!g_wsa.Ok())
        {
            SetStatus(L"Winsock 初始化失败", StatusTone::Error);
            MessageBoxW(g_mainWindow, L"Winsock 初始化失败", L"错误", MB_ICONERROR);
            return;
        }

        int port = ReadPort();
        if (port == 0)
        {
            SetStatus(L"端口号无效", StatusTone::Error);
            MessageBoxW(g_mainWindow, L"端口号无效", L"错误", MB_ICONERROR);
            return;
        }

        SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET)
        {
            SetStatus(L"创建监听 Socket 失败", StatusTone::Error);
            MessageBoxW(g_mainWindow, L"创建监听 Socket 失败", L"错误", MB_ICONERROR);
            return;
        }

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        serverAddress.sin_port = htons(static_cast<u_short>(port));

        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR ||
            listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
        {
            closesocket(listenSocket);
            SetStatus(L"端口绑定或监听失败，请检查端口是否被占用", StatusTone::Error);
            MessageBoxW(g_mainWindow, L"端口绑定或监听失败，请检查端口是否被占用", L"错误", MB_ICONERROR);
            return;
        }

        g_listenSocket = listenSocket;
        g_listening = true;
        g_acceptThread = std::thread(AcceptLoop);
        SetWindowTextW(g_listenButton, L"停止监听");
        SetStatus(L"正在监听端口：" + std::to_wstring(port), StatusTone::Success);
    }

    void TriggerReadyClients()
    {
        int countdownSeconds = ReadCountdownSeconds();
        if (countdownSeconds < 0)
        {
            SetStatus(L"倒计时秒数无效", StatusTone::Error);
            MessageBoxW(g_mainWindow, L"倒计时秒数无效", L"错误", MB_ICONERROR);
            return;
        }

        std::vector<std::shared_ptr<ClientInfo>> readyClients;
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            for (const auto& client : g_clients)
            {
                if (client->connected && client->ready)
                {
                    readyClients.push_back(client);
                }
            }
        }

        std::string triggerMessage = Protocol::BuildTriggerMessage(countdownSeconds);
        for (const auto& client : readyClients)
        {
            SocketUtil::SendLine(client->socketHandle, triggerMessage);
        }

        SetStatus(L"已出发准备客户端：" + std::to_wstring(readyClients.size()) + L"，倒计时：" + std::to_wstring(countdownSeconds) + L"秒", StatusTone::Success);
    }

    void CreateControls(HWND window)
    {
        InitUiResources();

        CreateLabel(window, L"端口", 42, 174, 42, 28);
        g_portEdit = CreateEdit(window, std::to_wstring(Protocol::DefaultPort).c_str(), 88, 172, 110, 30, IDC_PORT_EDIT);
        g_listenButton = CreateButton(window, L"启动监听", 214, 170, 112, 34, IDC_LISTEN_BUTTON);

        CreateLabel(window, L"倒计时", 360, 174, 58, 28);
        g_countdownEdit = CreateEdit(window, L"5", 424, 172, 70, 30, IDC_COUNTDOWN_EDIT);
        CreateLabel(window, L"秒", 502, 174, 28, 28);
        g_triggerButton = CreateButton(window, L"出发", 550, 170, 92, 34, IDC_TRIGGER_BUTTON);

        g_statusText = CreateWindowW(L"STATIC", L"未监听", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 674, 170, 210, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_TEXT)), g_instance, nullptr);
        SendMessageW(g_statusText, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

        g_clientList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 42, 302, 842, 230, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLIENT_LIST)), g_instance, nullptr);
        SendMessageW(g_clientList, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        ListView_SetExtendedListViewStyle(g_clientList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ListView_SetBkColor(g_clientList, ColorCard);
        ListView_SetTextBkColor(g_clientList, ColorCard);
        ListView_SetTextColor(g_clientList, ColorText);

        LVCOLUMNW ipColumn{};
        ipColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        ipColumn.pszText = const_cast<LPWSTR>(L"客户端 IP");
        ipColumn.cx = 320;
        ListView_InsertColumn(g_clientList, 0, &ipColumn);

        LVCOLUMNW stateColumn{};
        stateColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        stateColumn.pszText = const_cast<LPWSTR>(L"状态");
        stateColumn.cx = 150;
        ListView_InsertColumn(g_clientList, 1, &stateColumn);

        LVCOLUMNW remarkColumn{};
        remarkColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        remarkColumn.pszText = const_cast<LPWSTR>(L"备注");
        remarkColumn.cx = 360;
        ListView_InsertColumn(g_clientList, 2, &remarkColumn);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_mainWindow = window;
            CreateControls(window);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_LISTEN_BUTTON:
                g_listening ? StopListening() : StartListening();
                return 0;
            case IDC_TRIGGER_BUTTON:
                TriggerReadyClients();
                return 0;
            case IDC_REMARK_CLIENT:
                EditClientRemark(g_contextClient.lock());
                return 0;
            case IDC_KICK_CLIENT:
                KickClient(g_contextClient.lock());
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == g_clientList)
            {
                ShowClientContextMenu(window, lParam);
                return 0;
            }
            break;
        case WM_PAINT:
            PaintWindow(window);
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(hdc, TRANSPARENT);
            if (control == g_statusText)
            {
                SetTextColor(hdc, g_statusTextColor);
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, g_statusBgColor);
                return reinterpret_cast<LRESULT>(g_statusBrush);
            }
            SetTextColor(hdc, ColorText);
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }
        case WM_CTLCOLOREDIT:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, ColorText);
            SetBkColor(hdc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(g_editBrush);
        }
        case WM_REFRESH_CLIENTS:
            RefreshClientList();
            return 0;
        case WM_DESTROY:
            StopListening();
            ReleaseUiResources();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    const wchar_t className[] = L"DF_PlayClick_Server_Window";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"DF_PlayClick 服务端", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 940, 620, nullptr, nullptr, instance, nullptr);
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
