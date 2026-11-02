#include <winsock2.h>
#include <QtWidgets>
#include <cstdio>
#include <functional>
#include <thread>
#include <mutex>
#define private public
#include "../DF_PlayClick_Client_Qt/ClientController.h"
#undef private
#include "../DF_PlayClick_Client_Qt/ClientController.cpp"

static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
    std::printf("PASS %s\n", message);
}
static bool until(const std::function<bool()>& predicate, int timeout = 60000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout)
    { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(2); }
    return predicate();
}
static QWidget* child(QWidget* parent, const QString& name)
{
    auto* widget = new QWidget(parent); widget->setObjectName(name);
    auto* compact = new QVBoxLayout(widget); compact->setContentsMargins(0,0,0,0); compact->setSpacing(4);
    parent->layout()->addWidget(widget); return widget;
}

class TrackedCombo : public QComboBox
{
public:
    using QComboBox::QComboBox;
    static int popups;
    static int visiblePopups;
    static bool blockWheel;
    void wheelEvent(QWheelEvent* event) override
    { if (blockWheel) event->ignore(); else QComboBox::wheelEvent(event); }
    void showPopup() override
    {
        ++popups; QComboBox::showPopup();
        const auto popup = reinterpret_cast<HWND>(view()->window()->winId());
        COLORREF color = 0; BYTE alpha = 255; DWORD flags = 0;
        if (!GetLayeredWindowAttributes(popup, &color, &alpha, &flags) || !(flags & LWA_ALPHA) || alpha != 0)
            ++visiblePopups;
    }
};
int TrackedCombo::popups = 0;
int TrackedCombo::visiblePopups = 0;
bool TrackedCombo::blockWheel = false;

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv); app.setObjectName(QStringLiteral("QApplication"));
    if (argc == 3 && QString::fromLatin1(argv[1]) == QStringLiteral("--read-catalog"))
    {
        AuctionPurchaseAutomation reader;
        bool done = false, success = false;
        QString payload, error;
        QObject::connect(&reader, &AuctionPurchaseAutomation::completed,
            [&](int, bool ok, const QString& catalog, const QString& text) { done = true; success = ok; payload = catalog; error = text; });
        const auto hwnd = reinterpret_cast<HWND>(QString::fromLatin1(argv[2]).toULongLong());
        require(reader.readCatalog(hwnd) && until([&]() { return done; }, 60000), "live Qt catalog read completes");
        if (!success) std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        QJsonObject json; AuctionPurchase::Catalog catalog;
        require(success && AuctionPurchase::decode(payload, json) && AuctionPurchase::Catalog::fromJson(json, catalog), "live AuctionQtControl lists read by production UIA implementation");
        std::printf("PASS live Qt: %d modes; ammunition counts", int(catalog.purchaseModes.size()));
        for (const auto& names : catalog.ammunition) std::printf(" %d", int(names.size()));
        std::puts("");
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-desktop"))) {
        wchar_t desktopName[256]{}; DWORD needed = 0;
        require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            desktopName, sizeof(desktopName), &needed)
            && QString::fromWCharArray(desktopName).startsWith(QStringLiteral("CodexQtTest_")), "fixture runs on separate desktop without physical mouse interference");
    }
    QWidget dashboard; dashboard.setObjectName(QStringLiteral("dashboard")); new QVBoxLayout(&dashboard);
    auto* rule = child(&dashboard, QStringLiteral("rule-panel"));
    auto* modeLayer = child(rule, QStringLiteral("QWidget"));
    auto* mode = new TrackedCombo(modeLayer); mode->setObjectName(QStringLiteral("priceMode"));
    mode->addItems(AuctionPurchase::modes()); mode->setCurrentIndex(2); modeLayer->layout()->addWidget(mode);
    auto* picker = child(rule, QStringLiteral("ammo-picker"));
    std::array<QCheckBox*, 5> checks;
    std::array<QComboBox*, 5> combos;
    std::array<QLineEdit*, 5> edits;
    for (int index = 0; index < 5; ++index)
    {
        auto* layer = child(picker, QStringLiteral("QWidget"));
        checks[index] = new QCheckBox(QStringLiteral("购买第 %1 款").arg(index + 1), layer);
        checks[index]->setObjectName(QStringLiteral("ammo-slot-check-%1").arg(index + 1));
        checks[index]->setChecked(index < 2); layer->layout()->addWidget(checks[index]);
        auto* sub = child(layer, QStringLiteral("QWidget"));
        combos[index] = new TrackedCombo(sub);
        combos[index]->setObjectName(QStringLiteral("ammo-item") + (index ? QStringLiteral("-%1").arg(index + 1) : QString()));
        combos[index]->addItems({QStringLiteral("原子弹 [%1]").arg(index + 1), QStringLiteral("目标弹药 %1 [37050%1]").arg(index + 1)});
        sub->layout()->addWidget(combos[index]);
        edits[index] = new QLineEdit(QStringLiteral("7"), sub);
        edits[index]->setObjectName(QStringLiteral("buy-threshold-%1").arg(index + 1));
        edits[index]->setEnabled(false); sub->layout()->addWidget(edits[index]);
    }
    QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), [&](int index) {
        for (auto* edit : edits) edit->setEnabled(index != 2);
    });
    auto* task = child(&dashboard, QStringLiteral("task-panel"));
    auto* savePurchase = new QPushButton(QStringLiteral("保存购买配置"), task);
    savePurchase->setObjectName(QStringLiteral("task-save-buy")); task->layout()->addWidget(savePurchase);
    auto* startBuy = new QPushButton(QStringLiteral("开始购买"), task);
    startBuy->setObjectName(QStringLiteral("start")); task->layout()->addWidget(startBuy);
    startBuy->setEnabled(false);
    int saves = 0;
    bool restoreBuy = true;
    QObject::connect(savePurchase, &QPushButton::clicked, [&]() {
        ++saves;
        startBuy->setEnabled(false);
        if (restoreBuy) QTimer::singleShot(350, startBuy, [=]() { startBuy->setEnabled(true); });
    });
    dashboard.resize(500, 900); dashboard.show();
    QElapsedTimer settled; settled.start();
    until([&]() { return settled.elapsed() >= 300; });
    const HWND hwnd = reinterpret_cast<HWND>(dashboard.winId());
    AuctionPurchaseAutomation automation;
    bool done = false, ok = false; QString data, message;
    QObject::connect(&automation, &AuctionPurchaseAutomation::completed,
        [&](int, bool success, const QString& catalog, const QString& text) { done = true; ok = success; data = catalog; message = text; });
    if (qEnvironmentVariableIsSet("CODEX_SAVE_READY_TEST"))
    {
        AuctionPurchase::Config config; config.selected = 1;
        config.entries[0] = {combos[0]->currentText(), QStringLiteral("1200")};
        for (int index : {2, 0})
        {
            config.mode = AuctionPurchase::modes()[index]; done = ok = false;
            startBuy->setEnabled(false);
            const int before = saves;
            require(automation.apply(hwnd, index + 1, config) && until([&]() { return done; }), "save/readiness gate finishes");
            if (!ok) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
            require(ok && saves == before + 1 && startBuy->isEnabled(), "threshold and non-threshold sync save once and wait for purchase readiness");
        }
        done = ok = false; restoreBuy = false;
        startBuy->setEnabled(false);
        const auto cancelConnection = QObject::connect(savePurchase, &QPushButton::clicked,
            [&]() { QTimer::singleShot(200, &automation, &AuctionPurchaseAutomation::cancel); });
        require(automation.apply(hwnd, 5, config) && until([&]() { return done; }), "waiting sync remains cancellable");
        require(!ok && !startBuy->isEnabled(), "disabled purchase never reports sync success");
        QObject::disconnect(cancelConnection);
        return 0;
    }
    if (qEnvironmentVariableIsSet("CODEX_FOCUSED_SYNC_TEST"))
    {
        require(automation.readCatalog(hwnd) && until([&]() { return done; }) && ok, "prime popup handles from bound Qt catalog");
        done = ok = false;
        const int visibleBefore = TrackedCombo::visiblePopups;
        const int popupsBefore = TrackedCombo::popups;
        TrackedCombo::blockWheel = true;
        const HWND foreground = GetForegroundWindow();
        AuctionPurchase::Config config; config.selected = 3; config.mode = AuctionPurchase::modes()[0];
        config.entries[2] = {combos[2]->itemText(1), QStringLiteral("1250.5")};
        require(automation.apply(hwnd, 1, config) && until([&]() { return done; }), "background sync completes");
        if (!ok) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        require(ok && mode->currentIndex() == 0 && combos[2]->currentIndex() == 1, "background mode and ammunition changes committed");
        require(TrackedCombo::visiblePopups == visibleBefore && TrackedCombo::popups > popupsBefore && GetForegroundWindow() == foreground, "wheel-blocked combos synchronize with invisible popups and unchanged foreground");
        require(edits[2]->text() == QStringLiteral("1250.5"), "threshold synchronized");
        done = ok = false; config.mode = AuctionPurchase::modes()[2]; config.entries[2].ammo = combos[2]->itemText(0);
        require(automation.apply(hwnd, 2, config) && until([&]() { return done; }) && ok, "reverse mode and ammo switch succeeds");
        require(mode->currentIndex() == 2 && combos[2]->currentIndex() == 0 && TrackedCombo::visiblePopups == visibleBefore, "reverse switch also has no visible popup");
        return 0;
    }
    require(automation.readCatalog(hwnd), "start real Qt UIA catalog read");
    require(until([&]() { return done; }), "catalog worker finishes without blocking GUI");
    if (!ok) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    require(ok, "catalog read succeeds");
    QJsonObject json; AuctionPurchase::Catalog catalog;
    require(AuctionPurchase::decode(data, json) && AuctionPurchase::Catalog::fromJson(json, catalog), "catalog wire JSON round trip");
    require(catalog.purchaseModes == AuctionPurchase::modes() && catalog.ammunition[4] == QStringList({QStringLiteral("原子弹 [5]"), QStringLiteral("目标弹药 5 [370505]")}), "all five lists equal actual Qt contents");
    require(mode->currentIndex() == 2 && checks[0]->isChecked() && checks[1]->isChecked() && combos[4]->currentIndex() == 0, "catalog never changes selections");

    AuctionPurchase::Config config; config.selected = 3; config.mode = AuctionPurchase::modes()[0];
    config.entries[2] = {combos[2]->itemText(1), QStringLiteral("1250.5")};
    auto run = [&](int revision) { done = ok = false; require(automation.apply(hwnd, revision, config), "start Qt parameter apply"); require(until([&]() { return done; }), "apply worker finishes"); if (!ok) std::fprintf(stderr, "%s\n", message.toUtf8().constData()); };
    run(1);
    require(ok && mode->currentIndex() == 0 && combos[2]->currentIndex() == 1 && edits[2]->text() == QStringLiteral("1250.5"), "UIA selects mode, ammo and threshold and reads back");
    for (int index = 0; index < 5; ++index) require(checks[index]->isChecked() == (index == 2), "only selected checkbox enabled");
    config.mode = AuctionPurchase::modes()[2]; run(2);
    require(ok && !edits[2]->isEnabled() && edits[2]->text() == QStringLiteral("1250.5"), "returning to native mode preserves threshold");
    config.mode = AuctionPurchase::modes()[0]; run(3);
    require(ok && edits[2]->isEnabled() && edits[2]->text() == QStringLiteral("1250.5"), "switching back restores usable remembered threshold");
    config.entries[2].ammo = QStringLiteral("不存在的子弹"); run(4);
    require(!ok, "missing target item fails without fake acknowledgement");
    for (int index = 0; index < 80; ++index) combos[4]->addItem(QStringLiteral("测试弹药 %1").arg(index));
    QCoreApplication::processEvents();
    config.selected = 5; config.entries[4] = {combos[4]->itemText(combos[4]->count() - 1), QStringLiteral("1500")};
    run(5);
    require(ok && combos[4]->currentIndex() == combos[4]->count() - 1 && edits[4]->text() == QStringLiteral("1500"), "select offscreen item in long Qt dropdown without foreground mouse input");
    done = ok = false;
    require(automation.readCatalog(hwnd) && until([&]() { return done; }, 60000), "read long dropdown from last selected row");
    json = {}; catalog = {};
    require(ok && AuctionPurchase::decode(data, json) && AuctionPurchase::Catalog::fromJson(json, catalog)
        && catalog.ammunition[4].size() == combos[4]->count()
        && catalog.ammunition[4].last() == combos[4]->itemText(combos[4]->count() - 1), "catalog includes all 82 rows, in original order");
    for (int index = 0; index < combos[4]->count(); ++index)
        require(catalog.ammunition[4][index] == combos[4]->itemText(index), "long list matches every Qt model row");
    require(combos[4]->currentIndex() == combos[4]->count() - 1, "reading long list preserves committed selection");
    config.selected = 3;

    {
        ClientController controller;
        controller.currentRole_ = ClientRole::Coin; controller.auctionHwnd_ = hwnd;
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "open isolated loopback listener");
        int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect test client");
        SOCKET server = accept(listener, nullptr, nullptr); closesocket(listener);
        u_long nonblocking = 1; ioctlsocket(server, FIONBIO, &nonblocking);
        controller.connection_.socket_ = client; controller.connection_.connected_.store(true);
        auto drain = [&]() { QByteArray wire; char buffer[8192]; int size; while ((size = recv(server, buffer, sizeof(buffer), 0)) > 0) wire.append(buffer, size); return wire; };
        controller.automationActionRunning_ = true;
        config.entries[2].ammo = combos[2]->itemText(1);
        config.entries[2].threshold = QStringLiteral("100");
        controller.handleAuctionPurchaseConfig(10, AuctionPurchase::encode(config.toJson()));
        config.entries[2].threshold = QStringLiteral("200");
        controller.handleAuctionPurchaseConfig(11, AuctionPurchase::encode(config.toJson()));
        require(controller.pendingAuctionPurchaseRevision_ == 11 && !controller.auctionPurchaseAutomation_.busy() && edits[2]->text() == QStringLiteral("1250.5"), "running round queues only latest config and does not mutate Qt");
        require(drain().contains("AUCTION_CONFIG_RESULT 11 2 "), "pending update acknowledged over socket");
        controller.finishAutomationAction(); controller.processAuctionPurchaseSync();
        require(until([&]() { return !controller.auctionPurchaseAutomation_.busy(); }), "deferred apply completes after round ends");
        require(edits[2]->text() == QStringLiteral("200") && drain().contains("AUCTION_CONFIG_RESULT 11 1 "), "only newest config applied with success acknowledgement");
        controller.automationActionRunning_ = true;
        config.entries[2].threshold = QStringLiteral("300");
        controller.handleAuctionPurchaseConfig(12, AuctionPurchase::encode(config.toJson()));
        controller.forceStopCurrentAction(); controller.finishAutomationAction(); controller.processAuctionPurchaseSync();
        require(controller.pendingAuctionPurchaseRevision_ == 0 && !controller.auctionPurchaseAutomation_.busy() && edits[2]->text() == QStringLiteral("200"), "stop clears deferred config and prevents late writes");
        controller.connection_.connected_.store(false); controller.connection_.socket_ = INVALID_SOCKET;
        closesocket(client); closesocket(server);
    }
    std::puts("ALL AUCTION PURCHASE AUTOMATION TESTS PASSED");
    return 0;
}
