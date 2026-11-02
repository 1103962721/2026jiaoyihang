#include <winsock2.h>
#include <QtWidgets>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#define private public
#include "../DF_PlayClick_Server_Qt/MainWindow.h"
#undef private
#include "../Shared/Protocol.h"
#include "../DF_PlayClick_Server_Qt/MainWindow.cpp"
#include "../DF_PlayClick_Server_Qt/MainWindowPurchaseUi.cpp"
#include "../DF_PlayClick_Server_Qt/ServerConnectionManager.cpp"

static void require(bool ok, const char* text)
{
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", text); std::exit(1); }
    std::printf("PASS %s\n", text);
}
static bool until(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout)
    { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(2); }
    return predicate();
}
static QByteArray drain(SOCKET socket)
{
    QByteArray wire; char buffer[16384]; int size;
    while ((size = recv(socket, buffer, sizeof(buffer), 0)) > 0) wire.append(buffer, size);
    return wire;
}

static int testQtForceRestart(bool manualOnly = false)
{
    ServerController controller;
    auto& connections = controller.connectionManager_;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve restart test port");
    int length = sizeof(address); getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length); closesocket(probe);
    QString error;
    require(connections.startListening(ntohs(address.sin_port), error), "restart test server listens");
    std::array<SOCKET, 2> sockets;
    std::array<quint64, 2> ids;
    for (int index = 0; index < 2; ++index)
    {
        sockets[index] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(::connect(sockets[index], reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect restart client");
        SocketUtil::SendLine(sockets[index], Protocol::BuildHelloMessage("restart-" + std::to_string(index)));
        u_long nonblocking = 1; ioctlsocket(sockets[index], FIONBIO, &nonblocking);
        require(until([&]() { return connections.clients().size() == index + 1; }), "restart client registered");
        ids[index] = connections.clients()[index].id;
    }
    {
        std::lock_guard<std::mutex> guard(connections.mutex_);
        for (int index = 0; index < 2; ++index)
        {
            auto client = connections.findClientLocked(ids[index]);
            client->role = index == 0 ? ClientRole::Coin : ClientRole::Boss;
            client->auctionBound = client->ready = true;
            client->listingStatus = QStringLiteral("待执行");
        }
    }
    std::array<QByteArray, 2> wire;
    for (auto socket : sockets) drain(socket);
    int prompts = 0;
    QObject::connect(&controller, &ServerController::incompleteTriggerRequested, [&]() { ++prompts; });
    auto trigger = [&](bool force, const QString& delay = QStringLiteral("0")) {
        controller.triggerClients(QStringLiteral("0"), delay, true, false, {}, QStringLiteral("123"), force);
    };
    auto triggered = [&]() {
        return until([&]() {
            for (int index = 0; index < 2; ++index) wire[index] += drain(sockets[index]);
            return wire[0].contains("TRIGGER ") && wire[1].contains("TRIGGER ");
        });
    };
    auto clearWire = [&]() { for (int index = 0; index < 2; ++index) { drain(sockets[index]); wire[index].clear(); } };
    auto acknowledgeStop = [&]() {
        for (auto socket : sockets) SocketUtil::SendLine(socket, Protocol::BuildForceStopActionAckMessage());
        require(until([&]() { const auto clients = connections.clients(); return !clients[0].actionRunning && !clients[1].actionRunning; }), "stop acknowledgements release both locks");
        clearWire();
    };
    if (manualOnly)
    {
        MainWindow window;
        for (auto* combo : {window.purchaseModeCombo_, window.purchaseSlotCombo_, window.purchaseAmmoCombo_})
        {
            const QSignalBlocker blocker(combo);
            if (combo->count() < 2) combo->addItems({QStringLiteral("test1"), QStringLiteral("test2")});
            combo->setCurrentIndex(0);
            QWheelEvent wheel(QPointF(5, 5), QPointF(5, 5), {}, QPoint(0, -120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(combo, &wheel);
            require(combo->currentIndex() == 0, "server dropdown wheel cannot change selection");
        }
        require(connections.setClientManualSuccess(ids[0], error)
            && connections.setClientManualSuccess(ids[1], error), "coin and boss manual success accepted");
        const auto marked = connections.clients();
        require(marked[0].listingStatus == QStringLiteral("成功")
            && marked[1].teamReadyState == 1 && marked[1].manualReadyOverride, "manual success updates actual eligibility");
        trigger(false);
        require(triggered() && !controller.preparingExecution_ && !wire[1].contains("PREPARE_REQUEST"), "manual preparation bypasses next prepare and dispatches normal execution");
        require(!connections.clients()[1].manualReadyOverride, "manual preparation is consumed by execution");
        require(!connections.setClientManualSuccess(ids[0], error), "manual success cannot release running action");
        for (auto socket : sockets) closesocket(socket);
        connections.stopListening();
        return 0;
    }
    trigger(false);
    require(prompts == 1 && !controller.preparingExecution_, "normal execution retains listing eligibility rule");
    trigger(true);
    require(triggered() && !controller.preparingExecution_ && !wire[1].contains("PREPARE_REQUEST"), "forced execution includes pending coin and bypasses Qt boss preparation");
    controller.requestForceStopAllActions();
    clearWire();
    trigger(true);
    require(connections.clients()[0].runningAction == QStringLiteral("正在停止") && !drain(sockets[0]).contains("TRIGGER"), "force execution cannot overlap unfinished stop");
    SocketUtil::SendLine(sockets[1], Protocol::BuildPrepareResponseMessage(999, "ERROR", "stale prepare"));
    SocketUtil::SendLine(sockets[1], Protocol::BuildTeamReadyMessage(1, 1, 1));
    SocketUtil::SendLine(sockets[1], Protocol::BuildExecutionResponseMessage(false, "stale execution"));
    SocketUtil::SendLine(sockets[1], Protocol::BuildPingMessage());
    require(until([&]() { wire[1] += drain(sockets[1]); return wire[1].contains("PONG"); })
        && connections.clients()[1].runningAction == QStringLiteral("正在停止"), "old completion reports cannot unlock pending stop");
    acknowledgeStop();
    trigger(true);
    require(triggered(), "execution works again after stop without relisting");
    clearWire();
    SocketUtil::SendLine(sockets[1], Protocol::BuildForceStopActionAckMessage());
    SocketUtil::SendLine(sockets[1], Protocol::BuildPingMessage());
    require(until([&]() { wire[1] += drain(sockets[1]); return wire[1].contains("PONG"); })
        && connections.clients()[1].runningAction == QStringLiteral("执行中"), "duplicate stop acknowledgement cannot reset new round");
    controller.requestForceStopAllActions(); acknowledgeStop();
    trigger(true, QStringLiteral("200"));
    require(connections.clients()[0].actionRunning && !connections.clients()[1].actionRunning, "second group awaits configured delay");
    controller.requestForceStopAllActions(); acknowledgeStop();
    bool elapsed = false; QTimer::singleShot(300, [&]() { elapsed = true; });
    require(until([&]() { return elapsed; }) && !drain(sockets[1]).contains("TRIGGER"), "stop cancels delayed second-group dispatch");
    connections.setClientListingStatus(ids[0], QStringLiteral("成功"));
    trigger(false);
    require(controller.preparingExecution_ && controller.prepareExecutionTimer_.isActive(), "ordinary execution retains bounded preparation");
    const int oldRequest = controller.pendingPrepareRequests_.key(ids[1]);
    controller.prepareExecutionTimer_.start(20);
    require(until([&]() { return !controller.preparingExecution_; }) && connections.clients()[1].runningAction == QStringLiteral("正在停止"), "preparation watchdog cancels and stops stalled check");
    SocketUtil::SendLine(sockets[1], Protocol::BuildForceStopActionAckMessage());
    require(until([&]() { return !connections.clients()[1].actionRunning; }), "timed out preparation can be retried after stop");
    clearWire(); trigger(true);
    require(triggered(), "force execution recovers after preparation timeout");
    SocketUtil::SendLine(sockets[1], Protocol::BuildPrepareResponseMessage(oldRequest, "OK", "late prepare"));
    SocketUtil::SendLine(sockets[1], Protocol::BuildPingMessage()); wire[1].clear();
    require(until([&]() { wire[1] += drain(sockets[1]); return wire[1].contains("PONG"); })
        && connections.clients()[1].runningAction == QStringLiteral("执行中"), "late preparation result cannot clear new execution");
    for (auto socket : sockets) { shutdown(socket, SD_BOTH); closesocket(socket); }
    connections.stopListening();
    std::puts("QT FORCE RESTART TESTS PASSED");
    return 0;
}

static int testQtAutoRelist()
{
    ServerController controller;
    auto& connections = controller.connectionManager_;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve relist test port");
    int length = sizeof(address); getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length); closesocket(probe);
    QString error;
    require(connections.startListening(ntohs(address.sin_port), error), "relist server listens");
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect Qt coin");
    SocketUtil::SendLine(socket, Protocol::BuildHelloMessage("qt-auto-loop"));
    u_long nonblocking = 1; ioctlsocket(socket, FIONBIO, &nonblocking);
    require(until([&]() { return connections.clients().size() == 1; }), "coin registered");
    const quint64 id = connections.clients()[0].id;
    const auto execution = [&](bool automatic, bool qt = true) {
        controller.autoExecuteEnabled_ = automatic;
        std::lock_guard<std::mutex> lock(connections.mutex_);
        auto client = connections.findClientLocked(id);
        client->role = ClientRole::Coin; client->ready = true; client->auctionBound = qt;
        client->actionRunning = true; client->runningAction = QStringLiteral("执行中");
    };
    QByteArray wire;
    const auto waitListing = [&]() {
        return until([&]() { wire += drain(socket); return wire.contains("LISTING_REQUEST "); });
    };
    drain(socket);
    execution(true);
    SocketUtil::SendLine(socket, Protocol::BuildExecutionResponseMessage(true, "round complete"));
    require(waitListing(), "execution success automatically sends next Qt listing");
    const int firstRequest = wire.mid(wire.indexOf("LISTING_REQUEST ")).split(' ')[1].toInt();
    require(firstRequest > 0 && connections.clients()[0].runningAction == QStringLiteral("上架"), "next listing has its own request and action lock");
    wire.clear();
    SocketUtil::SendLine(socket, Protocol::BuildExecutionResponseMessage(true, "duplicate"));
    require(until([&]() { return connections.clients()[0].executionStatus.contains(QStringLiteral("duplicate")); }), "duplicate success received");
    require(connections.clients()[0].actionRunning && connections.clients()[0].runningAction == QStringLiteral("上架")
        && !drain(socket).contains("LISTING_REQUEST "), "duplicate cannot clear listing or start another purchase");
    SocketUtil::SendLine(socket, Protocol::BuildListingResponseMessage(firstRequest, true, "both buttons ready"));
    require(until([&]() { return connections.clients()[0].listingStatus == QStringLiteral("成功"); }), "listing success restores automatic eligibility");
    execution(true); wire.clear(); drain(socket);
    SocketUtil::SendLine(socket, Protocol::BuildExecutionResponseMessage(true, "next round complete"));
    require(waitListing(), "second round also starts next purchase");
    const int secondRequest = wire.mid(wire.indexOf("LISTING_REQUEST ")).split(' ')[1].toInt();
    require(secondRequest > firstRequest, "each round uses a new listing request");
    SocketUtil::SendLine(socket, Protocol::BuildListingResponseMessage(secondRequest, true, "ready again"));
    require(until([&]() { return !connections.clients()[0].actionRunning; }), "second listing completes");
    execution(false); drain(socket);
    SocketUtil::SendLine(socket, Protocol::BuildExecutionResponseMessage(true, "manual complete"));
    require(until([&]() { return !connections.clients()[0].actionRunning; })
        && !drain(socket).contains("LISTING_REQUEST "), "disabled automatic mode does not start next purchase");
    execution(true); drain(socket);
    SocketUtil::SendLine(socket, Protocol::BuildExecutionResponseMessage(false, "button disabled"));
    require(until([&]() { return !connections.clients()[0].actionRunning; })
        && !drain(socket).contains("LISTING_REQUEST "), "failed execution does not start next purchase");
    execution(true, false);
    require(!connections.requestListing(id, 100, {}, {}, false, 1, false, error), "unbound legacy listing still requires valid parameters");
    closesocket(socket); connections.stopListening();
    std::puts("QT AUTOMATIC RELIST TESTS PASSED");
    return 0;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); QApplication::setStyle(QStringLiteral("Fusion"));
    if (app.arguments().contains(QStringLiteral("--manual-success-only"))) return testQtForceRestart(true);
    if (app.arguments().contains(QStringLiteral("--qt-force-restart"))) return testQtForceRestart();
    if (app.arguments().contains(QStringLiteral("--qt-auto-relist"))) return testQtAutoRelist();
    require(argc > 1, "ammunition fixture path supplied");
    QFile fixture(QString::fromLocal8Bit(argv[1])); require(fixture.open(QIODevice::ReadOnly), "open recorded real Qt ammunition list");
    const auto jsonItems = QJsonDocument::fromJson(fixture.readAll()).object().value(QStringLiteral("Items")).toArray();
    QStringList items; for (const auto& item : jsonItems) items.append(item.toString());
    AuctionPurchase::Catalog catalog; catalog.purchaseModes = AuctionPurchase::modes();
    for (auto& list : catalog.ammunition) list = items;
    AuctionPurchase::Config config;
    config.entries[0] = {QStringLiteral("射手步枪竞赛弹 [37050200001]"), QStringLiteral("0")};
    QJsonObject object; AuctionPurchase::Config restored;
    require(AuctionPurchase::decode(AuctionPurchase::encode(config.toJson()), object)
        && AuctionPurchase::Config::fromJson(object, restored) && restored.toJson() == config.toJson(), "five-slot config codec preserves UTF-8 and all entries");
    object.insert(QStringLiteral("selected"), 6);
    require(!AuctionPurchase::Config::fromJson(object, restored) && !AuctionPurchase::decode(QStringLiteral("###"), object), "invalid slot and corrupt base64 rejected");
    const auto settingsPath = QCoreApplication::applicationDirPath() + QStringLiteral("/DF_PlayClick_Server.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.clear();
        settings.setValue(QStringLiteral("AuctionPurchase/Config"), AuctionPurchase::encode(config.toJson()));
        settings.setValue(QStringLiteral("AuctionPurchase/Catalog"), AuctionPurchase::encode(catalog.toJson()));
        settings.sync();
    }
    {
        ServerConnectionManager connections;
        AuctionPurchaseCoordinator coordinator(connections);
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve isolated test port");
        int length = sizeof(address); getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length); closesocket(probe);
        QString error;
        require(connections.startListening(ntohs(address.sin_port), error), "production server listens on test port");
        std::array<SOCKET, 3> clients;
        for (int index = 0; index < 3; ++index)
        {
            clients[index] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            require(::connect(clients[index], reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect simulated client");
            SocketUtil::SendLine(clients[index], Protocol::BuildHelloMessage("test-" + std::to_string(index)));
            SocketUtil::SendLine(clients[index], Protocol::BuildAuctionBindingMessage(index < 2));
            u_long nonblocking = 1; ioctlsocket(clients[index], FIONBIO, &nonblocking);
        }
        require(until([&]() { return coordinator.states_.size() == 2 && connections.clients().size() == 3; }), "only Qt-bound clients enter synchronization");
        std::array<QByteArray, 3> wire;
        for (int index = 0; index < 2; ++index)
        {
            require(until([&]() { wire[index] += drain(clients[index]); return wire[index].contains("AUCTION_CATALOG_REQUEST"); }), "server requests actual Qt catalog");
            SocketUtil::SendLine(clients[index], Protocol::BuildAuctionCatalogMessage(AuctionPurchase::encode(catalog.toJson()).toStdString()));
        }
        require(until([&]() { return coordinator.catalogClientCount() == 2; }), "catalogs arrive through production receive loop");
        for (int index = 0; index < 2; ++index)
        {
            require(until([&]() { wire[index] += drain(clients[index]); return wire[index].contains("AUCTION_CONFIG "); }), "idle bound client receives saved configuration");
            SocketUtil::SendLine(clients[index], Protocol::BuildAuctionConfigResultMessage(coordinator.revision_, 1,
                AuctionPurchase::encode({{QStringLiteral("text"), QStringLiteral("test readback confirmed")}}).toStdString()));
        }
        require(until([&]() { return coordinator.syncSuccessful(1) && coordinator.syncSuccessful(2); }), "applied status requires matching client acknowledgement");
        require(!drain(clients[2]).contains("AUCTION_"), "unbound client receives no Qt requests");
        {
            std::lock_guard<std::mutex> guard(connections.mutex_);
            connections.findClientLocked(1)->actionRunning = true;
        }
        for (int index = 0; index < 3; ++index) drain(clients[index]);
        wire[0].clear(); wire[1].clear();
        config.entries[0].threshold = QStringLiteral("999");
        coordinator.updateConfig(config);
        require(coordinator.syncStatus(1) == QStringLiteral("本轮结束后应用") && !drain(clients[0]).contains("AUCTION_CONFIG "), "running client does not receive mid-round writes");
        require(until([&]() { wire[1] += drain(clients[1]); return wire[1].contains("AUCTION_CONFIG "); }), "idle boss or coin receives change immediately");
        SocketUtil::SendLine(clients[0], Protocol::BuildExecutionResponseMessage(false, "button unavailable"));
        require(until([&]() { return connections.clients()[0].executionStatus.contains(QStringLiteral("执行失败")); }),
            "wire execution failure persists in client state");
        const int revision = coordinator.revision_;
        SocketUtil::SendLine(clients[1], Protocol::BuildAuctionConfigResultMessage(revision - 1, 1,
            AuctionPurchase::encode({{QStringLiteral("text"), QStringLiteral("stale")}}).toStdString()));
        QCoreApplication::processEvents();
        require(!coordinator.syncSuccessful(2), "stale acknowledgement cannot mark latest config synchronized");
        SocketUtil::SendLine(clients[0], Protocol::BuildTeamReadyMessage(1, 1, 1));
        require(until([&]() { wire[0] += drain(clients[0]); return wire[0].contains("AUCTION_CONFIG "); }), "round completion dispatches pending change");
        config.selected = 2; config.entries[1] = {items[0], QStringLiteral("1200")};
        coordinator.updateConfig(config);
        for (auto socket : clients) drain(socket);
        wire[0].clear();
        SocketUtil::SendLine(clients[0], Protocol::BuildAuctionBindingMessage(false));
        SocketUtil::SendLine(clients[0], Protocol::BuildAuctionBindingMessage(true));
        require(until([&]() { wire[0] += drain(clients[0]); return wire[0].contains("AUCTION_CATALOG_REQUEST"); }), "rapid rebind refreshes catalog instead of reusing stale window state");
        coordinator.timer_.stop();
        for (auto socket : clients) { shutdown(socket, SD_BOTH); closesocket(socket); }
        connections.stopListening();
    }
    {
        MainWindow window;
        require(window.purchaseSlotCombo_->currentIndex() == 1 && window.purchaseThresholdEdit_->text() == QStringLiteral("1200"), "restart restores selected slot and its own threshold");
        window.purchaseSlotCombo_->setCurrentIndex(0);
        require(window.purchaseThresholdEdit_->text() == QStringLiteral("999"), "slot one retains independent threshold");
        window.purchaseSlotCombo_->setCurrentIndex(1);
        require(window.purchaseThresholdEdit_->text() == QStringLiteral("1200"), "switching slots restores respective threshold");
        window.purchaseModeCombo_->setCurrentText(AuctionPurchase::modes()[0]);
        window.purchaseModeCombo_->setCurrentText(AuctionPurchase::modes()[2]);
        require(window.purchaseThresholdEdit_->text() == QStringLiteral("1200"), "switching purchase mode preserves per-slot memory");
        require(window.purchaseAmmoCombo_->count() == items.size() + 1, "server dropdown exactly follows actual Qt list plus placeholder");
        for (int index = 0; index < items.size(); ++index) require(window.purchaseAmmoCombo_->itemData(index + 1).toString() == items[index], "dropdown item order and full ID preserved");

        // Restore the approved preview's demo values only in this test process.
        window.controller_.auctionPurchase().timer_.stop();
        config.selected = 1; config.entries[0].threshold = QStringLiteral("0");
        window.controller_.auctionPurchase().updateConfig(config);
        window.purchaseDraft_ = config; window.refreshPurchaseControls();
        QVector<ClientListItem> examples(3);
        for (int index = 0; index < 3; ++index)
        {
            examples[index].id = index + 1;
            examples[index].clientNote = QStringLiteral("示例客户端 %1").arg(QChar('A' + index));
            examples[index].auctionBound = index < 2;
            examples[index].role = index == 1 ? ClientRole::Boss : ClientRole::Coin;
        }
        auto& coordinator = window.controller_.auctionPurchase();
        coordinator.states_[1].status = QStringLiteral("本轮结束后应用");
        coordinator.states_[2].status = QStringLiteral("方式 / 子弹已同步（演示）");
        coordinator.states_[2].appliedRevision = coordinator.revision_;
        coordinator.states_[2].needsApply = false;
        examples[0].executionStatus = QStringLiteral("执行失败：开始上架按钮不可用");
        examples[1].executionStatus = QStringLiteral("执行成功");
        window.refreshPurchaseClients(examples);
        require(window.clientTable_->columnCount() == 5
            && window.clientTable_->item(0, 4)->text() == examples[0].executionStatus
            && window.clientTable_->item(1, 4)->text() == examples[1].executionStatus,
            "overview preserves execution success and failure independently of sync state");
        window.serverTimeTimer_.stop();
        window.serverTimeLabel_->setText(QStringLiteral("服务器时间：2026-10-16 18:02:58.636（示例）"));
        window.resize(1018, 1196); window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        QCoreApplication::processEvents();
        require(window.connectionLogEdit_->isVisible() && window.connectionLogEdit_->height() == 110, "full log panel remains visible and retains preview height");
        require(window.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/server-ui-actual.png")), "capture actual production UI for preview comparison");
        QJsonObject bounds;
        bounds.insert(QStringLiteral("font"), QJsonArray{window.purchaseAmmoCombo_->font().family(), QFontInfo(window.purchaseAmmoCombo_->font()).family(), window.purchaseAmmoCombo_->font().pixelSize()});
        auto record = [&](const QString& name, QWidget* widget) {
            const auto position = widget->mapTo(&window, QPoint());
            bounds.insert(name, QJsonArray{position.x(), position.y(), widget->width(), widget->height()});
        };
        record(QStringLiteral("mode"), window.purchaseModeCombo_);
        record(QStringLiteral("slot"), window.purchaseSlotCombo_);
        record(QStringLiteral("ammo"), window.purchaseAmmoCombo_);
        record(QStringLiteral("threshold"), window.purchaseThresholdEdit_);
        record(QStringLiteral("log"), window.connectionLogEdit_);
        for (auto* widget : window.findChildren<QWidget*>())
            if (widget->property("uiSection").isValid()) record(widget->property("uiSection").toString(), widget);
        QFile file(QCoreApplication::applicationDirPath() + QStringLiteral("/server-ui-bounds.json"));
        require(file.open(QIODevice::WriteOnly), "write UI geometry verification"); file.write(QJsonDocument(bounds).toJson());
        window.resize(1024, 760); QCoreApplication::processEvents();
        const auto logBottom = window.connectionLogEdit_->mapTo(&window, QPoint(0, 110)).y();
        require(logBottom <= window.height() && window.connectionLogEdit_->isVisible(), "log remains onscreen at smaller window heights");
        require(window.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/server-ui-compact.png")), "capture smaller window without missing controls");
        window.purchaseThresholdEdit_->selectAll();
        QKeyEvent typing(QEvent::KeyPress, Qt::Key_7, Qt::NoModifier, QStringLiteral("7"));
        QApplication::sendEvent(window.purchaseThresholdEdit_, &typing);
        window.close();
        QSettings settings(settingsPath, QSettings::IniFormat);
        QJsonObject savedJson; AuctionPurchase::Config savedConfig;
        require(AuctionPurchase::decode(settings.value(QStringLiteral("AuctionPurchase/Config")).toString(), savedJson)
            && AuctionPurchase::Config::fromJson(savedJson, savedConfig) && savedConfig.entries[0].threshold == QStringLiteral("7"), "closing during debounce persists latest typed threshold");
    }
    std::puts("ALL SERVER PURCHASE SYNC AND UI TESTS PASSED");
    return 0;
}
