#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <future>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <memory>
#include <cwctype>
#include <atomic>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::wstring ReadString(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    if (FAILED(element->GetCurrentPropertyValue(property, &value))) {
        return L"";
    }
    std::wstring result;
    if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    }
    VariantClear(&value);
    return result;
}

bool ReadBool(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    if (FAILED(element->GetCurrentPropertyValue(property, &value))) return false;
    const bool result = value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    return result;
}

std::wstring ControlTypeName(int type) {
    switch (type) {
    case UIA_WindowControlTypeId: return L"Window";
    case UIA_EditControlTypeId: return L"Edit";
    case UIA_ButtonControlTypeId: return L"Button";
    case UIA_TextControlTypeId: return L"Text";
    case UIA_CheckBoxControlTypeId: return L"CheckBox";
    case UIA_GroupControlTypeId: return L"Group";
    case UIA_PaneControlTypeId: return L"Pane";
    case UIA_DocumentControlTypeId: return L"Document";
    case UIA_HyperlinkControlTypeId: return L"Hyperlink";
    default: return L"ControlType(" + std::to_wstring(type) + L")";
    }
}

std::wstring PatternNames(IUIAutomationElement* element) {
    std::wstring result;
    const struct PatternInfo { PATTERNID id; const wchar_t* name; } patterns[] = {
        {UIA_ValuePatternId, L"Value"},
        {UIA_InvokePatternId, L"Invoke"},
        {UIA_LegacyIAccessiblePatternId, L"LegacyIAccessible"},
        {UIA_TextPatternId, L"Text"},
    };
    for (const auto& pattern : patterns) {
        IUnknown* object = nullptr;
        if (SUCCEEDED(element->GetCurrentPattern(pattern.id, &object)) && object != nullptr) {
            if (!result.empty()) result += L", ";
            result += pattern.name;
            object->Release();
        }
    }
    return result;
}

std::wstring ProcessImageName(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"";
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        result.assign(path, length);
    }
    CloseHandle(process);
    return result;
}

std::wstring EscapeText(const std::wstring& value) {
    std::wostringstream output;
    output << std::hex << std::setfill(L'0');
    for (wchar_t ch : value) {
        if (ch >= 0x20 && ch <= 0x7e && ch != L'\\' && ch != L'"') {
            output << ch;
        } else if (ch == L'\\') {
            output << L"\\\\";
        } else if (ch == L'"') {
            output << L"\\\"";
        } else {
            output << L"\\u" << std::setw(4)
                   << static_cast<unsigned int>(static_cast<unsigned short>(ch));
        }
    }
    return output.str();
}

struct InspectionContext {
    std::vector<HWND> uiaTargets;
    HWND loginWindow = nullptr;
};

bool IsUiaCandidateClass(const std::wstring& className) {
    return className == L"SDL_app" ||
        className == L"CefBrowserWindow" ||
        className == L"Chrome_WidgetWin_1" ||
        className == L"Chrome_RenderWidgetHostHWND";
}

void AddUiaTarget(InspectionContext* context, HWND hwnd, const std::wstring& className) {
    if (!context || !IsUiaCandidateClass(className)) return;
    for (HWND existing : context->uiaTargets) {
        if (existing == hwnd) return;
    }
    context->uiaTargets.push_back(hwnd);
}

void ProbeUiaHandle(HWND hwnd) {
    auto promise = std::make_shared<std::promise<std::wstring>>();
    std::future<std::wstring> future = promise->get_future();
    std::thread([promise, hwnd]() {
        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation* automation = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation));
        std::wostringstream output;
        output << L"UIA PROBE hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd);
        if (SUCCEEDED(hr)) {
            IUIAutomationElement* element = nullptr;
            hr = automation->ElementFromHandle(hwnd, &element);
            output << L" ElementFromHandle=0x" << std::hex << hr;
            if (SUCCEEDED(hr) && element) {
                VARIANT typeValue{};
                element->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &typeValue);
                const int type = typeValue.vt == VT_I4 ? typeValue.intVal : 0;
                VariantClear(&typeValue);
                output << L" type=" << ControlTypeName(type)
                       << L" name=\"" << EscapeText(ReadString(element, UIA_NamePropertyId)) << L"\""
                       << L" automationId=\"" << EscapeText(ReadString(element, UIA_AutomationIdPropertyId)) << L"\""
                       << L" class=\"" << EscapeText(ReadString(element, UIA_ClassNamePropertyId)) << L"\""
                       << L" patterns=[" << PatternNames(element) << L"]";
                element->Release();
            }
            automation->Release();
        } else {
            output << L" CoCreateInstance=0x" << std::hex << hr;
        }
        if (SUCCEEDED(initHr)) CoUninitialize();
        promise->set_value(output.str());
    }).detach();

    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        std::wcout << future.get() << L"\n" << std::flush;
    } else {
        std::wcout << L"UIA PROBE hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd)
                   << L" timed out after 2 seconds\n" << std::flush;
    }
}

IUIAutomationElement* FindByTwoProperties(IUIAutomation* automation,
                                          IUIAutomationElement* root,
                                          PROPERTYID firstId, VARIANT firstValue,
                                          PROPERTYID secondId, VARIANT secondValue) {
    IUIAutomationCondition* first = nullptr;
    IUIAutomationCondition* second = nullptr;
    IUIAutomationCondition* combined = nullptr;
    IUIAutomationElement* result = nullptr;
    if (SUCCEEDED(automation->CreatePropertyCondition(firstId, firstValue, &first)) &&
        SUCCEEDED(automation->CreatePropertyCondition(secondId, secondValue, &second)) &&
        SUCCEEDED(automation->CreateAndCondition(first, second, &combined))) {
        root->FindFirst(TreeScope_Descendants, combined, &result);
    }
    if (combined) combined->Release();
    if (second) second->Release();
    if (first) first->Release();
    return result;
}

void PrintLocatedElement(const wchar_t* label, IUIAutomationElement* element) {
    if (!element) {
        std::wcout << L"NOT FOUND " << label << L"\n";
        return;
    }
    RECT rect{};
    element->get_CurrentBoundingRectangle(&rect);
    std::wcout << L"FOUND " << label
               << L" name=\"" << EscapeText(ReadString(element, UIA_NamePropertyId)) << L"\""
               << L" automationId=\"" << EscapeText(ReadString(element, UIA_AutomationIdPropertyId)) << L"\""
               << L" password=" << (ReadBool(element, UIA_IsPasswordPropertyId) ? 1 : 0)
               << std::dec << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
               << L" patterns=[" << PatternNames(element) << L"]\n";
}

struct LoginControls {
    IUIAutomationElement* username = nullptr;
    IUIAutomationElement* password = nullptr;
    IUIAutomationElement* loginButton = nullptr;

    void Release() {
        if (loginButton) loginButton->Release();
        if (password) password->Release();
        if (username) username->Release();
        loginButton = nullptr;
        password = nullptr;
        username = nullptr;
    }
};

LoginControls LocateLoginControls(IUIAutomation* automation, IUIAutomationElement* document) {
    LoginControls controls;
    VARIANT editType{};
    editType.vt = VT_I4;
    editType.lVal = UIA_EditControlTypeId;

    VARIANT userId{};
    userId.vt = VT_BSTR;
    userId.bstrVal = SysAllocString(L"\x00abr1\x00bb");
    controls.username = FindByTwoProperties(
        automation, document, UIA_ControlTypePropertyId, editType,
        UIA_AutomationIdPropertyId, userId);
    VariantClear(&userId);

    VARIANT passwordFlag{};
    passwordFlag.vt = VT_BOOL;
    passwordFlag.boolVal = VARIANT_TRUE;
    controls.password = FindByTwoProperties(
        automation, document, UIA_ControlTypePropertyId, editType,
        UIA_IsPasswordPropertyId, passwordFlag);

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    VARIANT loginName{};
    loginName.vt = VT_BSTR;
    loginName.bstrVal = SysAllocString(L"\x767b\x5f55");
    controls.loginButton = FindByTwoProperties(
        automation, document, UIA_ControlTypePropertyId, buttonType,
        UIA_NamePropertyId, loginName);
    VariantClear(&loginName);

    PrintLocatedElement(L"USERNAME", controls.username);
    PrintLocatedElement(L"PASSWORD", controls.password);
    PrintLocatedElement(L"LOGIN_BUTTON", controls.loginButton);
    return controls;
}

HRESULT SetElementValue(IUIAutomationElement* element, const std::wstring& value) {
    if (!element) return E_POINTER;
    IUIAutomationValuePattern* pattern = nullptr;
    HRESULT hr = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
    if (SUCCEEDED(hr) && pattern) {
        element->SetFocus();
        BSTR text = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        if (!text && !value.empty()) {
            hr = E_OUTOFMEMORY;
        } else {
            hr = pattern->SetValue(text);
        }
        SysFreeString(text);
        pattern->Release();
    }
    return hr;
}

HRESULT InvokeElement(IUIAutomationElement* element) {
    if (!element) return E_POINTER;
    IUIAutomationInvokePattern* pattern = nullptr;
    HRESULT hr = element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern));
    if (SUCCEEDED(hr) && pattern) {
        hr = pattern->Invoke();
        pattern->Release();
    }
    return hr;
}

bool ReadCredentials(std::wstring& username, std::wstring& password) {
    std::wcout << L"Steam account: " << std::flush;
    std::getline(std::wcin, username);
    if (username.empty()) return false;

    std::wcout << L"Steam password (hidden): " << std::flush;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD originalMode = 0;
    const bool hasConsoleMode = input != INVALID_HANDLE_VALUE &&
        GetConsoleMode(input, &originalMode) != FALSE;
    if (hasConsoleMode) {
        SetConsoleMode(input, originalMode & ~ENABLE_ECHO_INPUT);
    }
    std::getline(std::wcin, password);
    if (hasConsoleMode) {
        SetConsoleMode(input, originalMode);
    }
    std::wcout << L"\n";
    return !password.empty();
}

bool FillAndSubmit(LoginControls& controls, HWND loginWindow,
                   const std::wstring& username, const std::wstring& password) {
    const HRESULT usernameHr = SetElementValue(controls.username, username);
    std::wcout << L"Set username: 0x" << std::hex << usernameHr << L"\n";
    if (FAILED(usernameHr)) return false;

    const HRESULT passwordHr = SetElementValue(controls.password, password);
    std::wcout << L"Set password: 0x" << std::hex << passwordHr << L"\n";
    if (FAILED(passwordHr)) return false;

    if (loginWindow && IsIconic(loginWindow)) {
        ShowWindowAsync(loginWindow, SW_RESTORE);
    }
    const HRESULT invokeHr = InvokeElement(controls.loginButton);
    std::wcout << L"Invoke login: 0x" << std::hex << invokeHr << L"\n";
    return SUCCEEDED(invokeHr);
}

BOOL CALLBACK CollectSteamProcessWindow(HWND hwnd, LPARAM data);

std::wstring LowerText(std::wstring value) {
    for (wchar_t& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

bool ContainsAny(const std::wstring& value, const std::vector<std::wstring>& keywords) {
    const std::wstring lowered = LowerText(value);
    for (const std::wstring& keyword : keywords) {
        if (lowered.find(LowerText(keyword)) != std::wstring::npos) return true;
    }
    return false;
}

bool ScanAndHandleDialog(IUIAutomation* automation) {
    InspectionContext inspection;
    EnumWindows(CollectSteamProcessWindow, reinterpret_cast<LPARAM>(&inspection));

    std::vector<IUIAutomationElement*> affirmativeButtons;
    std::vector<IUIAutomationElement*> closeButtons;
    const std::vector<std::wstring> affirmativeKeywords = {
        L"\x63a5\x53d7", L"\x540c\x610f", L"accept", L"agree"
    };
    const std::vector<std::wstring> closeKeywords = {
        L"\x5173\x95ed", L"close"
    };

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    IUIAutomationCondition* buttonCondition = nullptr;
    if (FAILED(automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId, buttonType, &buttonCondition))) {
        return false;
    }

    for (HWND target : inspection.uiaTargets) {
        wchar_t targetClass[256]{};
        GetClassNameW(target, targetClass, ARRAYSIZE(targetClass));
        if (wcscmp(targetClass, L"Chrome_RenderWidgetHostHWND") != 0) continue;

        IUIAutomationElement* document = nullptr;
        if (FAILED(automation->ElementFromHandle(target, &document)) || !document) continue;
        IUIAutomationElementArray* buttons = nullptr;
        if (SUCCEEDED(document->FindAll(TreeScope_Descendants, buttonCondition, &buttons)) && buttons) {
            int count = 0;
            buttons->get_Length(&count);
            for (int index = 0; index < count; ++index) {
                IUIAutomationElement* button = nullptr;
                if (FAILED(buttons->GetElement(index, &button)) || !button) continue;
                const std::wstring name = ReadString(button, UIA_NamePropertyId);
                if (ContainsAny(name, affirmativeKeywords)) {
                    affirmativeButtons.push_back(button);
                } else if (ContainsAny(name, closeKeywords)) {
                    closeButtons.push_back(button);
                } else {
                    button->Release();
                }
            }
            buttons->Release();
        }
        document->Release();
    }
    buttonCondition->Release();

    IUIAutomationElement* selected = !affirmativeButtons.empty()
        ? affirmativeButtons.front()
        : (!closeButtons.empty() ? closeButtons.front() : nullptr);
    bool invoked = false;
    if (selected) {
        const std::wstring name = ReadString(selected, UIA_NamePropertyId);
        const HRESULT hr = InvokeElement(selected);
        std::wcout << L"Dialog action: name=\"" << EscapeText(name)
                   << L"\" Invoke=0x" << std::hex << hr << L"\n";
        invoked = SUCCEEDED(hr);
    }

    for (IUIAutomationElement* button : affirmativeButtons) button->Release();
    for (IUIAutomationElement* button : closeButtons) button->Release();
    return invoked;
}

void MonitorDialogsUntilEnter(IUIAutomation* automation) {
    std::atomic<bool> running(true);
    std::thread monitor([&]() {
        while (running.load()) {
            ScanAndHandleDialog(automation);
            Sleep(500);
        }
    });
    std::wcout << L"Dialog monitoring is active. Press Enter to stop...\n" << std::flush;
    std::wstring stop;
    std::getline(std::wcin, stop);
    running.store(false);
    monitor.join();
}

std::wstring WindowTextWithTimeout(HWND hwnd) {
    wchar_t text[1024]{};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, ARRAYSIZE(text),
            reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG | SMTO_BLOCK,
            500, &result)) {
        return L"<WM_GETTEXT timeout/error " + std::to_wstring(GetLastError()) + L">";
    }
    return std::wstring(text);
}

void PrintWindowInfo(const wchar_t* prefix, HWND hwnd) {
    wchar_t title[512]{};
    wchar_t className[256]{};
    wchar_t realClassName[256]{};
    const int titleLength = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    RealGetWindowClassW(hwnd, realClassName, ARRAYSIZE(realClassName));
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    std::wcout << prefix
               << L" hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd)
               << L" pid=" << std::dec << processId
               << L" tid=" << threadId
               << L" visible=" << (IsWindowVisible(hwnd) ? 1 : 0)
               << L" class=\"" << className << L"\""
               << L" realClass=\"" << realClassName << L"\""
               << L" titleLength=" << titleLength
               << L" title=\"" << EscapeText(std::wstring(title, titleLength)) << L"\""
               << L" wmText=\"" << EscapeText(WindowTextWithTimeout(hwnd)) << L"\""
               << L" owner=0x" << std::hex
               << reinterpret_cast<UINT_PTR>(GetWindow(hwnd, GW_OWNER))
               << L" parent=0x" << reinterpret_cast<UINT_PTR>(GetParent(hwnd))
               << L" style=0x" << static_cast<UINT_PTR>(GetWindowLongPtrW(hwnd, GWL_STYLE))
               << L" exStyle=0x" << static_cast<UINT_PTR>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))
               << std::dec << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
               << L"\n" << std::flush;
}

BOOL CALLBACK DumpChildWindow(HWND hwnd, LPARAM data) {
    PrintWindowInfo(L"  CHILD", hwnd);
    wchar_t className[256]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    AddUiaTarget(reinterpret_cast<InspectionContext*>(data), hwnd, className);
    return TRUE;
}

BOOL CALLBACK DumpSteamProcessWindow(HWND hwnd, LPARAM data) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    const std::wstring image = ProcessImageName(processId);
    const auto slash = image.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? image : image.substr(slash + 1);
    if (_wcsicmp(name.c_str(), L"steam.exe") != 0 &&
        _wcsicmp(name.c_str(), L"steamwebhelper.exe") != 0) {
        return TRUE;
    }
    PrintWindowInfo(L"STEAM WINDOW", hwnd);
    wchar_t className[256]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    auto* context = reinterpret_cast<InspectionContext*>(data);
    AddUiaTarget(context, hwnd, className);
    EnumChildWindows(hwnd, DumpChildWindow, data);
    return TRUE;
}

BOOL CALLBACK CollectChildWindow(HWND hwnd, LPARAM data) {
    wchar_t className[256]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    AddUiaTarget(reinterpret_cast<InspectionContext*>(data), hwnd, className);
    return TRUE;
}

BOOL CALLBACK CollectSteamProcessWindow(HWND hwnd, LPARAM data) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    const std::wstring image = ProcessImageName(processId);
    const auto slash = image.find_last_of(L"\\/");
    const std::wstring imageName = slash == std::wstring::npos ? image : image.substr(slash + 1);
    if (_wcsicmp(imageName.c_str(), L"steam.exe") != 0 &&
        _wcsicmp(imageName.c_str(), L"steamwebhelper.exe") != 0) {
        return TRUE;
    }

    auto* context = reinterpret_cast<InspectionContext*>(data);
    wchar_t className[256]{};
    wchar_t title[512]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    const int titleLength = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    const std::wstring titleText(title, titleLength);
    AddUiaTarget(context, hwnd, className);
    if (wcscmp(className, L"SDL_app") == 0 &&
        (titleText == L"\x767b\x5f55 Steam" || titleText == L"Sign in to Steam")) {
        context->loginWindow = hwnd;
    }
    EnumChildWindows(hwnd, CollectChildWindow, data);
    return TRUE;
}

void DumpTree(IUIAutomation* automation, IUIAutomationTreeWalker* walker,
              IUIAutomationElement* element, int depth) {
    if (!element || depth > 12) return;

    VARIANT typeValue{};
    element->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &typeValue);
    const int actualType = typeValue.vt == VT_I4 ? typeValue.intVal : 0;
    VariantClear(&typeValue);

    const std::wstring name = EscapeText(ReadString(element, UIA_NamePropertyId));
    const std::wstring automationId = EscapeText(ReadString(element, UIA_AutomationIdPropertyId));
    const std::wstring className = EscapeText(ReadString(element, UIA_ClassNamePropertyId));
    RECT rect{};
    element->get_CurrentBoundingRectangle(&rect);

    std::wcout << std::wstring(depth * 2, L' ')
               << L"type=" << ControlTypeName(actualType)
               << L" name=\"" << name << L"\""
               << L" automationId=\"" << automationId << L"\""
               << L" class=\"" << className << L"\""
               << L" password=" << (ReadBool(element, UIA_IsPasswordPropertyId) ? 1 : 0)
               << L" focusable=" << (ReadBool(element, UIA_IsKeyboardFocusablePropertyId) ? 1 : 0)
               << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
               << L" patterns=[" << PatternNames(element) << L"]\n";

    if (walker) {
        IUIAutomationElement* child = nullptr;
        walker->GetFirstChildElement(element, &child);
        while (child) {
            DumpTree(automation, walker, child, depth + 1);
            IUIAutomationElement* next = nullptr;
            walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = next;
        }
    }
}

BOOL CALLBACK FindSteamWindow(HWND hwnd, LPARAM data) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    std::wstring value(title);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    const std::wstring image = ProcessImageName(processId);
    // Steam's login UI is usually hosted by steamwebhelper and may have no title.
    const auto lastSlash = image.find_last_of(L"\\/");
    const std::wstring imageName = lastSlash == std::wstring::npos
        ? image
        : image.substr(lastSlash + 1);
    // Only trust Steam's own processes. A browser page with "Steam" in its title
    // must not be mistaken for the Steam login window.
    if (_wcsicmp(imageName.c_str(), L"steam.exe") != 0 &&
        _wcsicmp(imageName.c_str(), L"steamwebhelper.exe") != 0) {
        return TRUE;
    }
    auto* windows = reinterpret_cast<std::vector<HWND>*>(data);
    windows->push_back(hwnd);
    return TRUE;
}

BOOL CALLBACK DumpLoginWindow(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512]{};
    wchar_t className[256]{};
    const int titleLength = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    std::wstring titleText(title, titleLength);
    if (titleText.find(L"登录 Steam") == std::wstring::npos &&
        titleText.find(L"登录steam") == std::wstring::npos &&
        titleText.find(L"Sign in to Steam") == std::wstring::npos) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    std::wcout << L"LOGIN WINDOW hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd)
               << L" pid=" << std::dec << processId
               << L" class=\"" << className << L"\""
               << L" title=\"" << EscapeText(titleText) << L"\""
               << L" process=\"" << EscapeText(ProcessImageName(processId)) << L"\"\n";
    return TRUE;
}

} // namespace

int wmain() {
    std::wcout << L"Steam login automation demo build 2026-09-24-1220\n" << std::flush;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << hr << L"\n";
        std::wcout << L"Press Enter to exit...";
        std::wstring pause;
        std::getline(std::wcin, pause);
        return 1;
    }

    IUIAutomation* automation = nullptr;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&automation));
    if (FAILED(hr)) {
        std::wcerr << L"UI Automation unavailable: 0x" << std::hex << hr << L"\n";
        CoUninitialize();
        std::wcout << L"Press Enter to exit...";
        std::wstring pause;
        std::getline(std::wcin, pause);
        return 1;
    }

    if (ScanAndHandleDialog(automation)) {
        automation->Release();
        CoUninitialize();
        std::wcout << L"Current Steam dialog handled. Press Enter to exit...";
        std::wstring pause;
        std::getline(std::wcin, pause);
        return 0;
    }

    InspectionContext inspection;
    EnumWindows(CollectSteamProcessWindow, reinterpret_cast<LPARAM>(&inspection));

    bool loginPageFound = false;
    bool submitted = false;
    for (HWND target : inspection.uiaTargets) {
        wchar_t targetClass[256]{};
        GetClassNameW(target, targetClass, ARRAYSIZE(targetClass));
        if (wcscmp(targetClass, L"Chrome_RenderWidgetHostHWND") != 0) continue;

        IUIAutomationElement* document = nullptr;
        const HRESULT documentHr = automation->ElementFromHandle(target, &document);
        if (FAILED(documentHr) || !document) continue;
        const std::wstring documentName = ReadString(document, UIA_NamePropertyId);
        if (documentName != L"\x767b\x5f55 Steam" && documentName != L"Sign in to Steam") {
            document->Release();
            continue;
        }

        loginPageFound = true;
        std::wcout << L"Steam login page found.\n";
        LoginControls controls = LocateLoginControls(automation, document);
        if (!controls.username || !controls.password || !controls.loginButton) {
            std::wcerr << L"Required login controls were not found.\n";
            controls.Release();
            document->Release();
            break;
        }

        std::wstring username;
        std::wstring password;
        if (!ReadCredentials(username, password)) {
            std::wcerr << L"Account or password is empty; login cancelled.\n";
        } else {
            submitted = FillAndSubmit(controls, inspection.loginWindow, username, password);
            if (!password.empty()) {
                SecureZeroMemory(&password[0], password.size() * sizeof(wchar_t));
            }
            password.clear();
            std::wcout << (submitted
                ? L"Credentials submitted to Steam.\n"
                : L"Steam login automation failed.\n");
            if (submitted) {
                MonitorDialogsUntilEnter(automation);
            }
        }
        controls.Release();
        document->Release();
        break;
    }

    if (!loginPageFound) {
        std::wcerr << L"Steam login page was not found. Open the Steam login window and retry.\n";
    }

    automation->Release();
    CoUninitialize();
    std::wcout << L"Press Enter to exit...";
    std::wstring pause;
    std::getline(std::wcin, pause);
    return submitted ? 0 : 1;
}
