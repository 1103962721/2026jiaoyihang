#include "AuctionPurchaseAutomation.h"
#include "AuctionControlAutomation.h"

#include <basetyps.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <QElapsedTimer>

namespace
{
using Microsoft::WRL::ComPtr;
using Cancel = std::function<bool()>;

QString readString(BSTR text)
{
    const QString value = text ? QString::fromWCharArray(text) : QString();
    SysFreeString(text);
    return value;
}

ComPtr<IUIAutomationElement> find(IUIAutomation* automation, IUIAutomationElement* root,
    const QString& suffix, CONTROLTYPEID type, QString& error)
{
    VARIANT value{}; value.vt = VT_I4; value.lVal = type;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition))
        || FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements)))
    { error = QStringLiteral("无法读取 Qt 控件列表"); return {}; }
    int count = 0, matches = 0;
    elements->get_Length(&count);
    ComPtr<IUIAutomationElement> found;
    for (int index = 0; index < count; ++index)
    {
        ComPtr<IUIAutomationElement> element;
        BSTR id = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || FAILED(element->get_CurrentAutomationId(&id))) continue;
        const QString name = readString(id);
        if (name == suffix || name.endsWith(QChar('.') + suffix)) { found = element; ++matches; }
    }
    if (matches != 1) { error = QStringLiteral("Qt 控件不存在或不唯一：%1").arg(suffix); return {}; }
    return found;
}

QString valueOf(IUIAutomationElement* element, bool* ok = nullptr)
{
    ComPtr<IUIAutomationValuePattern> pattern;
    BSTR value = nullptr;
    const bool success = SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern)))
        && pattern && SUCCEEDED(pattern->get_CurrentValue(&value));
    if (ok) *ok = success;
    return readString(value);
}

struct ComboItems
{
    ComPtr<IUIAutomationExpandCollapsePattern> expansion;
    ComPtr<IUIAutomationElementArray> elements;
    bool close = false;
    ~ComboItems()
    {
        if (!close || !expansion) return;
        expansion->Collapse();
        // Qt may animate the collapse asynchronously. Opening another popup
        // before the previous one releases its grab can close the new popup.
        for (int retry = 0; retry < 50; ++retry)
        {
            ExpandCollapseState state = ExpandCollapseState_Expanded;
            if (FAILED(expansion->get_CurrentExpandCollapseState(&state)) || state == ExpandCollapseState_Collapsed) break;
            QThread::msleep(20);
        }
    }
};

bool expandItems(IUIAutomation* automation, IUIAutomationElement* combo, ComboItems& result, QString& error, const Cancel& canceled)
{
    ExpandCollapseState before = ExpandCollapseState_Collapsed;
    if (FAILED(combo->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&result.expansion)))
        || !result.expansion || FAILED(result.expansion->get_CurrentExpandCollapseState(&before)))
    { error = QStringLiteral("Qt 下拉框不支持展开"); return false; }
    result.close = before == ExpandCollapseState_Collapsed;
    if (result.close && FAILED(result.expansion->Expand()))
    { error = QStringLiteral("无法展开 Qt 下拉框"); return false; }
    // Native Qt combo popups can animate for 150 ms. Keyboard navigation
    // during that animation cancels the popup instead of moving its highlight.
    if (result.close) QThread::msleep(200);
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition))) return false;
    for (int retry = 0; retry < 50 && !canceled(); ++retry)
    {
        result.elements.Reset();
        int count = 0;
        if (SUCCEEDED(combo->FindAll(TreeScope_Descendants, condition.Get(), &result.elements)) && result.elements
            && SUCCEEDED(result.elements->get_Length(&count)) && count > 0) return true;
        QThread::msleep(20);
    }
    error = QStringLiteral("无法枚举 Qt 下拉选项");
    return false;
}

HWND qtPopup(IUIAutomationElement* combo, QString& error)
{
    int processId = 0;
    if (FAILED(combo->get_CurrentProcessId(&processId))) return nullptr;
    struct Search { DWORD process; HWND popup = nullptr; int count = 0; } search{static_cast<DWORD>(processId)};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto* search = reinterpret_cast<Search*>(data);
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t name[256]{};
        if (process == search->process && IsWindowVisible(hwnd) && GetClassNameW(hwnd, name, 256)
            && QString::fromWCharArray(name).contains(QStringLiteral("QWindowPopup")))
        { search->popup = hwnd; ++search->count; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.count != 1) { error = QStringLiteral("无法定位唯一的 Qt 子弹/购买方式下拉弹窗"); return nullptr; }
    return search.popup;
}

bool popupKey(HWND popup, UINT code, QString& error, const Cancel& canceled)
{
    if (canceled()) return false;
    DWORD_PTR result = 0;
    const LPARAM scan = 1 | (static_cast<LPARAM>(MapVirtualKeyW(code, MAPVK_VK_TO_VSC)) << 16);
    if (!IsWindow(popup)
        || !SendMessageTimeoutW(popup, WM_KEYDOWN, code, scan, SMTO_ABORTIFHUNG, 1000, &result)
        || !SendMessageTimeoutW(popup, WM_KEYUP, code, scan | (LPARAM(3) << 30), SMTO_ABORTIFHUNG, 1000, &result))
    { error = QStringLiteral("Qt 下拉弹窗没有响应后台按键"); return false; }
    return !canceled();
}

bool readExpandedCombo(IUIAutomation* automation, IUIAutomationElement* combo, HWND popup,
    QStringList& names, QString& error, const Cancel& canceled)
{
    // Qt omits rows outside the popup viewport. Read the top and bottom
    // viewports, then walk overlapping pages only when the list is longer.
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition))
        || FAILED(automation->CreateCacheRequest(&cache))
        || FAILED(cache->AddProperty(UIA_NamePropertyId))) return false;
    const auto visibleItems = [&](QStringList& visible) {
        ComPtr<IUIAutomationElementArray> elements;
        int count = 0;
        for (int retry = 0; retry < 50 && !canceled(); ++retry)
        {
            elements.Reset(); count = 0;
            if (SUCCEEDED(combo->FindAllBuildCache(TreeScope_Descendants, condition.Get(), cache.Get(), &elements)) && elements
                && SUCCEEDED(elements->get_Length(&count)) && count > 0) break;
            QThread::msleep(20);
        }
        if (count < 1 || count > 1000)
        { error = QStringLiteral("Qt 下拉选项数量异常，未同步不完整列表"); return false; }
        for (int index = 0; index < count && !canceled(); ++index)
        {
            ComPtr<IUIAutomationElement> item;
            BSTR name = nullptr;
            if (FAILED(elements->GetElement(index, &item)) || FAILED(item->get_CachedName(&name)))
            { error = QStringLiteral("无法读取 Qt 子弹名称"); return false; }
            visible.append(readString(name));
        }
        return !canceled();
    };
    const auto move = [&](UINT key) {
        if (!popupKey(popup, key, error, canceled)) return false;
        QThread::msleep(20);
        return !canceled();
    };
    if (!move(VK_HOME) || !visibleItems(names)) return false;
    QStringList last;
    if (!move(VK_END) || !visibleItems(last)) return false;
    if (names == last) return true;
    if (!move(VK_HOME)) return false;
    for (int page = 0; page < 1000 && !canceled(); ++page)
    {
        if (!move(VK_NEXT)) return false;
        QStringList visible;
        if (!visibleItems(visible)) return false;
        int overlap = qMin(names.size(), visible.size());
        while (overlap > 0 && names.mid(names.size() - overlap) != visible.mid(0, overlap)) --overlap;
        names.append(visible.mid(overlap));
        if (names.size() > 1000) break;
        if (visible == last) return true;
    }
    error = QStringLiteral("Qt 下拉列表读取未完成，未同步不完整列表");
    return false;
}

QString comboId(IUIAutomationElement* combo)
{
    BSTR id = nullptr;
    if (FAILED(combo->get_CurrentAutomationId(&id))) return {};
    return readString(id);
}

// Prepare a previously discovered popup before Qt shows it. Transparency must
// be applied while hidden; hiding it after Expand allows a visible flash.
struct InvisiblePopup
{
    HWND hwnd;
    LONG_PTR style;
    COLORREF color = 0;
    BYTE alpha = 255;
    DWORD flags = LWA_ALPHA;
    bool ready = false;
    explicit InvisiblePopup(HWND window) : hwnd(window), style(GetWindowLongPtrW(window, GWL_EXSTYLE))
    {
        if (!IsWindow(hwnd) || IsWindowVisible(hwnd)) return;
        if ((style & WS_EX_LAYERED) && !GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags)) return;
        SetLastError(0);
        if (!SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED | WS_EX_NOACTIVATE) && GetLastError()) return;
        ready = SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA) != FALSE;
    }
    ~InvisiblePopup()
    {
        if (!IsWindow(hwnd)) return;
        if (ready && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        if (style & WS_EX_LAYERED) SetLayeredWindowAttributes(hwnd, color, alpha, flags);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    }
};

bool readCombo(IUIAutomation* automation, IUIAutomationElement* combo, QStringList& names, QString& error, const Cancel& canceled, QHash<QString, quintptr>& popups)
{
    bool readable = false;
    const auto before = valueOf(combo, &readable);
    if (!readable) { error = QStringLiteral("无法读取 Qt 下拉框当前值"); return false; }
    ComboItems list;
    if (!expandItems(automation, combo, list, error, canceled)) return false;
    const HWND popup = qtPopup(combo, error);
    if (!popup || !readExpandedCombo(automation, combo, popup, names, error, canceled)) return false;
    if (valueOf(combo) != before) { error = QStringLiteral("枚举后 Qt 下拉框值发生变化"); return false; }
    popups.insert(comboId(combo), reinterpret_cast<quintptr>(popup));
    return !canceled();
}

bool selectInvisibleCombo(IUIAutomation* automation, IUIAutomationElement* combo, const QString& desired,
    QString& error, const Cancel& canceled, const QHash<QString, quintptr>& popups)
{
    const HWND popup = reinterpret_cast<HWND>(popups.value(comboId(combo)));
    int process = 0;
    DWORD popupProcess = 0;
    combo->get_CurrentProcessId(&process);
    GetWindowThreadProcessId(popup, &popupProcess);
    if (!IsWindow(popup) || popupProcess != static_cast<DWORD>(process))
    { error = QStringLiteral("Qt 子弹弹窗缓存失效，请重新绑定 Qt 窗口后同步"); return false; }
    if (IsWindowVisible(popup))
    { error = QStringLiteral("请先收起目标 Qt 下拉列表后再同步"); return false; }
    InvisiblePopup invisible(popup);
    if (!invisible.ready)
    { error = QStringLiteral("无法隐藏 Qt 下拉弹窗，已停止同步以避免干扰前台"); return false; }
    ComboItems list;
    if (!expandItems(automation, combo, list, error, canceled)) return false;
    QStringList names;
    if (!readExpandedCombo(automation, combo, popup, names, error, canceled)) return false;
    const int row = names.indexOf(desired);
    if (row < 0) { error = QStringLiteral("Qt 下拉列表不存在选项：%1").arg(desired); return false; }
    if (!popupKey(popup, row < names.size() / 2 ? VK_HOME : VK_END, error, canceled)) return false;
    const int steps = row < names.size() / 2 ? row : int(names.size()) - 1 - row;
    for (int index = 0; index < steps; ++index)
        if (!popupKey(popup, row < names.size() / 2 ? VK_DOWN : VK_UP, error, canceled)) return false;
    if (!popupKey(popup, VK_RETURN, error, canceled)) return false;
    for (int retry = 0; retry < 20 && !canceled(); ++retry)
    {
        if (valueOf(combo) == desired) { error.clear(); return true; }
        QThread::msleep(20);
    }
    error = QStringLiteral("Qt 隐藏弹窗选择后回读失败：%1").arg(desired);
    return false;
}

bool selectCombo(IUIAutomation* automation, IUIAutomationElement* combo, const QString& desired, QString& error, const Cancel& canceled, const QHash<QString, quintptr>& popups, bool restore = true)
{
    bool readable = false;
    QString current = valueOf(combo, &readable);
    if (!readable || canceled()) return false;
    if (current == desired) return true;
    const QString original = current;
    BOOL enabled = FALSE;
    RECT bounds{};
    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUIAutomationElement> ancestor = combo;
    UIA_HWND native = nullptr;
    if (FAILED(combo->get_CurrentIsEnabled(&enabled)) || !enabled
        || FAILED(combo->get_CurrentBoundingRectangle(&bounds))
        || FAILED(automation->get_RawViewWalker(&walker)))
    { error = QStringLiteral("Qt 下拉框不可用，无法后台同步"); return false; }
    for (int depth = 0; ancestor && depth < 32; ++depth)
    {
        ancestor->get_CurrentNativeWindowHandle(&native);
        if (native) break;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(ancestor.Get(), &parent))) break;
        ancestor = parent;
    }
    const HWND window = reinterpret_cast<HWND>(native);
    if (!IsWindow(window) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    { error = QStringLiteral("无法定位 Qt 下拉框后台窗口"); return false; }
    // Deliver only to the target HWND. A closed QComboBox handles wheel steps
    // by committing its index, without opening a popup or requesting focus.
    const LPARAM point = MAKELPARAM((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
    for (int direction : {-WHEEL_DELTA, WHEEL_DELTA})
    {
        for (int step = 0; step < 1000 && !canceled(); ++step)
        {
            DWORD_PTR ignored = 0;
            if (!SendMessageTimeoutW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(direction)),
                point, SMTO_ABORTIFHUNG, 1000, &ignored))
            { error = QStringLiteral("Qt 下拉框后台切换超时"); return false; }
            QString next;
            for (int retry = 0; retry < 10 && !canceled(); ++retry)
            {
                QThread::msleep(10);
                next = valueOf(combo, &readable);
                if (!readable) { error = QStringLiteral("Qt 下拉框回读失败"); return false; }
                if (next != current) break;
            }
            if (next == desired) return !canceled();
            if (next == current) break;
            current = next;
        }
    }
    if (restore && !canceled() && current != original)
    {
        QString ignored;
        selectCombo(automation, combo, original, ignored, canceled, popups, false);
    }
    return !canceled() && selectInvisibleCombo(automation, combo, desired, error, canceled, popups);
}

QString ammoSuffix(int index)
{
    return QStringLiteral("ammo-picker.QWidget.QWidget.ammo-item") + (index == 0 ? QString() : QStringLiteral("-%1").arg(index + 1));
}

bool applyConfig(IUIAutomation* automation, IUIAutomationElement* root, const AuctionPurchase::Config& config,
    QString& message, const Cancel& canceled, const QHash<QString, quintptr>& popups)
{
    if (!config.ready(message)) return false;
    auto mode = find(automation, root, QStringLiteral("rule-panel.QWidget.priceMode"), UIA_ComboBoxControlTypeId, message);
    if (!mode || !selectCombo(automation, mode.Get(), config.mode, message, canceled, popups)) return false;

    std::array<ComPtr<IUIAutomationTogglePattern>, 5> toggles;
    // Clear the other four before enabling the chosen slot.
    for (int index = 0; index < 5 && !canceled(); ++index)
    {
        auto checkbox = find(automation, root, QStringLiteral("ammo-picker.QWidget.ammo-slot-check-%1").arg(index + 1), UIA_CheckBoxControlTypeId, message);
        if (!checkbox || FAILED(checkbox->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggles[index]))) || !toggles[index])
        { message = QStringLiteral("无法读取第 %1 款复选框").arg(index + 1); return false; }
        ToggleState state;
        if (FAILED(toggles[index]->get_CurrentToggleState(&state))) return false;
        if (index + 1 != config.selected && state != ToggleState_Off
            && (canceled() || FAILED(toggles[index]->Toggle()))) return false;
    }
    if (canceled()) return false;
    ToggleState chosen;
    if (FAILED(toggles[config.selected - 1]->get_CurrentToggleState(&chosen))) return false;
    if (chosen != ToggleState_On && FAILED(toggles[config.selected - 1]->Toggle())) return false;

    const auto& item = config.entries[config.selected - 1];
    auto combo = find(automation, root, ammoSuffix(config.selected - 1), UIA_ComboBoxControlTypeId, message);
    if (!combo || !selectCombo(automation, combo.Get(), item.ammo, message, canceled, popups)) return false;

    auto threshold = find(automation, root, QStringLiteral("ammo-picker.QWidget.QWidget.buy-threshold-%1").arg(config.selected), UIA_EditControlTypeId, message);
    BOOL enabled = FALSE;
    if (!threshold || FAILED(threshold->get_CurrentIsEnabled(&enabled))) return false;
    if (enabled)
    {
        ComPtr<IUIAutomationValuePattern> value;
        HRESULT written = threshold->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value));
        if (SUCCEEDED(written) && value && !canceled())
        {
            BSTR text = SysAllocString(reinterpret_cast<LPCWSTR>(item.threshold.utf16()));
            written = value->SetValue(text);
            SysFreeString(text);
        }
        else written = E_FAIL;
        if (FAILED(written) || valueOf(threshold.Get()) != item.threshold)
        { message = QStringLiteral("Qt 购买阈值写入或回读失败"); return false; }
    }
    else if (config.mode != AuctionPurchase::modes().at(2))
    { message = QStringLiteral("当前 Qt 购买方式未启用阈值，未完成同步"); return false; }

    for (int index = 0; index < 5; ++index)
    {
        ToggleState state;
        if (FAILED(toggles[index]->get_CurrentToggleState(&state)) || state != (index + 1 == config.selected ? ToggleState_On : ToggleState_Off))
        { message = QStringLiteral("购买款式单选回读失败"); return false; }
    }
    if (valueOf(mode.Get()) != config.mode || valueOf(combo.Get()) != item.ammo)
    { message = QStringLiteral("Qt 购买方式或子弹回读失败"); return false; }
    if (canceled()) return false;
    auto save = find(automation, root, QStringLiteral("task-panel.task-save-buy"), UIA_ButtonControlTypeId, message);
    ComPtr<IUIAutomationInvokePattern> invoke;
    BOOL saveEnabled = FALSE;
    if (!save || FAILED(save->get_CurrentIsEnabled(&saveEnabled)) || !saveEnabled
        || FAILED(save->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke)))
        || !invoke || canceled() || FAILED(invoke->Invoke()))
    { message = QStringLiteral("保存购买配置按钮不可用或后台调用失败"); return false; }

    auto buy = find(automation, root, QStringLiteral("task-panel.start"), UIA_ButtonControlTypeId, message);
    if (!buy) return false;
    QElapsedTimer wait;
    wait.start();
    while (!canceled() && wait.elapsed() < 15000)
    {
        BOOL buyEnabled = FALSE;
        if (FAILED(buy->get_CurrentIsEnabled(&buyEnabled)))
        { message = QStringLiteral("保存后无法读取开始购买按钮状态"); return false; }
        if (buyEnabled)
        {
            message = enabled ? QStringLiteral("购买参数已回读、保存，开始购买已可用")
                : QStringLiteral("购买参数已回读、保存，开始购买已可用；当前方式不启用阈值，服务端继续记忆");
            return !canceled();
        }
        QThread::msleep(100);
    }
    message = canceled() ? QStringLiteral("参数同步已取消")
        : QStringLiteral("已保存购买配置，但 15 秒内开始购买仍不可用，同步失败");
    return false;
}
}

AuctionPurchaseAutomation::AuctionPurchaseAutomation(QObject* parent) : QObject(parent) {}
AuctionPurchaseAutomation::~AuctionPurchaseAutomation() { shutdown(); }
bool AuctionPurchaseAutomation::readCatalog(HWND hwnd) { return start(hwnd, 0, {}); }
bool AuctionPurchaseAutomation::apply(HWND hwnd, int revision, const AuctionPurchase::Config& config) { return revision > 0 && start(hwnd, revision, config); }

bool AuctionPurchaseAutomation::start(HWND hwnd, int revision, const AuctionPurchase::Config& config)
{
    if (worker_) return false;
    canceled_.store(false);
    if (catalogWindow_ != hwnd) { comboPopups_.clear(); catalogWindow_ = hwnd; }
    struct Result { bool success = false; QString catalog; QString message; };
    auto result = std::make_shared<Result>();
    worker_ = QThread::create([=]() {
        const Cancel canceled = [this]() { return canceled_.load(); };
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) { result->message = QStringLiteral("参数同步 COM 初始化失败"); return; }
        {
            ComPtr<IUIAutomation> automation;
            ComPtr<IUIAutomationElement> root;
            if (!AuctionControlAutomation::isTargetWindow(hwnd)) result->message = QStringLiteral("Qt 绑定窗口已失效");
            else if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
                result->message = QStringLiteral("参数同步 UI Automation 初始化失败");
            else
            {
                ComPtr<IUIAutomation2> timeouts;
                if (SUCCEEDED(automation.As(&timeouts))) { timeouts->put_ConnectionTimeout(1000); timeouts->put_TransactionTimeout(1000); }
                if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) result->message = QStringLiteral("无法读取绑定 Qt 窗口");
                else if (revision > 0) result->success = applyConfig(automation.Get(), root.Get(), config, result->message, canceled, comboPopups_);
                else
                {
                    AuctionPurchase::Catalog catalog;
                    auto mode = find(automation.Get(), root.Get(), QStringLiteral("rule-panel.QWidget.priceMode"), UIA_ComboBoxControlTypeId, result->message);
                    bool ok = mode && readCombo(automation.Get(), mode.Get(), catalog.purchaseModes, result->message, canceled, comboPopups_);
                    for (int index = 0; index < 5 && ok && !canceled(); ++index)
                    {
                        auto combo = find(automation.Get(), root.Get(), ammoSuffix(index), UIA_ComboBoxControlTypeId, result->message);
                        ok = combo && readCombo(automation.Get(), combo.Get(), catalog.ammunition[index], result->message, canceled, comboPopups_);
                    }
                    result->success = ok && !canceled();
                    if (result->success) { result->catalog = AuctionPurchase::encode(catalog.toJson()); result->message = QStringLiteral("已读取 Qt 五款子弹及购买方式列表"); }
                }
            }
        }
        CoUninitialize();
    });
    auto* worker = worker_;
    connect(worker, &QThread::finished, this, [=]() {
        worker->wait(); worker_ = nullptr; worker->deleteLater();
        const bool canceled = canceled_.load();
        emit completed(revision, result->success && !canceled, result->catalog,
            canceled ? QStringLiteral("参数同步已取消") : result->message.isEmpty() ? QStringLiteral("Qt 控件写入失败") : result->message);
    });
    worker_->start();
    return true;
}

void AuctionPurchaseAutomation::cancel() { canceled_.store(true); }
void AuctionPurchaseAutomation::shutdown()
{
    cancel();
    if (!worker_) return;
    disconnect(worker_, nullptr, this, nullptr);
    worker_->wait(); delete worker_; worker_ = nullptr;
}
