#include "AuctionControlAutomation.h"
#include "AuctionRoundState.h"

#include <basetyps.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <memory>
#include <functional>

namespace
{
using Microsoft::WRL::ComPtr;
const wchar_t* BuyId = L"QApplication.dashboard.dashboard-scroll.qt_scrollarea_viewport.scroll-content.shell.QWidget.QWidget.task-panel.start";
const wchar_t* SellId = L"QApplication.dashboard.dashboard-scroll.qt_scrollarea_viewport.scroll-content.shell.QWidget.QWidget.task-panel.sell-start-quick";

bool findButton(IUIAutomation* automation, IUIAutomationElement* root, const wchar_t* id,
    ComPtr<IUIAutomationElement>& button, QString& error)
{
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(id);
    ComPtr<IUIAutomationCondition> condition;
    HRESULT hr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
    VariantClear(&value);
    if (SUCCEEDED(hr)) hr = root->FindFirst(TreeScope_Descendants, condition.Get(), &button);
    // Qt providers can omit the QApplication/anonymous QWidget prefix. Keep
    // the task-panel + object-name identity, and reject ambiguous matches.
    if (SUCCEEDED(hr) && !button)
    {
        value.vt = VT_I4; value.lVal = UIA_ButtonControlTypeId;
        condition.Reset();
        hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
        ComPtr<IUIAutomationElementArray> candidates;
        if (SUCCEEDED(hr)) hr = root->FindAll(TreeScope_Descendants, condition.Get(), &candidates);
        int count = 0, matches = 0;
        if (candidates) candidates->get_Length(&count);
        const QString suffix = id == BuyId ? QStringLiteral("task-panel.start") : QStringLiteral("task-panel.sell-start-quick");
        for (int i = 0; i < count; ++i)
        {
            ComPtr<IUIAutomationElement> candidate;
            if (FAILED(candidates->GetElement(i, &candidate)) || !candidate) continue;
            BSTR candidateId = nullptr;
            if (FAILED(candidate->get_CurrentAutomationId(&candidateId))) continue;
            const QString text = QString::fromWCharArray(candidateId);
            SysFreeString(candidateId);
            if (text == suffix || text.endsWith(QChar('.') + suffix))
            { button = candidate; ++matches; }
        }
        if (matches != 1) button.Reset();
    }
    CONTROLTYPEID type = 0;
    if (FAILED(hr) || !button || FAILED(button->get_CurrentControlType(&type)) || type != UIA_ButtonControlTypeId)
    {
        error = QStringLiteral("未找到顶部当前任务面板按钮：%1").arg(QString::fromWCharArray(id));
        return false;
    }
    ComPtr<IUIAutomationInvokePattern> pattern;
    if (FAILED(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
    {
        error = QStringLiteral("顶部任务按钮不支持 Invoke 调用：%1").arg(QString::fromWCharArray(id));
        return false;
    }
    return true;
}

enum class Invocation { Invoked, Retry, Failed };
Invocation invoke(IUIAutomationElement* button, QString& error, const std::function<bool()>& canceled)
{
    BOOL enabled = FALSE;
    if (FAILED(button->get_CurrentIsEnabled(&enabled)))
    {
        error = QStringLiteral("无法确认按钮调用前的状态");
        return Invocation::Failed;
    }
    if (!enabled) return Invocation::Retry;
    ComPtr<IUIAutomationInvokePattern> pattern;
    HRESULT hr = button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern));
    if (canceled()) return Invocation::Failed;
    if (SUCCEEDED(hr) && pattern) hr = pattern->Invoke();
    else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
    if (hr == UIA_E_ELEMENTNOTENABLED) return Invocation::Retry;
    if (FAILED(hr))
    {
        error = QStringLiteral("按钮 Invoke 调用失败：0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
        return Invocation::Failed;
    }
    return Invocation::Invoked;
}

ComPtr<IUIAutomationElement> findControl(IUIAutomation* automation, IUIAutomationElement* root,
    const QString& suffix, CONTROLTYPEID type, QString& error)
{
    VARIANT value{}; value.vt = VT_I4; value.lVal = type;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition))
        || FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements)) || !elements)
    { error = QStringLiteral("无法读取上架数量控件：%1").arg(suffix); return {}; }
    int count = 0, matches = 0;
    elements->get_Length(&count);
    ComPtr<IUIAutomationElement> found;
    for (int index = 0; index < count; ++index)
    {
        ComPtr<IUIAutomationElement> element;
        BSTR id = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || !element
            || FAILED(element->get_CurrentAutomationId(&id))) continue;
        const QString name = id ? QString::fromWCharArray(id) : QString();
        SysFreeString(id);
        if (name == suffix || name.endsWith(QChar('.') + suffix)) { found = element; ++matches; }
    }
    if (matches != 1)
    { error = QStringLiteral("上架数量控件不存在或不唯一：%1").arg(suffix); return {}; }
    return found;
}

bool parseQuantity(QString text, qint64& quantity)
{
    text = text.trimmed();
    // Accept the integer label, optionally formatted with thousands separators.
    static const QRegularExpression number(QStringLiteral("^(?:[0-9]+|[0-9]{1,3}(?:[,， ][0-9]{3})+)$"));
    if (!number.match(text).hasMatch()) return false;
    text.remove(','); text.remove(QChar(0xff0c)); text.remove(' ');
    bool ok = false;
    quantity = text.toLongLong(&ok);
    return ok && quantity >= 0;
}

bool readQuantity(IUIAutomationElement* element, qint64& quantity, QString& error, bool* pending = nullptr)
{
    if (pending) *pending = false;
    BSTR text = nullptr;
    const HRESULT read = element->get_CurrentName(&text);
    const QString actual = text ? QString::fromWCharArray(text) : QString();
    SysFreeString(text);
    if (FAILED(read)) { error = QStringLiteral("无法读取上架数量文字"); return false; }
    if (!parseQuantity(actual, quantity))
    {
        const QString trimmed = actual.trimmed();
        if (pending) *pending = trimmed.isEmpty() || trimmed == QStringLiteral("-") || trimmed == QStringLiteral("—");
        error = QStringLiteral("上架数量尚未形成有效数字");
        return false;
    }
    return true;
}

bool readPurchasedQuantity(IUIAutomation* automation, IUIAutomationElement* root, qint64& quantity, QString& error)
{
    auto confirmed = findControl(automation, root, QStringLiteral("task-panel.confirmed"), UIA_TextControlTypeId, error);
    if (!confirmed) return false;
    if (!readQuantity(confirmed.Get(), quantity, error) || quantity <= 0)
    { error = QStringLiteral("无法读取本轮实际成交数量或数量不大于零，未开始上架"); return false; }
    return true;
}

struct Result { bool success = false; bool ready = false; bool canceled = false; QString text; };
}

AuctionControlAutomation::AuctionControlAutomation(QObject* parent) : QObject(parent) {}
AuctionControlAutomation::~AuctionControlAutomation() { shutdown(); }

bool AuctionControlAutomation::isTargetWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length)
        && QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).fileName()
            .compare(QStringLiteral("AuctionQtControl.exe"), Qt::CaseInsensitive) == 0;
    CloseHandle(process);
    return ok;
}

bool AuctionControlAutomation::start(HWND hwnd, Mode mode, int requestId, const std::atomic_bool* externalCancel)
{
    if (worker_) return false;
    canceled_.store(false);
    auto result = std::make_shared<Result>();
    worker_ = QThread::create([this, hwnd, mode, externalCancel, result]() {
        const auto canceled = [&]() { return canceled_.load() || (externalCancel && externalCancel->load()); };
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) { result->text = QStringLiteral("UI Automation COM 初始化失败"); return; }
        {
            ComPtr<IUIAutomation> automation;
            const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            if (FAILED(hr)) result->text = QStringLiteral("UI Automation 初始化失败");
            else
            {
                ComPtr<IUIAutomation2> timeouts;
                if (SUCCEEDED(automation.As(&timeouts)))
                {
                    timeouts->put_ConnectionTimeout(1000);
                    timeouts->put_TransactionTimeout(1000);
                }
                AuctionRoundState state;
                AuctionCoinState coinState(mode == CoinListing
                    ? AuctionCoinState::Mode::Listing : AuctionCoinState::Mode::Execution);
                int previous = -1;
                qint64 expectedSellQuantity = 0;
                bool sellStarted = false, sellSawBusy = false, roundStopRequested = false;
                qint64 previousSellQuantity = -1;
                ComPtr<IUIAutomationElement> sellConfirmed, roundStop;
                QElapsedTimer prepareTimer;
                prepareTimer.start();
                ComPtr<IUIAutomationElement> root, buy, sell;
                while (!canceled())
                {
                    if (!isTargetWindow(hwnd)) { result->text = QStringLiteral("AuctionQtControl 绑定窗口已关闭或失效，请重新绑定"); break; }
                    BOOL buyEnabled = FALSE, sellEnabled = FALSE;
                    if (!buy || !sell)
                    {
                        root.Reset(); buy.Reset(); sell.Reset();
                        if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root)
                        { result->text = QStringLiteral("无法读取 AuctionQtControl 窗口控件"); break; }
                        if (!findButton(automation.Get(), root.Get(), BuyId, buy, result->text)
                            || !findButton(automation.Get(), root.Get(), SellId, sell, result->text)) break;
                    }
                    if (FAILED(buy->get_CurrentIsEnabled(&buyEnabled)) || FAILED(sell->get_CurrentIsEnabled(&sellEnabled)))
                    {
                        // Reacquire replaced Qt controls once before treating this as a failure.
                        root.Reset(); buy.Reset(); sell.Reset();
                        if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root
                            || !findButton(automation.Get(), root.Get(), BuyId, buy, result->text)
                            || !findButton(automation.Get(), root.Get(), SellId, sell, result->text)
                            || FAILED(buy->get_CurrentIsEnabled(&buyEnabled)) || FAILED(sell->get_CurrentIsEnabled(&sellEnabled)))
                        { if (result->text.isEmpty()) result->text = QStringLiteral("无法读取购买/上架按钮状态"); break; }
                    }
                    const int status = (buyEnabled ? 1 : 0) | (sellEnabled ? 2 : 0);
                    if (status != previous)
                    {
                        emit logAppended(QStringLiteral("AuctionQtControl 按钮状态：开始购买=%1，开始上架=%2")
                            .arg(buyEnabled ? QStringLiteral("可用") : QStringLiteral("不可用"),
                                sellEnabled ? QStringLiteral("可用") : QStringLiteral("不可用")));
                        previous = status;
                    }
                    if (mode == CheckReady || (mode == Prepare && status == 3))
                    {
                        result->success = true;
                        result->ready = status == 3;
                        result->text = result->ready ? QStringLiteral("两个按钮均可用，准备成功") : QStringLiteral("购买或上架按钮不可用，尚未准备");
                        break;
                    }
                    if (mode == Prepare && prepareTimer.elapsed() >= 5000)
                    {
                        result->text = QStringLiteral("5 秒内购买/上架按钮未同时恢复可用，准备检查失败；可等待 Qt 任务结束后重试或手动强制执行");
                        break;
                    }
                    // After invoking listing, quantity and stop acknowledgement own completion.
                    // The coin button-state shortcut must not finish while listing is still active.
                    if (sellStarted)
                    {
                        if (!sellEnabled) sellSawBusy = true;
                        if (!roundStopRequested)
                        {
                            if (!sellConfirmed)
                                sellConfirmed = findControl(automation.Get(), root.Get(), QStringLiteral("sell-task-panel.sell-confirmed"), UIA_TextControlTypeId, result->text);
                            if (!sellConfirmed) break;
                            qint64 listed = -1;
                            QString quantityError;
                            bool pending = false;
                            if (readQuantity(sellConfirmed.Get(), listed, quantityError, &pending))
                            {
                                if (listed != previousSellQuantity)
                                {
                                    emit logAppended(QStringLiteral("上架进度：已确认 %1 件，本轮实际成交目标 %2 件（忽略 Qt 分母）").arg(listed).arg(expectedSellQuantity));
                                    previousSellQuantity = listed;
                                }
                                if (sellSawBusy && listed == expectedSellQuantity)
                                {
                                    if (!roundStop)
                                        roundStop = findControl(automation.Get(), root.Get(), QStringLiteral("task-panel.cycle-stop"), UIA_ButtonControlTypeId, result->text);
                                    if (!roundStop) break;
                                    const auto stopped = invoke(roundStop.Get(), result->text, canceled);
                                    if (stopped == Invocation::Failed) break;
                                    if (stopped == Invocation::Invoked)
                                    {
                                        roundStopRequested = true;
                                        emit logAppended(QStringLiteral("上架数量达到本轮实际成交数 %1，已调用“停止”；等待两个开始按钮恢复可用").arg(expectedSellQuantity));
                                    }
                                    else if (buyEnabled && sellEnabled)
                                    {
                                        // A naturally finished task may already have disabled its stop button.
                                        result->success = result->ready = true;
                                        result->text = QStringLiteral("上架数量达到本轮实际成交数，任务已自然结束，两个开始按钮均可用，本轮完成");
                                        break;
                                    }
                                }
                            }
                            else if (!pending) { result->text = quantityError; break; }
                        }
                        else if (buyEnabled && sellEnabled)
                        {
                            result->success = result->ready = true;
                            result->text = QStringLiteral("上架数量达到本轮实际成交数，调用“停止”后两个开始按钮均已恢复可用，本轮完成");
                            break;
                        }
                        for (int i = 0; i < 10 && !canceled(); ++i) QThread::msleep(10);
                        continue;
                    }
                    if (mode == Round)
                    {
                        const auto action = state.observe(buyEnabled != FALSE, sellEnabled != FALSE);
                        if (action == AuctionRoundState::Action::Complete)
                        {
                            result->success = result->ready = true;
                            result->text = QStringLiteral("购买和上架已执行，两个按钮重新可用，本轮完成");
                            break;
                        }
                        if (action == AuctionRoundState::Action::Buy || action == AuctionRoundState::Action::Sell)
                        {
                            if (canceled()) break;
                            if (action == AuctionRoundState::Action::Sell
                                && !readPurchasedQuantity(automation.Get(), root.Get(), expectedSellQuantity, result->text)) break;
                            const auto called = invoke(action == AuctionRoundState::Action::Buy ? buy.Get() : sell.Get(), result->text, canceled);
                            if (called == Invocation::Failed) break;
                            if (called == Invocation::Invoked)
                            {
                                state.invoked(action);
                                if (action == AuctionRoundState::Action::Sell) sellStarted = true;
                                emit logAppended(action == AuctionRoundState::Action::Buy
                                    ? QStringLiteral("PC 老板已调用开始购买；等待执行状态及开始上架可用")
                                    : QStringLiteral("PC 老板已调用开始上架；等待执行状态及两个按钮重新可用"));
                            }
                        }
                    }
                    if (mode == CoinListing || mode == CoinExecution)
                    {
                        const auto action = coinState.observe(buyEnabled != FALSE, sellEnabled != FALSE);
                        if (action == AuctionCoinState::Action::Failed)
                        {
                            result->text = mode == CoinListing
                                ? QStringLiteral("开始购买按钮不可用，无法开始币号上架；请确认 Qt 当前任务已结束")
                                : QStringLiteral("开始上架按钮不可用，无法开始币号执行；请确认 Qt 当前任务已结束");
                            break;
                        }
                        if (action == AuctionCoinState::Action::Complete)
                        {
                            result->success = true;
                            result->text = mode == CoinListing
                                ? QStringLiteral("开始购买已执行，两个按钮均已恢复可用，上架成功")
                                : QStringLiteral("开始上架已执行，开始购买可用且开始上架不可用，执行成功");
                            break;
                        }
                        if (action == AuctionCoinState::Action::Invoke)
                        {
                            if (mode == CoinExecution
                                && !readPurchasedQuantity(automation.Get(), root.Get(), expectedSellQuantity, result->text)) break;
                            const auto called = invoke(mode == CoinListing ? buy.Get() : sell.Get(), result->text, canceled);
                            if (called == Invocation::Failed) break;
                            if (called == Invocation::Invoked)
                            {
                                coinState.invoked();
                                if (mode == CoinExecution) sellStarted = true;
                                emit logAppended(mode == CoinListing
                                    ? QStringLiteral("币号已调用开始购买，等待按钮状态变化")
                                    : QStringLiteral("币号已调用开始上架，等待按钮状态变化"));
                            }
                        }
                    }
                    // Small cancellable sleeps keep the UI responsive and stop subsequent invokes promptly.
                    for (int i = 0; i < 10 && !canceled(); ++i) QThread::msleep(10);
                }
            }
        }
        CoUninitialize();
        result->canceled = canceled();
        if (result->canceled) { result->success = result->ready = false; result->text = QStringLiteral("已停止 PC 老板按钮监控及后续调用"); }
    });
    QThread* worker = worker_;
    connect(worker, &QThread::finished, this, [this, worker, mode, requestId, result]() {
        worker->wait();
        worker_ = nullptr;
        worker->deleteLater();
        // A stop/role change can arrive after the worker computes its result
        // but before this queued finished callback reaches the UI thread.
        const bool canceled = result->canceled || canceled_.load();
        emit completed(mode, requestId, result->success && !canceled, result->ready && !canceled, canceled,
            canceled ? QStringLiteral("已停止 PC 老板按钮监控及后续调用") : result->text);
    });
    worker_->start();
    return true;
}

void AuctionControlAutomation::cancel() { canceled_.store(true); }
void AuctionControlAutomation::shutdown()
{
    cancel();
    if (!worker_) return;
    disconnect(worker_, nullptr, this, nullptr);
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
}
