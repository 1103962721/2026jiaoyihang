#include "MainWindow.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QSettings>
#include <QTableWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include "../Shared/FramelessTitleBar.h"
#include "../Shared/FramelessWindowSupport.h"

namespace
{
QString formatAmountForDisplay(const QString& amount)
{
    if (amount == QStringLiteral("-"))
        return amount;
    bool ok = false;
    QString normalized = amount;
    normalized.remove(',');
    const qint64 value = normalized.toLongLong(&ok);
    if (!ok)
        return amount;
    QString digits = QString::number(value);
    const bool negative = digits.startsWith('-');
    const int start = negative ? 1 : 0;
    for (int index = digits.size() - 3; index > start; index -= 3)
        digits.insert(index, ',');
    return digits;
}

QString clientNoteKey(const ClientListItem& client, const QVector<ClientListItem>& clients)
{
    const QString deviceId = client.deviceId.trimmed();
    // HELLO 到达前 deviceId 可能暂时为空。不能让所有未完成握手的客户端
    // 共用同一个空键，否则给其中一个客户端编辑备注会覆盖其他客户端。
    if (deviceId.isEmpty())
        return QStringLiteral("Session/%1").arg(client.id);

    int sameDeviceCount = 0;
    for (const ClientListItem& candidate : clients)
    {
        if (candidate.deviceId.trimmed() == deviceId)
            ++sameDeviceCount;
    }
    // 复制客户端配置会造成多个连接上报相同 DeviceId。此时不能共用持久化键，
    // 使用本次连接的客户端 ID 隔离备注，避免修改一行同步到其他行。
    return sameDeviceCount > 1
        ? QStringLiteral("DuplicateDevice/%1/Session/%2").arg(deviceId).arg(client.id)
        : deviceId;
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    loadPersistentLoadoutSettings();
    controller_.setSavedLoadoutConfig(ClientRole::Coin, loadoutWeapon_, loadoutBackpack_, loadoutEgg_,
        loadoutBulletName_, loadoutPerClick_.toInt(), loadoutBulletTotal_.toInt());
    controller_.setSavedLoadoutConfig(ClientRole::Boss, bossLoadoutWeapon_, bossLoadoutBackpack_, bossLoadoutEgg_,
        bossLoadoutBulletName_, bossLoadoutPerClick_.toInt(), bossLoadoutBulletTotal_.toInt());
    buildUi();
    connectSignals();
    serverTimeTimer_.setInterval(50);
    connect(&serverTimeTimer_, &QTimer::timeout, this, [this]() {
        if (serverTimeLabel_)
            serverTimeLabel_->setText(QStringLiteral("服务器时间：%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    });
    serverTimeTimer_.start();
    refreshClients(controller_.clients());
    updateListeningState(controller_.isListening());
    updateFlowPreview();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    // 无边框窗口仍保留 Windows 原生边缘缩放，边界宽度按当前 DPI 自动换算。
    if (handleFramelessResize(this, message, result))
    {
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::connectSignals()
{
    connectPurchaseSignals();
    const auto syncListingParameters = [this]() {
        controller_.updateGlobalListingParameters(
            listingTextEdit_->text(), listingPriceEdit_->text(), listingPurchaseCountSpin_->value());
    };
    syncListingParameters();
    connect(listingTextEdit_, &QLineEdit::textChanged, this, [syncListingParameters](const QString&) {
        syncListingParameters();
    });
    connect(listingPriceEdit_, &QLineEdit::textChanged, this, [syncListingParameters](const QString&) {
        syncListingParameters();
    });
    connect(listingPurchaseCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [syncListingParameters](int) {
        syncListingParameters();
    });
    connect(listenButton_, &QPushButton::clicked, this, [this]() {
        controller_.startListening(portEdit_->text());
    });
    connect(stopButton_, &QPushButton::clicked, &controller_, &ServerController::stopListening);
    connect(triggerAllButton_, &QPushButton::clicked, this, [this]() {
        controller_.triggerClients(countdownEdit_->text(), groupDelayEdit_->text(), coinFirst_,
            compensationEnabled_, compensationLeadEdit_->text(), bossPriceEdit_->text());
        appendConnectionLog(QStringLiteral("已提交全部出发指令。"));
    });
    connect(listingButton_, &QPushButton::clicked, this, [this]() {
        controller_.startListing(listingTextEdit_->text(), listingPriceEdit_->text(), false,
            listingPurchaseCountSpin_->value(), !tradePurchase_);
        appendConnectionLog(QStringLiteral("已提交币号上架任务。"));
    });
    connect(saveLoadoutButton_, &QPushButton::clicked, this, [this]() {
        if (tradePurchase_)
        {
            QMessageBox::information(this, QStringLiteral("购买方式"), QStringLiteral("请先在设置中选择“配装”方式"));
            return;
        }
        controller_.saveLoadout();
        appendConnectionLog(QStringLiteral("已提交币号保存配装任务。"));
    });
    connect(forceStopAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestForceStopAllActions);
    connect(mailAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestMailAll);
    connect(rebindAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestRebindAllWindows);
    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);
    const auto showClientContextMenu = [this](QTableWidget* table, const QPoint& position) {
        const QModelIndex index = table->indexAt(position);
        if (!index.isValid())
            return;
        const int row = index.row();
        table->selectRow(row);
        const QTableWidgetItem* idItem = table->item(row, 0);
        const quint64 clientId = idItem ? idItem->data(Qt::UserRole).toULongLong() : 0;
        if (clientId == 0)
            return;
        const QVector<ClientListItem> currentClients = controller_.clients();
        const auto iterator = std::find_if(currentClients.cbegin(), currentClients.cend(),
            [clientId](const ClientListItem& client) { return client.id == clientId; });
        if (iterator == currentClients.cend())
            return;
        const ClientListItem client = *iterator;
        const QString noteKey = clientNoteKey(client, currentClients);
        QMenu menu(this);
        QAction* coinAction = menu.addAction(QStringLiteral("设置为币号"));
        QAction* bossAction = menu.addAction(QStringLiteral("设置为老板"));
        menu.addSeparator();
        QAction* noteAction = menu.addAction(QStringLiteral("编辑备注"));
        QAction* clearNoteAction = menu.addAction(QStringLiteral("清空备注"));
        QAction* clearDataAction = menu.addAction(QStringLiteral("清除当前数据"));
        QAction* manualSuccessAction = menu.addAction(client.role == ClientRole::Coin
            ? QStringLiteral("手动设置上架成功") : QStringLiteral("手动设置准备成功 [1/1]"));
        manualSuccessAction->setEnabled(client.connected && !client.actionRunning);
        menu.addSeparator();
        QAction* listingAction = nullptr;
        if (client.role == ClientRole::Coin)
            listingAction = menu.addAction(QStringLiteral("单独上架"));
        QAction* accountLoginAction = nullptr;
        QAction* scriptLoginAction = nullptr;
        QAction* accountSwitchAction = nullptr;
        if (client.role == ClientRole::Coin)
        {
            accountLoginAction = menu.addAction(QStringLiteral("上号"));
            scriptLoginAction = menu.addAction(QStringLiteral("脚本上号"));
            accountSwitchAction = menu.addAction(QStringLiteral("换号"));
        }
        QAction* loadoutAction = menu.addAction(QStringLiteral("保存配装"));
        QAction* mailAction = menu.addAction(QStringLiteral("领取邮件"));
        QAction* rebindAction = menu.addAction(QStringLiteral("重新绑定"));
        const bool accountLoginRunning = client.role == ClientRole::Coin
            && client.actionRunning && client.runningAction == QStringLiteral("上号中");
        QAction* stopAction = menu.addAction(accountLoginRunning
            ? QStringLiteral("中止上号") : QStringLiteral("停止动作"));
        QAction* kickAction = menu.addAction(QStringLiteral("断开客户端"));
        QAction* chosen = menu.exec(table->viewport()->mapToGlobal(position));
        if (chosen == coinAction)
            controller_.setClientRole(clientId, ClientRole::Coin);
        else if (chosen == bossAction)
            controller_.setClientRole(clientId, ClientRole::Boss);
        else if (chosen == noteAction)
        {
            bool accepted = false;
            const QString currentNote = clientNotes_.value(noteKey, client.clientNote);
            const QString note = QInputDialog::getText(this, QStringLiteral("编辑备注"),
                QStringLiteral("备注"), QLineEdit::Normal, currentNote, &accepted);
            if (accepted)
            {
                clientNotes_[noteKey] = note;
                // 只有真实设备标识才写入持久化配置；握手尚未完成时使用会话键，
                // 防止空 deviceId 造成多个客户端共享备注。
                const bool duplicateDevice = noteKey.startsWith(QStringLiteral("DuplicateDevice/"));
                if (!client.deviceId.trimmed().isEmpty() && !duplicateDevice)
                {
                    QSettings settings(QCoreApplication::applicationDirPath() + QStringLiteral("/DF_PlayClick_Server.ini"), QSettings::IniFormat);
                    settings.setValue(QStringLiteral("ClientNotes/%1").arg(noteKey), note);
                    settings.sync();
                }
                controller_.setClientNote(clientId, note);
            }
        }
        else if (chosen == clearNoteAction)
        {
            clientNotes_.remove(noteKey);
            const bool duplicateDevice = noteKey.startsWith(QStringLiteral("DuplicateDevice/"));
            if (!client.deviceId.trimmed().isEmpty() && !duplicateDevice)
            {
                QSettings settings(QCoreApplication::applicationDirPath() + QStringLiteral("/DF_PlayClick_Server.ini"), QSettings::IniFormat);
                settings.remove(QStringLiteral("ClientNotes/%1").arg(noteKey));
                settings.sync();
            }
            controller_.setClientNote(clientId, QString());
        }
        else if (chosen == clearDataAction)
        {
            controller_.clearClientData(clientId);
        }
        else if (chosen == manualSuccessAction)
            controller_.setClientManualSuccess(clientId);
        else if (chosen == listingAction)
            controller_.startListingForClient(clientId, listingTextEdit_->text(), listingPriceEdit_->text(), false, listingPurchaseCountSpin_->value(), !tradePurchase_);
        else if (chosen == accountLoginAction)
        {
            if (controller_.setAccountLoginConfig(clientId,
                listingTextEdit_->text(), listingPriceEdit_->text(), listingPurchaseCountSpin_->value()))
                controller_.startAccountLogin(clientId);
        }
        else if (chosen == scriptLoginAction)
            controller_.startScriptAccountLogin(clientId);
        else if (chosen == accountSwitchAction)
        {
            if (controller_.setAccountLoginConfig(clientId,
                listingTextEdit_->text(), listingPriceEdit_->text(), listingPurchaseCountSpin_->value()))
                controller_.startAccountSwitch(clientId);
        }
        else if (chosen == loadoutAction)
        {
            controller_.saveLoadoutForClient(clientId, client.role);
        }
        else if (chosen == mailAction)
            controller_.requestMail(clientId);
        else if (chosen == rebindAction)
            controller_.requestRebindWindow(clientId);
        else if (chosen == stopAction)
        {
            if (accountLoginRunning)
                controller_.requestAccountLoginStop(clientId);
            else
                controller_.requestForceStopAction(clientId);
        }
        else if (chosen == kickAction)
            controller_.kickClient(clientId);
    };
    connect(importAccountsButton_, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入账号文件"), QString(), QStringLiteral("文本文件 (*.txt)"));
        if (!path.isEmpty()) controller_.importAccounts(path);
    });
    connect(clientTable_, &QTableWidget::customContextMenuRequested, this,
        [this, showClientContextMenu](const QPoint& position) {
            showClientContextMenu(clientTable_, position);
        });
    connect(clientListTable_, &QTableWidget::customContextMenuRequested, this,
        [this, showClientContextMenu](const QPoint& position) {
            showClientContextMenu(clientListTable_, position);
        });
    connect(addAccountsButton_, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("增加账号文件"), QString(), QStringLiteral("文本文件 (*.txt)"));
        if (!path.isEmpty()) controller_.addAccounts(path);
    });
    connect(clearAccountsButton_, &QPushButton::clicked, &controller_, &ServerController::clearAccounts);
    connect(exportAccountsButton_, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出剩余账号"), QString(), QStringLiteral("文本文件 (*.txt)"));
        if (!path.isEmpty()) controller_.exportAccounts(path);
    });
    connect(accountLoginAllButton_, &QPushButton::clicked, this, [this]() {
        const QVector<ClientListItem> currentClients = controller_.clients();
        bool configValid = true;
        for (const ClientListItem& client : currentClients)
        {
            if (!client.connected || client.role != ClientRole::Coin)
                continue;
            configValid = controller_.setAccountLoginConfig(client.id,
                listingTextEdit_->text(), listingPriceEdit_->text(), listingPurchaseCountSpin_->value()) && configValid;
        }
        if (!configValid)
            return;
        controller_.startAccountLoginForAllCoins();
        appendConnectionLog(QStringLiteral("已提交全体币号一键上号任务。"));
    });
    connect(scriptLoginAllButton_, &QPushButton::clicked, &controller_, &ServerController::startScriptAccountLoginForAllCoins);
    connect(&controller_, &ServerController::accountQueueChanged, this, [this](int total, int remaining, int) {
        if (accountQueueLabel_)
            accountQueueLabel_->setText(QStringLiteral("账号队列：%1/%2").arg(remaining).arg(total));
    });
    connect(autoBossThresholdSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        controller_.setAutoExecuteThresholds(autoBossThresholdSpin_->value(), autoCoinThresholdSpin_->value());
    });
    connect(autoCoinThresholdSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        controller_.setAutoExecuteThresholds(autoBossThresholdSpin_->value(), autoCoinThresholdSpin_->value());
    });
    connect(highPriceStopThresholdSpin_, qOverload<int>(&QSpinBox::valueChanged), &controller_,
        &ServerController::setHighPriceStopThreshold);
    connect(autoExecuteCheck_, &QCheckBox::toggled, &controller_, &ServerController::setAutoExecuteEnabled);
    const auto syncAutoParameters = [this]() {
        controller_.updateAutoExecuteParameters(countdownEdit_->text(), groupDelayEdit_->text(), coinFirst_,
            compensationEnabled_, compensationLeadEdit_->text(), bossPriceEdit_->text());
    };
    connect(countdownEdit_, &QLineEdit::textChanged, this, [this, syncAutoParameters](const QString&) {
        syncAutoParameters();
    });
    connect(groupDelayEdit_, &QLineEdit::textChanged, this, [this, syncAutoParameters](const QString&) {
        syncAutoParameters();
        updateFlowPreview();
    });
    connect(compensationLeadEdit_, &QLineEdit::textChanged, this, [this, syncAutoParameters](const QString&) {
        syncAutoParameters();
        updateFlowPreview();
    });
    connect(bossPriceEdit_, &QLineEdit::textChanged, this, [syncAutoParameters](const QString&) {
        syncAutoParameters();
    });
    connect(accountSwitchAmountEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        accountSwitchAmount_ = text.trimmed();
        controller_.setAccountSwitchAmountThreshold(accountSwitchAmount_);
        savePersistentLoadoutSettings();
    });
    controller_.setAccountSwitchAmountThreshold(accountSwitchAmount_);
    connect(&controller_, &ServerController::clientsChanged, this, &MainWindow::refreshClients);
    connect(&controller_, &ServerController::listeningChanged, this, &MainWindow::updateListeningState);
    connect(&controller_, &ServerController::statusChanged, this, &MainWindow::updateStatus);
    connect(&controller_, &ServerController::connectionLogAppended, this, &MainWindow::appendConnectionLog);
    connect(&controller_, &ServerController::messageRequested, this,
            [this](const QString& title, const QString& text, bool error) {
                if (error)
                {
                    QMessageBox::critical(this, title, text);
                }
                else
                {
                    QMessageBox::information(this, title, text);
                }
    });
    connect(&controller_, &ServerController::incompleteTriggerRequested, this, [this]() {
        const auto answer = QMessageBox::question(this, QStringLiteral("客户端数量不足"),
            QStringLiteral("当前不满足一整组，是否强制下发给空闲且已准备的客户端？\n币号将忽略上架成功状态，Qt 老板将跳过执行前准备检查；正在执行或停止中的客户端仍需等待结束。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
        {
            controller_.triggerClients(countdownEdit_->text(), groupDelayEdit_->text(), coinFirst_,
                compensationEnabled_, compensationLeadEdit_->text(), bossPriceEdit_->text(), true);
        }
    });
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("调度设置"));
    dialog.setModal(true);
    dialog.resize(720, 600);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);
    auto* optionsGrid = new QGridLayout();
    optionsGrid->setHorizontalSpacing(12);
    optionsGrid->setVerticalSpacing(12);

    auto* orderBox = new QGroupBox(QStringLiteral("发送顺序"), &dialog);
    auto* orderLayout = new QVBoxLayout(orderBox);
    auto* coinFirstRadio = new QRadioButton(QStringLiteral("币号 → 老板"), orderBox);
    auto* bossFirstRadio = new QRadioButton(QStringLiteral("老板 → 币号"), orderBox);
    auto* orderGroup = new QButtonGroup(orderBox);
    orderGroup->setExclusive(true);
    orderGroup->addButton(coinFirstRadio);
    orderGroup->addButton(bossFirstRadio);
    coinFirstRadio->setChecked(coinFirst_);
    bossFirstRadio->setChecked(!coinFirst_);
    orderLayout->addWidget(coinFirstRadio);
    orderLayout->addWidget(bossFirstRadio);
    optionsGrid->addWidget(orderBox, 0, 0);

    auto* modeBox = new QGroupBox(QStringLiteral("模式选择"), &dialog);
    auto* modeLayout = new QVBoxLayout(modeBox);
    auto* normalRadio = new QRadioButton(QStringLiteral("普通模式"), modeBox);
    auto* compensationRadio = new QRadioButton(QStringLiteral("延迟补偿"), modeBox);
    auto* modeGroup = new QButtonGroup(modeBox);
    modeGroup->setExclusive(true);
    modeGroup->addButton(normalRadio);
    modeGroup->addButton(compensationRadio);
    normalRadio->setChecked(!compensationEnabled_);
    compensationRadio->setChecked(compensationEnabled_);
    modeLayout->addWidget(normalRadio);
    modeLayout->addWidget(compensationRadio);
    optionsGrid->addWidget(modeBox, 0, 1);

    auto* captchaBox = new QGroupBox(QStringLiteral("验证码"), &dialog);
    auto* captchaLayout = new QVBoxLayout(captchaBox);
    auto* bruteRadio = new QRadioButton(QStringLiteral("暴力"), captchaBox);
    auto* aiRadio = new QRadioButton(QStringLiteral("AI"), captchaBox);
    auto* captchaGroup = new QButtonGroup(captchaBox);
    captchaGroup->setExclusive(true);
    captchaGroup->addButton(bruteRadio);
    captchaGroup->addButton(aiRadio);
    bruteRadio->setChecked(bruteCaptcha_);
    aiRadio->setChecked(!bruteCaptcha_);
    captchaLayout->addWidget(bruteRadio);
    captchaLayout->addWidget(aiRadio);
    optionsGrid->addWidget(captchaBox, 1, 0);

    auto* purchaseBox = new QGroupBox(QStringLiteral("购买方式"), &dialog);
    auto* purchaseLayout = new QVBoxLayout(purchaseBox);
    auto* marketRadio = new QRadioButton(QStringLiteral("交易行"), purchaseBox);
    auto* loadoutRadio = new QRadioButton(QStringLiteral("配装"), purchaseBox);
    auto* purchaseGroup = new QButtonGroup(purchaseBox);
    purchaseGroup->setExclusive(true);
    purchaseGroup->addButton(marketRadio);
    purchaseGroup->addButton(loadoutRadio);
    marketRadio->setChecked(tradePurchase_);
    loadoutRadio->setChecked(!tradePurchase_);
    purchaseLayout->addWidget(marketRadio);
    purchaseLayout->addWidget(loadoutRadio);
    optionsGrid->addWidget(purchaseBox, 1, 1);
    root->addLayout(optionsGrid, 1);

    auto* loadoutParamsBox = new QGroupBox(QStringLiteral("配装参数"), &dialog);
    auto* loadoutParamsLayout = new QGridLayout(loadoutParamsBox);
    loadoutParamsLayout->setHorizontalSpacing(10);
    loadoutParamsLayout->setVerticalSpacing(8);
    auto addLoadoutField = [loadoutParamsBox, loadoutParamsLayout](int row, int column, const QString& labelText,
        const QString& value, QLineEdit*& edit, bool integerOnly = false) {
        auto* label = new QLabel(labelText, loadoutParamsBox);
        edit = new QLineEdit(value, loadoutParamsBox);
        edit->setPlaceholderText(QStringLiteral("请输入%1").arg(labelText));
        if (integerOnly)
        {
            edit->setValidator(new QIntValidator(1, 999999999, edit));
        }
        loadoutParamsLayout->addWidget(label, row, column * 2);
        loadoutParamsLayout->addWidget(edit, row, column * 2 + 1);
    };
    QLineEdit* weaponEdit = nullptr;
    QLineEdit* backpackEdit = nullptr;
    QLineEdit* eggEdit = nullptr;
    QLineEdit* bulletNameEdit = nullptr;
    QLineEdit* perClickEdit = nullptr;
    QLineEdit* bulletTotalEdit = nullptr;
    addLoadoutField(0, 0, QStringLiteral("武器"), loadoutWeapon_, weaponEdit);
    addLoadoutField(0, 1, QStringLiteral("背包"), loadoutBackpack_, backpackEdit);
    addLoadoutField(1, 0, QStringLiteral("蛋挂"), loadoutEgg_, eggEdit);
    addLoadoutField(1, 1, QStringLiteral("子弹名称"), loadoutBulletName_, bulletNameEdit);
    addLoadoutField(2, 0, QStringLiteral("单次数量"), loadoutPerClick_, perClickEdit, true);
    addLoadoutField(2, 1, QStringLiteral("子弹总数"), loadoutBulletTotal_, bulletTotalEdit, true);
    loadoutParamsBox->setVisible(!tradePurchase_);
    connect(loadoutRadio, &QRadioButton::toggled, loadoutParamsBox, &QWidget::setVisible);
    root->addWidget(loadoutParamsBox);

    auto* bossLoadoutParamsBox = new QGroupBox(QStringLiteral("老板配装参数"), &dialog);
    auto* bossLoadoutParamsLayout = new QGridLayout(bossLoadoutParamsBox);
    bossLoadoutParamsLayout->setHorizontalSpacing(10);
    bossLoadoutParamsLayout->setVerticalSpacing(8);
    QLineEdit* bossWeaponEdit = nullptr;
    QLineEdit* bossBackpackEdit = nullptr;
    QLineEdit* bossEggEdit = nullptr;
    QLineEdit* bossBulletNameEdit = nullptr;
    QLineEdit* bossPerClickEdit = nullptr;
    QLineEdit* bossBulletTotalEdit = nullptr;
    auto addBossLoadoutField = [bossLoadoutParamsBox, bossLoadoutParamsLayout](int row, int column, const QString& labelText,
        const QString& value, QLineEdit*& edit, bool integerOnly = false) {
        auto* label = new QLabel(labelText, bossLoadoutParamsBox);
        edit = new QLineEdit(value, bossLoadoutParamsBox);
        edit->setPlaceholderText(QStringLiteral("请输入%1").arg(labelText));
        if (integerOnly)
            edit->setValidator(new QIntValidator(1, 999999999, edit));
        bossLoadoutParamsLayout->addWidget(label, row, column * 2);
        bossLoadoutParamsLayout->addWidget(edit, row, column * 2 + 1);
    };
    addBossLoadoutField(0, 0, QStringLiteral("武器"), bossLoadoutWeapon_, bossWeaponEdit);
    addBossLoadoutField(0, 1, QStringLiteral("背包"), bossLoadoutBackpack_, bossBackpackEdit);
    addBossLoadoutField(1, 0, QStringLiteral("蛋挂"), bossLoadoutEgg_, bossEggEdit);
    addBossLoadoutField(1, 1, QStringLiteral("子弹名称"), bossLoadoutBulletName_, bossBulletNameEdit);
    addBossLoadoutField(2, 0, QStringLiteral("单次数量"), bossLoadoutPerClick_, bossPerClickEdit, true);
    addBossLoadoutField(2, 1, QStringLiteral("子弹总数"), bossLoadoutBulletTotal_, bossBulletTotalEdit, true);
    root->addWidget(bossLoadoutParamsBox);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    coinFirst_ = coinFirstRadio->isChecked();
    compensationEnabled_ = compensationRadio->isChecked();
    bruteCaptcha_ = bruteRadio->isChecked();
    tradePurchase_ = marketRadio->isChecked();
    loadoutWeapon_ = weaponEdit->text().trimmed();
    loadoutBackpack_ = backpackEdit->text().trimmed();
    loadoutEgg_ = eggEdit->text().trimmed();
    loadoutBulletName_ = bulletNameEdit->text().trimmed();
    loadoutPerClick_ = perClickEdit->text().trimmed();
    loadoutBulletTotal_ = bulletTotalEdit->text().trimmed();
    bossLoadoutWeapon_ = bossWeaponEdit->text().trimmed();
    bossLoadoutBackpack_ = bossBackpackEdit->text().trimmed();
    bossLoadoutEgg_ = bossEggEdit->text().trimmed();
    bossLoadoutBulletName_ = bossBulletNameEdit->text().trimmed();
    bossLoadoutPerClick_ = bossPerClickEdit->text().trimmed();
    bossLoadoutBulletTotal_ = bossBulletTotalEdit->text().trimmed();
    savePersistentLoadoutSettings();
    controller_.setSavedLoadoutConfig(ClientRole::Coin, loadoutWeapon_, loadoutBackpack_, loadoutEgg_,
        loadoutBulletName_, loadoutPerClick_.toInt(), loadoutBulletTotal_.toInt());
    controller_.setSavedLoadoutConfig(ClientRole::Boss, bossLoadoutWeapon_, bossLoadoutBackpack_, bossLoadoutEgg_,
        bossLoadoutBulletName_, bossLoadoutPerClick_.toInt(), bossLoadoutBulletTotal_.toInt());
    controller_.updateAutoExecuteParameters(countdownEdit_->text(), groupDelayEdit_->text(), coinFirst_,
        compensationEnabled_, compensationLeadEdit_->text(), bossPriceEdit_->text());
    updateFlowPreview();
}

void MainWindow::loadPersistentLoadoutSettings()
{
    const QString settingsPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/DF_PlayClick_Server.ini");
    const QSettings settings(settingsPath, QSettings::IniFormat);
    loadoutWeapon_ = settings.value(QStringLiteral("Loadout/Weapon"), loadoutWeapon_).toString();
    loadoutBackpack_ = settings.value(QStringLiteral("Loadout/Backpack"), loadoutBackpack_).toString();
    loadoutEgg_ = settings.value(QStringLiteral("Loadout/Egg"), loadoutEgg_).toString();
    loadoutBulletName_ = settings.value(QStringLiteral("Loadout/BulletName"), loadoutBulletName_).toString();
    loadoutPerClick_ = settings.value(QStringLiteral("Loadout/PerClick"), loadoutPerClick_).toString();
    loadoutBulletTotal_ = settings.value(QStringLiteral("Loadout/BulletTotal"), loadoutBulletTotal_).toString();
    accountSwitchAmount_ = settings.value(QStringLiteral("AccountSwitch/Amount"), accountSwitchAmount_).toString();
    bossLoadoutWeapon_ = settings.value(QStringLiteral("BossLoadout/Weapon"), bossLoadoutWeapon_).toString();
    bossLoadoutBackpack_ = settings.value(QStringLiteral("BossLoadout/Backpack"), bossLoadoutBackpack_).toString();
    bossLoadoutEgg_ = settings.value(QStringLiteral("BossLoadout/Egg"), bossLoadoutEgg_).toString();
    bossLoadoutBulletName_ = settings.value(QStringLiteral("BossLoadout/BulletName"), bossLoadoutBulletName_).toString();
    bossLoadoutPerClick_ = settings.value(QStringLiteral("BossLoadout/PerClick"), bossLoadoutPerClick_).toString();
    bossLoadoutBulletTotal_ = settings.value(QStringLiteral("BossLoadout/BulletTotal"), bossLoadoutBulletTotal_).toString();
    const QStringList noteKeys = settings.allKeys();
    for (const QString& key : noteKeys)
    {
        if (key.startsWith(QStringLiteral("ClientNotes/")))
            clientNotes_.insert(key.mid(QStringLiteral("ClientNotes/").size()), settings.value(key).toString());
    }
}

void MainWindow::savePersistentLoadoutSettings() const
{
    const QString settingsPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/DF_PlayClick_Server.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("Loadout/Weapon"), loadoutWeapon_);
    settings.setValue(QStringLiteral("Loadout/Backpack"), loadoutBackpack_);
    settings.setValue(QStringLiteral("Loadout/Egg"), loadoutEgg_);
    settings.setValue(QStringLiteral("Loadout/BulletName"), loadoutBulletName_);
    settings.setValue(QStringLiteral("Loadout/PerClick"), loadoutPerClick_);
    settings.setValue(QStringLiteral("Loadout/BulletTotal"), loadoutBulletTotal_);
    settings.setValue(QStringLiteral("AccountSwitch/Amount"), accountSwitchAmount_);
    settings.setValue(QStringLiteral("BossLoadout/Weapon"), bossLoadoutWeapon_);
    settings.setValue(QStringLiteral("BossLoadout/Backpack"), bossLoadoutBackpack_);
    settings.setValue(QStringLiteral("BossLoadout/Egg"), bossLoadoutEgg_);
    settings.setValue(QStringLiteral("BossLoadout/BulletName"), bossLoadoutBulletName_);
    settings.setValue(QStringLiteral("BossLoadout/PerClick"), bossLoadoutPerClick_);
    settings.setValue(QStringLiteral("BossLoadout/BulletTotal"), bossLoadoutBulletTotal_);
    settings.sync();
}

QWidget* MainWindow::createAppHeader()
{
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("appHeader"));
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(20, 14, 20, 14);
    layout->setSpacing(10);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(3);
    auto* title = new QLabel(QStringLiteral("服务端调度中心"), header);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("客户端分组 · 倒计时 · 精确延迟 · 批量触发"), header);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    titles->addWidget(title);
    titles->addWidget(subtitle);

    layout->addLayout(titles);
    layout->addStretch();
    listenBadge_ = createBadge(QStringLiteral("未监听"), QStringLiteral("#f8fafc"), QStringLiteral("#182230"));
    clientCountBadge_ = createBadge(QStringLiteral("0 客户端"), QStringLiteral("#f8fafc"), QStringLiteral("#182230"));
    layout->addWidget(listenBadge_);
    layout->addWidget(clientCountBadge_);
    return header;
}

QWidget* MainWindow::createCard(const QString& title, QLayout* contentLayout)
{
    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(17, 17, 17, 17);
    layout->setSpacing(15);
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    titleLabel->setFixedHeight(21);
    layout->addWidget(titleLabel);
    layout->addLayout(contentLayout);
    return card;
}

QLabel* MainWindow::createBadge(const QString& text, const QString& background, const QString& color)
{
    auto* badge = new QLabel(text, this);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral("padding:5px 10px;border-radius:11px;background:%1;color:%2;font-weight:700;")
                             .arg(background, color));
    return badge;
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, #windowRoot { background:#f3f5f8; color:#182230; font-family:"Microsoft YaHei UI"; font-size:12px; }
        QDialog { background:#ffffff; color:#182230; font-family:"Microsoft YaHei UI"; font-size:12px; }
        QGroupBox { border:1px solid #d8dee8; border-radius:8px; margin-top:9px; padding:12px 10px 8px 10px; background:#ffffff; color:#182230; font-weight:800; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 5px; background:#ffffff; }
        #framelessTitleBar { background:#ffffff; border-bottom:1px solid #d8dee8; }
        #windowTitleText { color:#182230; font-size:12px; font-weight:700; }
        #windowMinimizeButton, #windowMaximizeButton, #windowCloseButton { min-width:42px; max-width:42px; min-height:38px; max-height:38px; border:0; border-radius:0; background:transparent; color:#344054; font-size:16px; padding:0; }
        #windowMinimizeButton:hover, #windowMaximizeButton:hover { background:#eef2f6; }
        #windowCloseButton:hover { background:#d9534f; color:white; }
        #appHeader { background:#ffffff; border-bottom:1px solid #d8dee8; }
        #appTitle { color:#182230; font-size:20px; font-weight:800; }
        #appSubtitle { color:#667085; font-size:12px; }
        #body { background:#f3f5f8; }
        #sidebar { background:#f9fafc; border-right:1px solid #d8dee8; }
        #card { background:#ffffff; border:1px solid #d8dee8; border-radius:8px; }
        #cardTitle { color:#182230; font-size:14px; font-weight:800; }
        #navCaption { color:#667085; font-size:11px; font-weight:700; padding:4px 10px; }
        #pageNavButton { min-height:34px; text-align:left; background:transparent; color:#475467; border:0; border-radius:6px; padding:0 10px; }
        #pageNavButton:hover { background:#eef2f6; }
        #pageNavButton:checked { background:#e8efff; color:#1d4ed8; }
        #sidebarHint, #pageHint { color:#667085; font-size:11px; line-height:1.5; }
        #pageHeading { color:#182230; font-size:16px; font-weight:700; }
        #fieldLabel, #hintText { color:#667085; font-size:11px; }
        #listingQuantityTotal, #listingSuccessClients { border:1px solid #cbd5e1; border-radius:7px; background:#f8fafc; color:#1d4ed8; padding:6px 10px; font-size:14px; font-weight:800; }
        QLineEdit, QSpinBox { min-height:28px; max-height:28px; border:1px solid #cbd5e1; border-radius:6px; padding:0 8px; background:#ffffff; color:#182230; }
        QSpinBox::up-button, QSpinBox::down-button { width:0px; height:0px; border:0px; }
        QSpinBox::up-arrow, QSpinBox::down-arrow { width:0px; height:0px; border:0px; }
        QLineEdit:focus, QSpinBox:focus { border-color:#2563eb; }
        QRadioButton, QCheckBox { min-height:22px; color:#344054; spacing:6px; font-weight:700; }
        QRadioButton::indicator, QCheckBox::indicator { width:12px; height:12px; border:2px solid #98a2b3; border-radius:3px; background:#ffffff; }
        QRadioButton::indicator:checked, QCheckBox::indicator:checked { background:#2563eb; border-color:#2563eb; }
        QPushButton { min-height:29px; border:1px solid #cbd5e1; border-radius:6px; background:#ffffff; color:#344054; padding:0 10px; font-weight:700; }
        QPushButton:hover { background:#f1f5f9; border-color:#3b82f6; }
        QPushButton:disabled { color:#98a2b3; background:#f2f4f7; border-color:#d0d5dd; }
        #roleCell { background:transparent; }
        #roleSelector { background:#f8fafc; border:1px solid #cbd5e1; border-radius:14px; }
        #coinRoleButton, #bossRoleButton { min-width:44px; max-width:44px; min-height:26px; max-height:26px; border:0; border-radius:12px; padding:0 6px; background:transparent; color:#667085; }
        #coinRoleButton:hover, #bossRoleButton:hover { background:#e4e7ec; }
        #coinRoleButton:checked { background:#2563eb; color:white; }
        #bossRoleButton:checked { background:#7653c9; color:white; }
        #executeButton { background:#18a36f; color:white; border-color:#18a36f; }
        #executeButton:hover { background:#12865b; }
        #listingButton { background:#7653c9; color:white; border-color:#7653c9; }
        #listingButton:hover { background:#6343af; }
        #saveLoadoutButton { background:#0f8a8a; color:white; border-color:#0f8a8a; }
        #saveLoadoutButton:hover { background:#0b7070; }
         QTableWidget { background:#ffffff; alternate-background-color:#f8fafc; border:0; gridline-color:#eaecf0; selection-background-color:#dbe7ff; color:#182230; }
         QTableWidget::item:selected { color:#182230; background:#ffffff; }
        #expectedAmountEdit { min-height:28px; max-height:28px; border:0; border-radius:4px; padding:0 8px; background:transparent; color:#182230; }
        #expectedAmountEdit:focus { border:1px solid #2563eb; background:#ffffff; }
        QHeaderView::section { background:#f2f4f7; color:#475467; font-weight:800; border:0; border-right:1px solid #eaecf0; border-bottom:1px solid #d8dee8; padding:7px; }
        #flowPreview { padding:8px 11px; border-radius:6px; background:#eef4ff; color:#1d4ed8; font-weight:700; }
        #statusLabel { padding:7px 10px; border-radius:6px; background:#f5f7fa; color:#344054; }
        #serverTimeLabel { color:#667085; padding:7px; }
        #connectionLog { background:#182230; color:#b7f7c5; border:1px solid #344054; border-radius:6px; padding:8px; font-family:"Consolas","Microsoft YaHei UI"; }
    )"));
    setStyleSheet(styleSheet() + QStringLiteral(R"(
        QWidget { font-family:"Microsoft YaHei UI"; font-size:14px; font-weight:400; color:#233047; }
        QMainWindow, #windowRoot, #body, #overviewContent, QScrollArea, QScrollArea > QWidget > QWidget { background:#f4f6fa; }
        #windowRoot { border:1px solid #dce3ec; border-radius:10px; }
        #framelessTitleBar { background:#ffffff; border-bottom:1px solid #dce3ec; }
        #windowTitleText { font-size:16px; font-weight:400; color:#233047; }
        #sidebar { background:#ffffff; border-right:1px solid #dce3ec; }
        #pageNavButton { min-height:43px; max-height:43px; text-align:left; padding:0 12px; color:#66748a; background:transparent; border:0; border-radius:6px; font-weight:400; }
        #pageNavButton:checked { background:#edf3ff; color:#245ed6; }
        #pageHeading { font-size:19px; font-weight:400; color:#233047; }
        #card { background:#ffffff; border:1px solid #dce3ec; border-radius:8px; }
        #cardTitle { font-size:14px; font-weight:400; color:#233047; }
        #fieldLabel { color:#233047; font-size:14px; }
        #sectionHint { color:#66748a; font-size:12px; }
        #purchaseScope { color:#245ed6; font-size:12px; }
        #purchaseWarning { color:#925813; font-size:12px; border-top:1px solid #dce3ec; padding-top:12px; margin-top:0px; }
        QLineEdit, QSpinBox, QComboBox { min-height:38px; max-height:38px; border:1px solid #dce3ec; border-radius:6px; padding:0 10px; color:#233047; background:#ffffff; font-size:14px; font-weight:400; }
        QLineEdit, QSpinBox { min-height:39px; max-height:39px; }
        QComboBox::drop-down { border:0; width:24px; }
        QComboBox::down-arrow { image:url(:/purchase-chevron.png); width:12px; height:12px; }
        QComboBox QAbstractItemView { background:#ffffff; color:#233047; selection-background-color:#edf3ff; border:1px solid #dce3ec; }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color:#245ed6; }
        QPushButton { min-height:35px; max-height:35px; border:1px solid #dce3ec; border-radius:6px; padding:0 15px; background:#ffffff; color:#233047; font-weight:400; }
        QPushButton:disabled { background:#ffffff; color:#66748a; border-color:#dce3ec; }
        #inlineButton { min-height:33px; max-height:33px; padding:0 9px; font-size:14px; }
        #inlineSpin { min-height:33px; max-height:33px; padding:0 8px; font-size:14px; }
        QCheckBox { font-weight:400; font-size:14px; spacing:6px; min-height:21px; color:#233047; }
        QCheckBox::indicator { width:14px; height:14px; border:1px solid #98a2b3; border-radius:2px; background:#ffffff; }
        QTableWidget { background:#ffffff; color:#233047; border:0; selection-background-color:#edf3ff; font-size:12px; font-weight:400; }
        QTableWidget::item { padding:8px; border-bottom:1px solid #dce3ec; }
        QTableWidget::item:selected { color:#233047; background:#edf3ff; }
        QHeaderView::section { background:#ffffff; color:#66748a; border:0; border-bottom:1px solid #dce3ec; padding:0 8px 10px 8px; font-size:12px; font-weight:400; }
        #statusLabel { padding:0; background:transparent; color:#233047; font-size:12px; }
        #serverTimeLabel { padding:0; color:#66748a; font-size:12px; }
        #connectionLog { background:#192433; color:#a6c7ca; border:1px solid #dce3ec; border-radius:6px; padding:12px; font-family:"Microsoft YaHei UI"; font-size:12px; }
    )"));
}

void MainWindow::refreshClients(const QVector<ClientListItem>& clients)
{
    int listingQuantityTotal = 0;
    int connectedCoinCount = 0;
    int successfulListingClientCount = 0;
    QVector<ClientListItem> displayClients = clients;
    std::stable_sort(displayClients.begin(), displayClients.end(), [](const ClientListItem& left, const ClientListItem& right) {
        if (left.role != right.role)
            return left.role == ClientRole::Boss;
        // 老板保持置顶；币号不再按状态插队，统一按客户端 ID 固定排序。
        return left.id < right.id;
    });
    for (const ClientListItem& client : displayClients)
    {
        if (client.role == ClientRole::Coin && client.connected)
        {
            ++connectedCoinCount;
            if (client.listingQuantityX >= 0)
            {
                listingQuantityTotal += client.listingQuantityX;
            }
            if (client.listingStatus == QStringLiteral("成功"))
            {
                ++successfulListingClientCount;
            }
        }
    }
    if (listingQuantityTotalLabel_)
    {
        listingQuantityTotalLabel_->setText(QStringLiteral("上架总数:%1").arg(listingQuantityTotal));
    }
    if (listingSuccessClientsLabel_)
    {
        listingSuccessClientsLabel_->setText(QStringLiteral("成功上架客户端:%1/%2")
            .arg(successfulListingClientCount).arg(connectedCoinCount));
        const bool allSuccessful = connectedCoinCount > 0
            && successfulListingClientCount == connectedCoinCount;
        listingSuccessClientsLabel_->setStyleSheet(allSuccessful
            ? QStringLiteral("border:1px solid #86efac;border-radius:9px;background:#f0fdf4;color:#15803d;padding:6px 10px;font-size:14px;font-weight:800;")
            : QStringLiteral("border:1px solid #fca5a5;border-radius:9px;background:#fef2f2;color:#b91c1c;padding:6px 10px;font-size:14px;font-weight:800;"));
    }
    clientListTable_->setRowCount(0);
    for (const ClientListItem& client : displayClients)
    {
        const int row = clientListTable_->rowCount();
        clientListTable_->insertRow(row);

        QString displayAddress = client.ip;
        const QString deviceId = client.deviceId.trimmed();
        if (deviceId.startsWith(QStringLiteral("ADB-"), Qt::CaseInsensitive))
        {
            const QString adbPort = deviceId.mid(4).trimmed();
            if (!adbPort.isEmpty())
                displayAddress += QStringLiteral(":%1").arg(adbPort);
        }
        auto* ipItem = new QTableWidgetItem(displayAddress);
        ipItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(client.id));
        clientListTable_->setItem(row, 0, ipItem);

        const QString roleText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(client.role));
        // 当前连接对象的备注优先于持久化缓存，避免旧设备键或重复 deviceId
        // 在刷新时覆盖另一条连接的独立备注。
        const QString displayNote = resolvedClientNote(client, clients);
        auto* roleNoteItem = new QTableWidgetItem(QStringLiteral("%1 / %2").arg(roleText,
            displayNote.isEmpty() ? QStringLiteral("（无备注）") : displayNote));
        roleNoteItem->setToolTip(displayNote);
        clientListTable_->setItem(row, 1, roleNoteItem);

        const QString displayStatus = client.role == ClientRole::Coin
            ? client.listingStatus
            : client.teamReadyText;
        auto* listingStatusItem = new QTableWidgetItem(displayStatus);
        listingStatusItem->setTextAlignment(Qt::AlignCenter);
        if (client.role == ClientRole::Coin)
        {
            if (client.listingStatus == QStringLiteral("成功"))
            {
                listingStatusItem->setForeground(QColor(QStringLiteral("#16a34a")));
            }
            else if (client.listingStatus == QStringLiteral("少"))
            {
                listingStatusItem->setForeground(QColor(QStringLiteral("#d97706")));
            }
            else if (client.listingStatus == QStringLiteral("超出") || client.listingStatus == QStringLiteral("失败"))
            {
                listingStatusItem->setForeground(QColor(QStringLiteral("#dc2626")));
            }
        }
        else if (client.teamReadyState == 1)
        {
            listingStatusItem->setForeground(QColor(QStringLiteral("#16a34a")));
        }
        else if (client.teamReadyState == 0)
        {
            listingStatusItem->setForeground(QColor(QStringLiteral("#d97706")));
        }
        else
        {
            listingStatusItem->setForeground(QColor(QStringLiteral("#64748b")));
        }
        clientListTable_->setItem(row, 2, listingStatusItem);

        const bool isBoss = client.role == ClientRole::Boss;
        if (isBoss)
        {
            auto* expectedEdit = new QLineEdit(client.expectedAmount == QStringLiteral("-") ? QString() : client.expectedAmount, clientListTable_);
            expectedEdit->setObjectName(QStringLiteral("expectedAmountEdit"));
            expectedEdit->setPlaceholderText(QStringLiteral("期望金额"));
            expectedEdit->setValidator(new QIntValidator(0, 2147483647, expectedEdit));
            connect(expectedEdit, &QLineEdit::editingFinished, this, [this, expectedEdit, clientId = client.id]() {
                controller_.setBossExpectedAmount(clientId, expectedEdit->text());
            });
            clientListTable_->setCellWidget(row, 3, expectedEdit);
        }
        else
        {
            clientListTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("-")));
        }
        clientListTable_->setItem(row, 4, new QTableWidgetItem(
            formatAmountForDisplay(client.currentAmount)));
        clientListTable_->setItem(row, 5, new QTableWidgetItem(
            isBoss && client.hasAmountBaseline ? formatAmountForDisplay(QString::number(client.cumulativeIncome))
                : QStringLiteral("-")));
        clientListTable_->setItem(row, 6, new QTableWidgetItem(isBoss
            ? QStringLiteral("低价:%1 / 高价:%2").arg(client.lowPriceCount).arg(client.highPriceCount)
            : QStringLiteral("-")));

        auto* idItem = clientListTable_->item(row, 0);
        idItem->setData(Qt::UserRole + 1, client.deviceId);
    }
    refreshPurchaseClients(displayClients);
    clientCountBadge_->setText(QStringLiteral("%1 客户端").arg(clients.size()));
}

void MainWindow::updateListeningState(bool listening)
{
    portEdit_->setEnabled(!listening);
    listenButton_->setEnabled(!listening);
    stopButton_->setEnabled(listening);
    listenBadge_->setText(listening ? QStringLiteral("监听中") : QStringLiteral("未监听"));
}

QString MainWindow::resolvedClientNote(const ClientListItem& client, const QVector<ClientListItem>& clients) const
{
    return client.clientNote.isEmpty() ? clientNotes_.value(clientNoteKey(client, clients)) : client.clientNote;
}

void MainWindow::updateStatus(const QString& text, ServerStatusTone tone)
{
    statusLabel_->setText(text);
    if (tone == ServerStatusTone::Success)
    {
        statusLabel_->setStyleSheet(QStringLiteral("padding:7px 10px;border-radius:8px;background:#dcfce7;color:#166534;"));
    }
    else if (tone == ServerStatusTone::Error)
    {
        statusLabel_->setStyleSheet(QStringLiteral("padding:7px 10px;border-radius:8px;background:#fee2e2;color:#991b1b;"));
    }
    else
    {
        statusLabel_->setStyleSheet(QStringLiteral("padding:7px 10px;border-radius:8px;background:#f5f7fa;color:#4a586d;"));
    }
}

void MainWindow::updateFlowPreview()
{
    const QString delay = groupDelayEdit_->text().isEmpty() ? QStringLiteral("0") : groupDelayEdit_->text();
    const QString captchaMode = bruteCaptcha_ ? QStringLiteral("暴力") : QStringLiteral("AI");
    const QString purchaseMode = tradePurchase_ ? QStringLiteral("交易行") : QStringLiteral("配装");
    const QString optionSummary = QStringLiteral(" · 验证码：%1 · 购买方式：%2")
        .arg(captchaMode, purchaseMode);
    if (compensationEnabled_)
    {
        const QString lead = compensationLeadEdit_->text().isEmpty() ? QStringLiteral("1500") : compensationLeadEdit_->text();
        flowPreviewLabel_->setText(QStringLiteral("延迟补偿：先同步时间（最多 10 秒）→ 提前准备 %1 ms → %2 ms 后执行第二组%3")
            .arg(lead, delay, optionSummary));
        return;
    }
    if (!tradePurchase_)
    {
        flowPreviewLabel_->setText(QStringLiteral("购买方式：配装 · 验证码：%1 · 保存配装参数已配置")
            .arg(captchaMode));
        return;
    }
    if (coinFirst_)
    {
        flowPreviewLabel_->setText(QStringLiteral("流程预览：币号客户端近同时触发 → 精确延迟 %1 ms → 老板触发%2")
            .arg(delay, optionSummary));
    }
    else
    {
        flowPreviewLabel_->setText(QStringLiteral("流程预览：老板触发 → 精确延迟 %1 ms → 币号客户端近同时触发%2")
            .arg(delay, optionSummary));
    }
}

void MainWindow::appendConnectionLog(const QString& text)
{
    if (connectionLogEdit_)
    {
        connectionLogEdit_->append(text);
    }
}

quint64 MainWindow::clientIdForRow(int row) const
{
    QTableWidgetItem* item = clientTable_->item(row, 0);
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}
