#include <winsock2.h>
#include <QtWidgets>
#include "../DF_PlayClick_Client_Qt/AuctionControlAutomation.h"
#include <cstdio>
#include <functional>
#include <basetyps.h>
#include <uiautomation.h>
#include <wrl/client.h>
#ifdef AUCTION_CONTROLLER_RUNTIME
#include <thread>
#include <mutex>
#define private public
#include "../DF_PlayClick_Client_Qt/ClientController.h"
#undef private
#include "../DF_PlayClick_Client_Qt/ClientController.cpp"
#endif

static void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
static bool until(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout)
    { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(2); }
    return predicate();
}
static QWidget* child(QWidget* parent, const QString& name)
{
    auto* widget = new QWidget(parent);
    widget->setObjectName(name);
    new QVBoxLayout(widget);
    parent->layout()->addWidget(widget);
    return widget;
}

static void dumpTestButtonIds(HWND hwnd)
{
    std::atomic_bool done{false};
    QThread* thread = QThread::create([&]() {
        using Microsoft::WRL::ComPtr;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IUIAutomation> automation;
            CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            ComPtr<IUIAutomationElement> root;
            automation->ElementFromHandle(hwnd, &root);
            VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_ButtonControlTypeId;
            ComPtr<IUIAutomationCondition> condition;
            automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
            ComPtr<IUIAutomationElementArray> buttons;
            root->FindAll(TreeScope_Descendants, condition.Get(), &buttons);
            int count = 0; if (buttons) buttons->get_Length(&count);
            for (int i = 0; i < count; ++i)
            {
                ComPtr<IUIAutomationElement> button; buttons->GetElement(i, &button);
                BSTR id = nullptr; button->get_CurrentAutomationId(&id);
                std::fprintf(stderr, "TEST BUTTON ID: %s\n", QString::fromWCharArray(id).toUtf8().constData());
                SysFreeString(id);
            }
        }
        CoUninitialize(); done.store(true);
    });
    thread->start(); until([&]() { return done.load(); }); thread->wait(); delete thread;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setObjectName(QStringLiteral("QApplication"));
    struct TestWindow : QWidget { using QWidget::destroy; };
    TestWindow dashboard; dashboard.setObjectName(QStringLiteral("dashboard"));
    auto* layout = new QVBoxLayout(&dashboard);
    auto* scroll = new QScrollArea(&dashboard); scroll->setObjectName(QStringLiteral("dashboard-scroll"));
    scroll->viewport()->setObjectName(QStringLiteral("qt_scrollarea_viewport"));
    layout->addWidget(scroll);
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("scroll-content")); new QVBoxLayout(content);
    scroll->setWidget(content); scroll->setWidgetResizable(true);
    auto* shell = child(content, QStringLiteral("shell"));
    auto* layer1 = child(shell, QString());
    auto* layer2 = child(layer1, QString());
    auto* panel = child(layer2, QStringLiteral("task-panel"));
    auto* buy = new QPushButton(QStringLiteral("开始购买 →"), panel); buy->setObjectName(QStringLiteral("start"));
    auto* sell = new QPushButton(QStringLiteral("开始上架 →"), panel); sell->setObjectName(QStringLiteral("sell-start-quick"));
    panel->layout()->addWidget(buy); panel->layout()->addWidget(sell);
    auto* wrongPanel = child(shell, QStringLiteral("sell-task-panel"));
    auto* wrongSell = new QPushButton(QStringLiteral("开始上架 →"), wrongPanel); wrongSell->setObjectName(QStringLiteral("sell-start"));
    wrongPanel->layout()->addWidget(wrongSell);
    int purchases = 0, listings = 0, wrongListings = 0;
    QObject::connect(buy, &QPushButton::clicked, [&]() {
        ++purchases; buy->setEnabled(false); sell->setEnabled(false);
        QTimer::singleShot(1000, &dashboard, [=]() { sell->setEnabled(true); });
    });
    QObject::connect(sell, &QPushButton::clicked, [&]() {
        ++listings; sell->setEnabled(false);
        QTimer::singleShot(1000, &dashboard, [=]() { buy->setEnabled(true); sell->setEnabled(true); });
    });
    QObject::connect(wrongSell, &QPushButton::clicked, [&]() { ++wrongListings; });
    dashboard.resize(600, 600); dashboard.show();
    const HWND hwnd = reinterpret_cast<HWND>(dashboard.winId());
    AuctionControlAutomation automation;
    std::atomic_bool cancel{false};
    bool finished = false, success = false, ready = false, canceled = false;
    QString text;
    QObject::connect(&automation, &AuctionControlAutomation::logAppended, &app, [&](const QString& message) {
        std::puts(message.toUtf8().constData());
    }, Qt::QueuedConnection);
    QObject::connect(&automation, &AuctionControlAutomation::completed,
        [&](int, int, bool ok, bool isReady, bool wasCanceled, const QString& message) {
            finished = true; success = ok; ready = isReady; canceled = wasCanceled; text = message;
        });
    auto run = [&](AuctionControlAutomation::Mode mode) {
        finished = success = ready = canceled = false;
        require(automation.start(hwnd, mode, 42, &cancel), "start worker");
    };
    run(AuctionControlAutomation::CheckReady);
    require(until([&]() { return finished; }), "ready check finishes");
    if (!success) { std::fprintf(stderr, "%s\n", text.toUtf8().constData()); dumpTestButtonIds(hwnd); }
    require(success && ready, "actual Qt UIA finds top buttons and enabled state");
    require(purchases == 0 && listings == 0, "readiness never invokes buttons");
    buy->setEnabled(false);
    run(AuctionControlAutomation::CheckReady);
    require(until([&]() { return finished; }) && success && !ready, "disabled button is not ready");
    run(AuctionControlAutomation::Prepare);
    QTimer::singleShot(300, &dashboard, [=]() { buy->setEnabled(true); });
    require(until([&]() { return finished; }) && success && ready, "prepare waits until both enabled without invokes");
    require(purchases == 0 && listings == 0, "prepare never invokes");
    buy->setEnabled(false);
    run(AuctionControlAutomation::Round);
    require(!automation.start(hwnd, AuctionControlAutomation::Round, 43, &cancel), "reject duplicate worker");
    QTimer::singleShot(300, &dashboard, [=]() { buy->setEnabled(true); });
    require(until([&]() { return finished; }) && success && ready, "real UIA Invoke completes purchase then listing");
    require(purchases == 1 && listings == 1 && wrongListings == 0, "invoke only correct top controls once");
    int ticks = 0;
    QTimer heartbeat; heartbeat.setInterval(10); QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; }); heartbeat.start();
    buy->setEnabled(false);
    run(AuctionControlAutomation::Round);
    QTimer::singleShot(250, &dashboard, [&]() { cancel.store(true); });
    require(until([&]() { return finished; }) && canceled && !success, "immediate cancellation while waiting");
    require(purchases == 1 && listings == 1 && ticks > 10, "no late invokes and GUI heartbeat stays responsive");
    cancel.store(false); buy->setEnabled(true);
    run(AuctionControlAutomation::Round);
    require(until([&]() { return purchases == 2; }), "purchase invoked before testing cancellation");
    cancel.store(true);
    require(until([&]() { return finished; }) && canceled, "cancel after purchase before listing");
    require(purchases == 2 && listings == 1, "cancel prevents later listing");
    cancel.store(false);
#ifdef AUCTION_CONTROLLER_RUNTIME
    {
        // Route the production controller through real Qt controls and an
        // isolated loopback socket. No OCR service or mouse input is available.
        buy->setEnabled(true); sell->setEnabled(true);
        ClientController controller;
        controller.currentRole_ = ClientRole::Boss;
        controller.auctionHwnd_ = hwnd;
        controller.boundHwnd_ = nullptr;
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback bind");
        require(listen(listener, 1) == 0, "loopback listen");
        int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback connect");
        SOCKET server = accept(listener, nullptr, nullptr); closesocket(listener);
        u_long nonblocking = 1; ioctlsocket(server, FIONBIO, &nonblocking);
        controller.connection_.socket_ = client;
        controller.connection_.connected_.store(true);
        auto drain = [&]() {
            QByteArray wire; char buffer[8192]; int size;
            while ((size = recv(server, buffer, sizeof(buffer), 0)) > 0) wire.append(buffer, size);
            return wire;
        };
        require(controller.boundHwnd() == hwnd && controller.isPcAuctionBoss(), "boss binds auction with no game window");
        controller.handlePrepareRequest(42);
        require(until([&]() { return !controller.auctionAutomation_.busy(); }), "production prepare finishes");
        QByteArray wire = drain();
        require(wire.startsWith("PREPARE_RESPONSE 42 OK ") && !wire.contains("TEAM_READY")
            && !wire.contains("AMOUNT_REPORT"), "prepare OK without amount or trailing ready lock reset");
        controller.handleTeamReadyRequest();
        require(until([&]() { return !controller.auctionAutomation_.busy(); }), "button readiness query finishes");
        wire = drain(); require(wire.contains("TEAM_READY 1 1 1"), "two enabled buttons report 1/1");
        const int beforeBuy = purchases, beforeSell = listings;
        controller.startCountdown(0, QString());
        controller.startCountdown(0, QString());
        controller.handleTeamReadyRequest();
        require(until([&]() { return !controller.auctionAutomation_.busy(); }, 10000), "production trigger completes");
        wire = drain();
        require(purchases == beforeBuy + 1 && listings == beforeSell + 1 && wrongListings == 0,
            "PC boss routes to UIA only and duplicate trigger is ignored");
        require(wire.contains("EXECUTION_RESPONSE OK ") && wire.contains("TEAM_READY 1 1 1")
            && !wire.contains("AMOUNT_REPORT") && wire.count("TEAM_READY") == 1,
            "single completion readiness and no money report or mid-round ready response");
        buy->setEnabled(false);
        controller.startCountdown(0, QString());
        QTimer::singleShot(150, &app, [&]() { controller.forceStopCurrentAction(); });
        require(until([&]() { return !controller.auctionAutomation_.busy(); }), "force stop releases worker");
        wire = drain();
        require(wire.contains("FORCE_STOP_ACTION_ACK") && wire.contains("TEAM_READY 0 0 1"), "stop ack and readiness cleared");
        controller.currentRole_ = ClientRole::Coin;
        require(!controller.isPcAuctionBoss() && controller.boundHwnd() == nullptr, "coin keeps original HWND backend");
        controller.currentRole_ = ClientRole::Boss; controller.externalBossSession_ = true;
        require(!controller.isPcAuctionBoss(), "ADB boss never selects Auction backend");
        controller.shutdownForExit(); closesocket(server);
        std::puts("PASS production PC boss routing/protocol, no amount OCR, duplicate trigger, no mid-round readiness, force-stop ack, coin/ADB isolation");
    }
#endif
    dashboard.hide(); dashboard.destroy();
    finished = false;
    require(automation.start(hwnd, AuctionControlAutomation::Round, 44, &cancel), "start with stale handle");
    require(until([&]() { return finished; }) && !success, "closed target reports failure");
    std::puts("PASS actual Qt UIA lookup/state/Invoke, duplicate-name disambiguation, preparation, round, cancellation, GUI responsiveness, closed window");
}
