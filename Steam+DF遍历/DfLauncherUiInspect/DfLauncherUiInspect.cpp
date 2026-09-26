#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")

namespace {

struct WindowSet {
    std::vector<HWND> handles;
    HWND launcherWindow = nullptr;
};

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

std::wstring ReadString(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    if (FAILED(element->GetCurrentPropertyValue(property, &value))) return L"";
    std::wstring result;
    if (value.vt == VT_BSTR && value.bstrVal) {
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

int ReadInt(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    if (FAILED(element->GetCurrentPropertyValue(property, &value))) return 0;
    const int result = value.vt == VT_I4 ? value.intVal : 0;
    VariantClear(&value);
    return result;
}

std::wstring ProcessImageName(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"";
    wchar_t path[MAX_PATH * 4]{};
    DWORD length = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) result.assign(path, length);
    CloseHandle(process);
    return result;
}

bool IsDfLauncherWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    const std::wstring path = ProcessImageName(processId);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"df_launcher.exe") == 0;
}

void AddHandle(WindowSet* windows, HWND hwnd) {
    for (HWND existing : windows->handles) if (existing == hwnd) return;
    windows->handles.push_back(hwnd);
}

void PrintNativeWindow(const wchar_t* prefix, HWND hwnd) {
    wchar_t title[512]{};
    wchar_t className[256]{};
    const int titleLength = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    std::wcout << prefix
               << L" hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd)
               << L" pid=" << std::dec << processId
               << L" visible=" << (IsWindowVisible(hwnd) ? 1 : 0)
               << L" class=\"" << EscapeText(className) << L"\""
               << L" title=\"" << EscapeText(std::wstring(title, titleLength)) << L"\""
               << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
               << L"\n";
}

BOOL CALLBACK CollectChild(HWND hwnd, LPARAM data) {
    auto* windows = reinterpret_cast<WindowSet*>(data);
    AddHandle(windows, hwnd);
    PrintNativeWindow(L"  CHILD", hwnd);
    return TRUE;
}

BOOL CALLBACK CollectTopLevel(HWND hwnd, LPARAM data) {
    if (!IsDfLauncherWindow(hwnd)) return TRUE;
    auto* windows = reinterpret_cast<WindowSet*>(data);
    AddHandle(windows, hwnd);
    PrintNativeWindow(L"WINDOW", hwnd);
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (wcscmp(title, L"\x4e09\x89d2\x6d32\x884c\x52a8") == 0) {
        windows->launcherWindow = hwnd;
    }
    EnumChildWindows(hwnd, CollectChild, data);
    return TRUE;
}

std::wstring ControlTypeName(int type) {
    switch (type) {
    case UIA_WindowControlTypeId: return L"Window";
    case UIA_PaneControlTypeId: return L"Pane";
    case UIA_DocumentControlTypeId: return L"Document";
    case UIA_ButtonControlTypeId: return L"Button";
    case UIA_CheckBoxControlTypeId: return L"CheckBox";
    case UIA_RadioButtonControlTypeId: return L"RadioButton";
    case UIA_TextControlTypeId: return L"Text";
    case UIA_GroupControlTypeId: return L"Group";
    default: return L"ControlType(" + std::to_wstring(type) + L")";
    }
}

std::wstring PatternNames(IUIAutomationElement* element) {
    std::wstring result;
    const struct PatternInfo { PATTERNID id; const wchar_t* name; } patterns[] = {
        {UIA_InvokePatternId, L"Invoke"},
        {UIA_TogglePatternId, L"Toggle"},
        {UIA_SelectionItemPatternId, L"SelectionItem"},
        {UIA_LegacyIAccessiblePatternId, L"LegacyIAccessible"},
    };
    for (const auto& pattern : patterns) {
        IUnknown* object = nullptr;
        if (SUCCEEDED(element->GetCurrentPattern(pattern.id, &object)) && object) {
            if (!result.empty()) result += L", ";
            result += pattern.name;
            object->Release();
        }
    }
    return result;
}

void DumpRawInteresting(IUIAutomationTreeWalker* walker,
                        IUIAutomationElement* element, int depth) {
    if (!walker || !element || depth > 16) return;
    const int type = ReadInt(element, UIA_ControlTypePropertyId);
    const std::wstring name = ReadString(element, UIA_NamePropertyId);
    const std::wstring automationId = ReadString(element, UIA_AutomationIdPropertyId);
    const std::wstring patterns = PatternNames(element);
    if (!name.empty() || !automationId.empty() || !patterns.empty() ||
        type == UIA_ButtonControlTypeId || type == UIA_CheckBoxControlTypeId ||
        type == UIA_RadioButtonControlTypeId) {
        std::wcout << std::wstring(depth * 2, L' ')
                   << L"RAW type=" << ControlTypeName(type)
                   << L" name=\"" << EscapeText(name) << L"\""
                   << L" automationId=\"" << EscapeText(automationId) << L"\""
                   << L" enabled=" << (ReadBool(element, UIA_IsEnabledPropertyId) ? 1 : 0)
                   << L" patterns=[" << patterns << L"]\n";
    }

    IUIAutomationElement* child = nullptr;
    if (SUCCEEDED(walker->GetFirstChildElement(element, &child)) && child) {
        while (child) {
            DumpRawInteresting(walker, child, depth + 1);
            IUIAutomationElement* next = nullptr;
            walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = next;
        }
    }
}

void PrintElement(const wchar_t* prefix, IUIAutomationElement* element) {
    RECT rect{};
    element->get_CurrentBoundingRectangle(&rect);
    std::wcout << prefix
               << L" type=" << ControlTypeName(ReadInt(element, UIA_ControlTypePropertyId))
               << L" name=\"" << EscapeText(ReadString(element, UIA_NamePropertyId)) << L"\""
               << L" automationId=\"" << EscapeText(ReadString(element, UIA_AutomationIdPropertyId)) << L"\""
               << L" class=\"" << EscapeText(ReadString(element, UIA_ClassNamePropertyId)) << L"\""
               << L" enabled=" << (ReadBool(element, UIA_IsEnabledPropertyId) ? 1 : 0)
               << L" focusable=" << (ReadBool(element, UIA_IsKeyboardFocusablePropertyId) ? 1 : 0)
               << L" rect=" << std::dec << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
               << L" patterns=[" << PatternNames(element) << L"]\n";
}

void FindInteractiveControls(IUIAutomation* automation, IUIAutomationElement* root,
                             std::set<std::wstring>& seen, int& foundCount) {
    const CONTROLTYPEID types[] = {
        UIA_ButtonControlTypeId, UIA_CheckBoxControlTypeId, UIA_RadioButtonControlTypeId
    };
    std::vector<IUIAutomationCondition*> conditions;
    for (CONTROLTYPEID type : types) {
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = type;
        IUIAutomationCondition* condition = nullptr;
        if (SUCCEEDED(automation->CreatePropertyCondition(
                UIA_ControlTypePropertyId, value, &condition)) && condition) {
            conditions.push_back(condition);
        }
    }
    if (conditions.empty()) return;

    IUIAutomationCondition* combined = nullptr;
    HRESULT hr = conditions.front()->QueryInterface(IID_PPV_ARGS(&combined));
    for (size_t index = 1; SUCCEEDED(hr) && index < conditions.size(); ++index) {
        IUIAutomationCondition* next = nullptr;
        hr = automation->CreateOrCondition(combined, conditions[index], &next);
        combined->Release();
        combined = next;
    }
    for (IUIAutomationCondition* condition : conditions) condition->Release();
    if (FAILED(hr) || !combined) return;

    IUIAutomationElementArray* elements = nullptr;
    hr = root->FindAll(TreeScope_Subtree, combined, &elements);
    combined->Release();
    if (FAILED(hr) || !elements) return;

    int count = 0;
    elements->get_Length(&count);
    for (int index = 0; index < count; ++index) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || !element) continue;
        RECT rect{};
        element->get_CurrentBoundingRectangle(&rect);
        const std::wstring signature = std::to_wstring(ReadInt(element, UIA_ControlTypePropertyId)) + L"|" +
            ReadString(element, UIA_NamePropertyId) + L"|" +
            ReadString(element, UIA_AutomationIdPropertyId) + L"|" +
            std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top);
        if (seen.insert(signature).second) {
            PrintElement(L"FOUND", element);
            ++foundCount;
        }
        element->Release();
    }
    elements->Release();
}

IUIAutomationElement* FindElementByProperty(IUIAutomation* automation,
                                            IUIAutomationElement* root,
                                            PROPERTYID property, const wchar_t* value) {
    VARIANT requested{};
    requested.vt = VT_BSTR;
    requested.bstrVal = SysAllocString(value);
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElement* result = nullptr;
    if (requested.bstrVal &&
        SUCCEEDED(automation->CreatePropertyCondition(property, requested, &condition)) &&
        condition) {
        root->FindFirst(TreeScope_Subtree, condition, &result);
    }
    if (condition) condition->Release();
    VariantClear(&requested);
    return result;
}

bool EnsureChecked(IUIAutomationElement* checkbox) {
    if (!checkbox) return false;
    IUIAutomationTogglePattern* toggle = nullptr;
    HRESULT hr = checkbox->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle));
    if (FAILED(hr) || !toggle) {
        std::wcerr << L"read-module does not expose TogglePattern: 0x" << std::hex << hr << L"\n";
        return false;
    }
    ToggleState state = ToggleState_Off;
    HRESULT stateHr = toggle->get_CurrentToggleState(&state);
    if (state != ToggleState_On) {
        hr = toggle->Toggle();
        if (SUCCEEDED(hr)) {
            // CEF updates the UI asynchronously after Toggle().  An immediate
            // read can still report Off even though the toggle request worked.
            constexpr int kStateRetryCount = 30;
            constexpr DWORD kStateRetryDelayMs = 100;
            for (int attempt = 0; attempt < kStateRetryCount; ++attempt) {
                stateHr = toggle->get_CurrentToggleState(&state);
                if (SUCCEEDED(stateHr) && state == ToggleState_On) break;
                Sleep(kStateRetryDelayMs);
            }
        }
    }
    toggle->Release();
    std::wcout << L"Agreement checkbox state=" << static_cast<int>(state)
               << L" toggle=0x" << std::hex << hr
               << L" stateRead=0x" << stateHr << L"\n";
    // If the page rebuilt the DOM, this old element may remain at Off even
    // though Toggle() succeeded.  The caller performs a fresh-element check.
    return SUCCEEDED(hr);
}

bool IsAgreementChecked(IUIAutomation* automation, IUIAutomationElement* root) {
    if (!automation || !root) return false;
    IUIAutomationElement* checkbox = FindElementByProperty(
        automation, root, UIA_AutomationIdPropertyId, L"read-module");
    if (!checkbox) return false;

    IUIAutomationTogglePattern* toggle = nullptr;
    const HRESULT patternHr = checkbox->GetCurrentPatternAs(
        UIA_TogglePatternId, IID_PPV_ARGS(&toggle));
    ToggleState state = ToggleState_Off;
    HRESULT stateHr = E_FAIL;
    if (SUCCEEDED(patternHr) && toggle) {
        stateHr = toggle->get_CurrentToggleState(&state);
        toggle->Release();
    }
    checkbox->Release();
    return SUCCEEDED(patternHr) && SUCCEEDED(stateHr) && state == ToggleState_On;
}

bool WaitForAgreementChecked(IUIAutomation* automation, const WindowSet& windows,
                             IUIAutomationElement* initialDocument) {
    constexpr int kRetryCount = 30;
    constexpr DWORD kRetryDelayMs = 100;
    for (int attempt = 0; attempt < kRetryCount; ++attempt) {
        if (IsAgreementChecked(automation, initialDocument)) return true;

        for (HWND hwnd : windows.handles) {
            IUIAutomationElement* candidate = nullptr;
            if (FAILED(automation->ElementFromHandle(hwnd, &candidate)) || !candidate) {
                continue;
            }
            const bool checked = IsAgreementChecked(automation, candidate);
            candidate->Release();
            if (checked) return true;
        }
        Sleep(kRetryDelayMs);
    }
    std::wcerr << L"Agreement checkbox did not reach ToggleState_On.\n";
    return false;
}

bool InvokeNamedElement(IUIAutomationElement* element, const wchar_t* label) {
    if (!element) return false;
    IUIAutomationInvokePattern* invoke = nullptr;
    const HRESULT hr = element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
    if (SUCCEEDED(hr) && invoke) {
        const HRESULT invokeHr = invoke->Invoke();
        invoke->Release();
        std::wcout << label << L" Invoke=0x" << std::hex << invokeHr << L"\n";
        return SUCCEEDED(invokeHr);
    }
    std::wcerr << label << L" has no InvokePattern: 0x" << std::hex << hr << L"\n";
    return false;
}

bool TryFindAndInvokeStart(IUIAutomation* automation, IUIAutomationElement* document) {
    IUIAutomationElement* start = FindElementByProperty(
        automation, document, UIA_NamePropertyId, L"\x5f00\x59cb\x6e38\x620f");
    if (!start) return false;
    const bool invoked = InvokeNamedElement(start, L"Start game UIA");
    start->Release();
    return invoked;
}

bool TryUiAutomationStartAfterRefresh(IUIAutomation* automation, const WindowSet& windows,
                                       IUIAutomationElement* initialDocument,
                                       bool& agreementHandled) {
    agreementHandled = false;
    IUIAutomationElement* checkbox = FindElementByProperty(
        automation, initialDocument, UIA_AutomationIdPropertyId, L"read-module");
    if (!checkbox) {
        std::wcerr << L"Agreement checkbox read-module was not found.\n";
        return false;
    }

    const bool checked = EnsureChecked(checkbox);
    checkbox->Release();
    if (!checked) return false;

    if (!WaitForAgreementChecked(automation, windows, initialDocument)) {
        return false;
    }
    agreementHandled = true;
    std::wcout << L"Agreement checkbox confirmed On.\n";

    // Toggling the agreement can asynchronously rebuild the CEF document.
    // Reacquire the document and retry so the first run can continue to Start Game.
    constexpr int kRetryCount = 10;
    constexpr DWORD kRetryDelayMs = 500;
    for (int attempt = 0; attempt < kRetryCount; ++attempt) {
        if (attempt > 0) Sleep(kRetryDelayMs);

        for (HWND hwnd : windows.handles) {
            IUIAutomationElement* refreshedRoot = nullptr;
            if (FAILED(automation->ElementFromHandle(hwnd, &refreshedRoot)) ||
                !refreshedRoot) {
                continue;
            }
            const int refreshedType = ReadInt(refreshedRoot, UIA_ControlTypePropertyId);
            const std::wstring refreshedClass =
                ReadString(refreshedRoot, UIA_ClassNamePropertyId);
            if (refreshedType == UIA_DocumentControlTypeId ||
                refreshedClass == L"CefBrowserWindow" ||
                refreshedClass == L"Chrome_WidgetWin_0" ||
                refreshedClass == L"Chrome_RenderWidgetHostHWND") {
                if (TryFindAndInvokeStart(automation, refreshedRoot)) {
                    refreshedRoot->Release();
                    return true;
                }
            }

            IUIAutomationTreeWalker* walker = nullptr;
            if (SUCCEEDED(automation->get_RawViewWalker(&walker)) && walker) {
                IUIAutomationElement* child = nullptr;
                if (SUCCEEDED(walker->GetFirstChildElement(refreshedRoot, &child)) && child) {
                    while (child) {
                        const int type = ReadInt(child, UIA_ControlTypePropertyId);
                        const std::wstring className = ReadString(child, UIA_ClassNamePropertyId);
                        if (type == UIA_DocumentControlTypeId ||
                            className == L"CefBrowserWindow" || className == L"Chrome_WidgetWin_0") {
                            if (TryFindAndInvokeStart(automation, child)) {
                                child->Release();
                                walker->Release();
                                refreshedRoot->Release();
                                return true;
                            }
                        }
                        IUIAutomationElement* next = nullptr;
                        walker->GetNextSiblingElement(child, &next);
                        child->Release();
                        child = next;
                    }
                }
                walker->Release();
            }
            if (TryFindAndInvokeStart(automation, refreshedRoot)) {
                refreshedRoot->Release();
                return true;
            }
            refreshedRoot->Release();
        }

        if (TryFindAndInvokeStart(automation, initialDocument)) return true;
    }

    std::wcerr << L"Start-game element was not found after refresh retries.\n";
    return false;
}

bool TryCoordinateFallback(HWND launcherWindow) {
    if (!launcherWindow) return false;
    RECT windowRect{};
    if (!GetWindowRect(launcherWindow, &windowRect)) return false;
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    // Relative center of the visible launcher controls from the supplied layout.
    const int x = windowRect.left + static_cast<int>(width * 0.847);
    const int y = windowRect.top + static_cast<int>(height * 0.876);
    std::wcout << L"UIA start-game lookup failed; coordinate fallback at screen point "
               << x << L"," << y << L"\n";
    SetCursorPos(x, y);
    INPUT click[2]{};
    click[0].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].type = INPUT_MOUSE;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(2, click, sizeof(INPUT)) == 2;
}

} // namespace

int wmain() {
    std::wcout << L"DfLauncher UI inspection demo\n";
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initHr)) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << initHr << L"\n";
        return 1;
    }

    IUIAutomation* automation = nullptr;
    const HRESULT createHr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (FAILED(createHr)) {
        std::wcerr << L"UI Automation unavailable: 0x" << std::hex << createHr << L"\n";
        CoUninitialize();
        return 1;
    }

    WindowSet windows;
    EnumWindows(CollectTopLevel, reinterpret_cast<LPARAM>(&windows));
    if (windows.handles.empty()) {
        std::wcout << L"df_launcher.exe has no top-level windows.\n";
    }

    std::set<std::wstring> seen;
    int foundCount = 0;
    bool startAttempted = false;
    bool started = false;
    for (HWND hwnd : windows.handles) {
        IUIAutomationElement* root = nullptr;
        const HRESULT hr = automation->ElementFromHandle(hwnd, &root);
        if (FAILED(hr) || !root) continue;
        PrintElement(L"UIA ROOT", root);
        FindInteractiveControls(automation, root, seen, foundCount);
        const std::wstring rootClass = ReadString(root, UIA_ClassNamePropertyId);
        if (ReadInt(root, UIA_ControlTypePropertyId) == UIA_DocumentControlTypeId ||
            rootClass == L"CefBrowserWindow" || rootClass == L"Chrome_WidgetWin_0") {
            IUIAutomationTreeWalker* walker = nullptr;
            if (SUCCEEDED(automation->get_RawViewWalker(&walker)) && walker) {
                std::wcout << L"RAW CEF TREE class=\"" << EscapeText(rootClass) << L"\"\n";
                DumpRawInteresting(walker, root, 0);
                walker->Release();
            }
        }
        if (!startAttempted && ReadInt(root, UIA_ControlTypePropertyId) == UIA_DocumentControlTypeId) {
            startAttempted = true;
            bool agreementHandled = false;
            started = TryUiAutomationStartAfterRefresh(
                automation, windows, root, agreementHandled);
            if (!started && agreementHandled) {
                started = TryCoordinateFallback(windows.launcherWindow);
            }
        }
        root->Release();
    }

    std::wcout << L"Interactive controls found: " << foundCount << L"\n";
    std::wcout << (started ? L"Start-game action completed.\n" :
                           L"Start-game action was not completed.\n");
    automation->Release();
    CoUninitialize();
    std::wcout << L"Press Enter to exit...";
    std::wstring pause;
    std::getline(std::wcin, pause);
    return foundCount > 0 ? 0 : 1;
}
