#include "MainWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QDateTime>
#include <QDoubleValidator>
#include <QDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QMessageBox>
#include <QSizePolicy>
#include <QTableWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include "../Shared/FramelessTitleBar.h"
#include "../Shared/FramelessWindowSupport.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
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

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("DF 倒子弹服务端"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_StyledBackground, true);
    // 让窗口按内容自然伸缩，避免固定最小高度把顶部调度区压扁。
    resize(1080, 900);
    setMinimumSize(860, 0);

    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("windowRoot"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(new FramelessTitleBar(QStringLiteral("DF 倒子弹服务端"), this, central));
    root->addWidget(createAppHeader());

    auto* body = new QWidget(central);
    body->setObjectName(QStringLiteral("body"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(14, 12, 14, 12);
    bodyLayout->setSpacing(10);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(10);

    auto* listenLayout = new QVBoxLayout();
    listenLayout->setSpacing(8);
    auto* listenRow = new QHBoxLayout();
    listenRow->setSpacing(7);
    portEdit_ = new QLineEdit(controller_.defaultPortText(), this);
    portEdit_->setPlaceholderText(QStringLiteral("监听端口"));
    portEdit_->setValidator(new QIntValidator(1, 65535, portEdit_));
    listenButton_ = new QPushButton(QStringLiteral("启动监听"), this);
    stopButton_ = new QPushButton(QStringLiteral("停止监听"), this);
    listenButton_->setMinimumWidth(74);
    stopButton_->setMinimumWidth(74);
    listenButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    stopButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    listenRow->addWidget(portEdit_, 1);
    listenRow->addWidget(listenButton_);
    listenRow->addWidget(stopButton_);
    listenLayout->addLayout(listenRow);
    auto* listenHint = new QLabel(QStringLiteral("新客户端连接后默认类型为「币号」。"), this);
    listenHint->setObjectName(QStringLiteral("hintText"));
    listenLayout->addWidget(listenHint);
    listingQuantityTotalLabel_ = new QLabel(QStringLiteral("上架总数:0"), this);
    listingQuantityTotalLabel_->setObjectName(QStringLiteral("listingQuantityTotal"));
    listingQuantityTotalLabel_->setAlignment(Qt::AlignCenter);
    listingQuantityTotalLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    listenLayout->addWidget(listingQuantityTotalLabel_);
    listingSuccessClientsLabel_ = new QLabel(QStringLiteral("上架客户端:0/0"), this);
    listingSuccessClientsLabel_->setObjectName(QStringLiteral("listingSuccessClients"));
    listingSuccessClientsLabel_->setAlignment(Qt::AlignCenter);
    listingSuccessClientsLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto* listingSuccessRow = new QHBoxLayout();
    listingSuccessRow->setContentsMargins(0, 0, 0, 0);
    listingSuccessRow->addStretch();
    listingSuccessRow->addWidget(listingSuccessClientsLabel_);
    listenLayout->insertLayout(0, listingSuccessRow);
    topRow->addWidget(createCard(QStringLiteral("监听设置"), listenLayout), 3);

    auto* dispatchLayout = new QVBoxLayout();
    dispatchLayout->setSpacing(8);
    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(7);

    auto* countdownBox = new QVBoxLayout();
    countdownBox->setSpacing(4);
    auto* countdownLabel = new QLabel(QStringLiteral("倒计时 / 秒"), this);
    countdownLabel->setObjectName(QStringLiteral("fieldLabel"));
    countdownEdit_ = new QLineEdit(QStringLiteral("5"), this);
    countdownEdit_->setValidator(new QIntValidator(0, 86400, countdownEdit_));
    countdownBox->addWidget(countdownLabel);
    countdownBox->addWidget(countdownEdit_);

    auto* delayBox = new QVBoxLayout();
    delayBox->setSpacing(4);
    auto* delayLabel = new QLabel(QStringLiteral("分组延迟 / ms"), this);
    delayLabel->setObjectName(QStringLiteral("fieldLabel"));
    groupDelayEdit_ = new QLineEdit(QStringLiteral("100"), this);
    groupDelayEdit_->setValidator(new QIntValidator(0, 600000, groupDelayEdit_));
    delayBox->addWidget(delayLabel);
    delayBox->addWidget(groupDelayEdit_);

    auto* compensationLeadBox = new QVBoxLayout();
    compensationLeadBox->setSpacing(4);
    auto* compensationLeadLabel = new QLabel(QStringLiteral("提前准备 / ms"), this);
    compensationLeadLabel->setObjectName(QStringLiteral("fieldLabel"));
    compensationLeadEdit_ = new QLineEdit(QStringLiteral("1500"), this);
    compensationLeadEdit_->setValidator(new QIntValidator(0, 600000, compensationLeadEdit_));
    compensationLeadEdit_->setToolTip(QStringLiteral("补偿模式下给客户端预留的同步和网络准备时间"));
    compensationLeadBox->addWidget(compensationLeadLabel);
    compensationLeadBox->addWidget(compensationLeadEdit_);

    triggerAllButton_ = new QPushButton(QStringLiteral("执行"), this);
    triggerAllButton_->setAccessibleName(QStringLiteral("全部出发"));
    triggerAllButton_->setObjectName(QStringLiteral("executeButton"));
    listingButton_ = new QPushButton(QStringLiteral("上架"), this);
    listingButton_->setObjectName(QStringLiteral("listingButton"));
    rebindAllButton_ = new QPushButton(QStringLiteral("全部重新绑定"), this);
    forceStopAllButton_ = new QPushButton(QStringLiteral("停止所有动作"), this);
    forceStopAllButton_->setObjectName(QStringLiteral("forceStopAllButton"));
    mailAllButton_ = new QPushButton(QStringLiteral("全部领取邮件"), this);
    actionRow->addLayout(countdownBox, 1);
    actionRow->addLayout(delayBox, 1);
    actionRow->addLayout(compensationLeadBox, 1);
    actionRow->addWidget(triggerAllButton_, 0, Qt::AlignBottom);
    actionRow->addWidget(listingButton_, 0, Qt::AlignBottom);
    dispatchLayout->addLayout(actionRow);

    auto* listingTextRow = new QHBoxLayout();
    listingTextRow->setSpacing(8);
    auto* listingTextLabel = new QLabel(QStringLiteral("上架文本"), this);
    listingTextLabel->setObjectName(QStringLiteral("fieldLabel"));
    listingTextEdit_ = new QLineEdit(this);
    listingTextEdit_->setPlaceholderText(QStringLiteral("请输入要粘贴的上架文本"));
    listingTextEdit_->setClearButtonEnabled(true);
    listingTextRow->addWidget(listingTextLabel);
    listingTextRow->addWidget(listingTextEdit_, 1);
    dispatchLayout->addLayout(listingTextRow);

    auto* listingOptionRow = new QHBoxLayout();
    listingOptionRow->setSpacing(8);
    auto* listingPriceLabel = new QLabel(QStringLiteral("上架价格"), this);
    listingPriceLabel->setObjectName(QStringLiteral("fieldLabel"));
    listingPriceEdit_ = new QLineEdit(this);
    listingPriceEdit_->setPlaceholderText(QStringLiteral("请输入价格"));
    auto* priceValidator = new QDoubleValidator(0.01, 999999999.0, 4, listingPriceEdit_);
    priceValidator->setNotation(QDoubleValidator::StandardNotation);
    listingPriceEdit_->setValidator(priceValidator);
    auto* listingPurchaseCountLabel = new QLabel(QStringLiteral("购买次数"), this);
    listingPurchaseCountLabel->setObjectName(QStringLiteral("fieldLabel"));
    listingPurchaseCountSpin_ = new QSpinBox(this);
    listingPurchaseCountSpin_->setRange(1, 99);
    listingPurchaseCountSpin_->setValue(5);
    listingPurchaseCountSpin_->setToolTip(QStringLiteral("兑换类型按钮的重复点击次数"));
    listingOptionRow->addWidget(listingPriceLabel);
    listingOptionRow->addWidget(listingPriceEdit_, 1);
    listingOptionRow->addWidget(listingPurchaseCountLabel);
    listingOptionRow->addWidget(listingPurchaseCountSpin_);
    dispatchLayout->addLayout(listingOptionRow);

    // 发送顺序按确认稿使用平铺单选，避免下拉框隐藏当前流程。
    auto* orderRow = new QHBoxLayout();
    orderRow->setSpacing(12);
    coinFirstRadio_ = new QRadioButton(QStringLiteral("币号 → 老板"), this);
    bossFirstRadio_ = new QRadioButton(QStringLiteral("老板 → 币号"), this);
    auto* orderButtonGroup = new QButtonGroup(this);
    orderButtonGroup->setExclusive(true);
    orderButtonGroup->addButton(coinFirstRadio_);
    orderButtonGroup->addButton(bossFirstRadio_);
    coinFirstRadio_->setChecked(true);
    coinFirstRadio_->setToolTip(QStringLiteral("币号优先：币号 → 延迟 → 老板，同组币号近同时触发"));
    bossFirstRadio_->setToolTip(QStringLiteral("老板优先：老板触发后，再按设定延迟近同时触发币号客户端"));
    orderRow->addWidget(coinFirstRadio_);
    orderRow->addWidget(bossFirstRadio_);
    normalTriggerRadio_ = new QRadioButton(QStringLiteral("普通模式"), this);
    compensationTriggerRadio_ = new QRadioButton(QStringLiteral("延迟补偿"), this);
    auto* triggerModeButtonGroup = new QButtonGroup(this);
    triggerModeButtonGroup->setExclusive(true);
    triggerModeButtonGroup->addButton(normalTriggerRadio_);
    triggerModeButtonGroup->addButton(compensationTriggerRadio_);
    // 明确设置两个独立单选组的默认状态：币号 → 老板 + 普通模式。
    normalTriggerRadio_->setChecked(true);
    compensationTriggerRadio_->setToolTip(QStringLiteral("先同步客户端时间，再按统一目标时间执行两组指令"));
    orderRow->addWidget(normalTriggerRadio_);
    orderRow->addWidget(compensationTriggerRadio_);
    orderRow->addStretch();
    dispatchLayout->addLayout(orderRow);
    auto* bulkActionRow = new QHBoxLayout();
    bulkActionRow->setSpacing(7);
    bulkActionRow->addStretch();
    clientStatusButton_ = new QPushButton(QStringLiteral("客户端状态详情"), this);
    bulkActionRow->addWidget(clientStatusButton_);
    bulkActionRow->addWidget(rebindAllButton_);
    bulkActionRow->addWidget(forceStopAllButton_);
    bulkActionRow->addWidget(mailAllButton_);
    dispatchLayout->addLayout(bulkActionRow);
    topRow->addWidget(createCard(QStringLiteral("全部出发"), dispatchLayout), 7);
    bodyLayout->addLayout(topRow);

    auto* tableLayout = new QVBoxLayout();
    tableLayout->setContentsMargins(0, 0, 0, 0);
    clientTable_ = new QTableWidget(0, 5, this);
    clientTable_->setHorizontalHeaderLabels({
        QStringLiteral("客户端 IP"), QStringLiteral("备注类型"),
        QStringLiteral("点击坐标"), QStringLiteral("上架状态"), QStringLiteral("操作")
    });
    // 各列使用接近均衡的固定宽度，避免 IP 列独占大量空间。
    clientTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    clientTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    clientTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    clientTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    clientTable_->horizontalHeader()->resizeSection(0, 220);
    clientTable_->horizontalHeader()->resizeSection(1, 154);
    clientTable_->horizontalHeader()->resizeSection(2, 174);
    clientTable_->horizontalHeader()->resizeSection(3, 100);
    clientTable_->horizontalHeader()->resizeSection(4, 254);
    clientTable_->verticalHeader()->setVisible(false);
    clientTable_->verticalHeader()->setDefaultSectionSize(40);
    clientTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clientTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    clientTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableLayout->addWidget(clientTable_);
    bodyLayout->addWidget(createCard(QStringLiteral("客户端列表"), tableLayout), 1);

    flowPreviewLabel_ = new QLabel(this);
    flowPreviewLabel_->setObjectName(QStringLiteral("flowPreview"));
    flowPreviewLabel_->setWordWrap(true);
    bodyLayout->addWidget(flowPreviewLabel_);

    statusLabel_ = new QLabel(QStringLiteral("等待操作"), this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    bodyLayout->addWidget(statusLabel_);
    serverTimeLabel_ = new QLabel(QStringLiteral("服务器时间：%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))), this);
    serverTimeLabel_->setObjectName(QStringLiteral("serverTimeLabel"));
    bodyLayout->addWidget(serverTimeLabel_);

    connectionLogEdit_ = new QTextEdit(this);
    connectionLogEdit_->setObjectName(QStringLiteral("connectionLog"));
    connectionLogEdit_->setReadOnly(true);
    connectionLogEdit_->setPlaceholderText(QStringLiteral("监听、连接与调度日志将在这里显示。"));
    bodyLayout->addWidget(connectionLogEdit_, 1);

    root->addWidget(body, 1);
    setCentralWidget(central);
    applyStyle();
}

void MainWindow::connectSignals()
{
    connect(listenButton_, &QPushButton::clicked, this, [this]() {
        controller_.startListening(portEdit_->text());
    });
    connect(stopButton_, &QPushButton::clicked, &controller_, &ServerController::stopListening);
    connect(triggerAllButton_, &QPushButton::clicked, this, [this]() {
        controller_.triggerClients(countdownEdit_->text(), groupDelayEdit_->text(), coinFirstRadio_->isChecked(),
            compensationTriggerRadio_->isChecked(), compensationLeadEdit_->text());
        appendConnectionLog(QStringLiteral("已提交全部出发指令。"));
    });
    connect(listingButton_, &QPushButton::clicked, this, [this]() {
        controller_.startListing(listingTextEdit_->text(), listingPriceEdit_->text(), false, listingPurchaseCountSpin_->value());
        appendConnectionLog(QStringLiteral("已提交币号上架任务。"));
    });
    connect(forceStopAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestForceStopAllActions);
    connect(mailAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestMailAll);
    connect(rebindAllButton_, &QPushButton::clicked, &controller_, &ServerController::requestRebindAllWindows);
    connect(clientStatusButton_, &QPushButton::clicked, this, [this]() {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("客户端状态详情"));
        dialog->resize(1260, 480);

        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        auto* table = new QTableWidget(0, 7, dialog);
        table->setHorizontalHeaderLabels({
            QStringLiteral("客户端"), QStringLiteral("IP"), QStringLiteral("备注类型"),
            QStringLiteral("当前动作"), QStringLiteral("上架状态"), QStringLiteral("点击坐标"), QStringLiteral("操作")
        });
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->horizontalHeader()->resizeSection(0, 75);
        table->horizontalHeader()->resizeSection(1, 220);
        table->horizontalHeader()->resizeSection(2, 120);
        table->horizontalHeader()->resizeSection(3, 150);
        table->horizontalHeader()->resizeSection(4, 100);
        table->horizontalHeader()->resizeSection(5, 170);
        table->horizontalHeader()->resizeSection(6, 270);
        layout->addWidget(table);

        const auto refreshDetailTable = [table, this](const QVector<ClientListItem>& clients) {
            table->setRowCount(0);
            for (const ClientListItem& client : clients)
            {
                const int row = table->rowCount();
                table->insertRow(row);
                const QString roleText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(client.role));
                const QString coordinate = client.role == ClientRole::Coin
                    ? QStringLiteral("X: 1756  Y: 1000")
                    : QStringLiteral("X: 2262  Y: 1159");
                const QStringList values = {
                    QString::number(client.id), client.ip, roleText,
                    client.actionRunning ? client.runningAction : QStringLiteral("空闲"),
                    client.role == ClientRole::Coin ? client.listingStatus : QStringLiteral("不适用"),
                    coordinate
                };
                for (int column = 0; column < values.size(); ++column)
                {
                    auto* item = new QTableWidgetItem(values.at(column));
                    if (column == 4)
                    {
                        item->setTextAlignment(Qt::AlignCenter);
                        const QString status = values.at(column);
                        if (status == QStringLiteral("成功"))
                        {
                            item->setForeground(QColor(QStringLiteral("#16a34a")));
                        }
                        else if (status == QStringLiteral("少"))
                        {
                            item->setForeground(QColor(QStringLiteral("#d97706")));
                        }
                        else if (status == QStringLiteral("超出") || status == QStringLiteral("失败"))
                        {
                            item->setForeground(QColor(QStringLiteral("#dc2626")));
                        }
                    }
                    table->setItem(row, column, item);
                }

                auto* actionCell = new QWidget(table);
                auto* actionLayout = new QHBoxLayout(actionCell);
                actionLayout->setContentsMargins(2, 2, 2, 2);
                actionLayout->setSpacing(3);
                auto* rebindButton = new QPushButton(QStringLiteral("重新绑定"), actionCell);
                auto* stopActionButton = new QPushButton(QStringLiteral("强制停止"), actionCell);
                auto* kickButton = new QPushButton(QStringLiteral("断开"), actionCell);
                rebindButton->setMinimumWidth(72);
                stopActionButton->setMinimumWidth(72);
                kickButton->setMinimumWidth(52);
                connect(rebindButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
                    controller_.requestRebindWindow(clientId);
                });
                connect(stopActionButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
                    controller_.requestForceStopAction(clientId);
                });
                connect(kickButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
                    controller_.kickClient(clientId);
                });
                actionLayout->addWidget(rebindButton);
                actionLayout->addWidget(stopActionButton);
                actionLayout->addWidget(kickButton);
                table->setCellWidget(row, 6, actionCell);
            }
        };
        connect(&controller_, &ServerController::clientsChanged, dialog, refreshDetailTable);
        refreshDetailTable(controller_.clients());
        dialog->show();
    });
    connect(coinFirstRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
        {
            updateFlowPreview();
        }
    });
    connect(bossFirstRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
        {
            updateFlowPreview();
        }
    });
    connect(groupDelayEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateFlowPreview();
    });
    connect(compensationLeadEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateFlowPreview();
    });
    connect(normalTriggerRadio_, &QRadioButton::toggled, this, [this](bool) {
        updateFlowPreview();
    });
    connect(compensationTriggerRadio_, &QRadioButton::toggled, this, [this](bool) {
        updateFlowPreview();
    });

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
    listenBadge_ = createBadge(QStringLiteral("未监听"), QStringLiteral("rgba(255,255,255,0.16)"), QStringLiteral("#ffffff"));
    clientCountBadge_ = createBadge(QStringLiteral("0 客户端"), QStringLiteral("rgba(255,255,255,0.16)"), QStringLiteral("#ffffff"));
    layout->addWidget(listenBadge_);
    layout->addWidget(clientCountBadge_);
    return header;
}

QWidget* MainWindow::createCard(const QString& title, QLayout* contentLayout)
{
    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(13, 11, 13, 11);
    layout->setSpacing(8);
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
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
        QMainWindow, #windowRoot { background:#eef3fb; color:#15233b; font-family:"Microsoft YaHei UI"; font-size:12px; }
        #framelessTitleBar { background:#f8fafc; border-bottom:1px solid #dce4ef; }
        #windowTitleText { color:#172033; font-size:12px; font-weight:700; }
        #windowMinimizeDot, #windowMaximizeDot, #windowCloseDot { border:0; border-radius:4px; min-width:9px; min-height:9px; max-width:9px; max-height:9px; padding:0; }
        #windowMinimizeDot { background:#f4c84a; }
        #windowMaximizeDot { background:#56c779; }
        #windowCloseDot { background:#ef6a6a; }
        #appHeader { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #102a5c,stop:1 #245bc4); }
        #appTitle { color:white; font-size:20px; font-weight:800; }
        #appSubtitle { color:#dbeafe; font-size:12px; }
        #body { background:#eef3fb; }
        #card { background:#ffffff; border:1px solid #dbe4f0; border-radius:12px; }
        #cardTitle { color:#17233b; font-size:14px; font-weight:800; }
         #fieldLabel, #hintText { color:#70809a; font-size:11px; }
         #listingQuantityTotal, #listingSuccessClients { border:1px solid #93c5fd; border-radius:9px; background:#eff6ff; color:#1d4ed8; padding:6px 10px; font-size:14px; font-weight:800; }
        QLineEdit { min-height:30px; border:1px solid #cfd8e6; border-radius:7px; padding:0 9px; background:#f9fbfe; color:#1e2b43; }
        QLineEdit:focus { border-color:#3f76d8; background:white; }
        QRadioButton { min-height:22px; color:#334155; spacing:6px; font-weight:700; }
        QRadioButton::indicator { width:12px; height:12px; border:2px solid #2563eb; border-radius:7px; background:#ffffff; }
        QRadioButton::indicator:checked { background:#2563eb; }
        QPushButton { min-height:30px; border:1px solid #cbd5e1; border-radius:7px; background:#ffffff; color:#334155; padding:0 11px; font-weight:700; }
        QPushButton:hover { background:#eff5ff; border-color:#91addb; }
        QPushButton:disabled { color:#9aa8ba; background:#edf1f6; border-color:#d9e0e9; }
        #roleCell { background:transparent; }
        #roleSelector { background:#edf2f8; border:1px solid #d2dae6; border-radius:14px; }
        #coinRoleButton, #bossRoleButton { min-width:44px; max-width:44px; min-height:26px; max-height:26px; border:0; border-radius:12px; padding:0 6px; background:transparent; color:#64748b; }
        #coinRoleButton:hover, #bossRoleButton:hover { background:#e1e8f2; }
        #coinRoleButton:checked { background:#2563eb; color:white; }
        #bossRoleButton:checked { background:#7653c9; color:white; }
        #executeButton { background:#18a36f; color:white; border-color:#18a36f; }
        #executeButton:hover { background:#12865b; }
        #listingButton { background:#7653c9; color:white; border-color:#7653c9; }
        #listingButton:hover { background:#6343af; }
         QTableWidget { background:white; border:0; gridline-color:#e4e9f1; selection-background-color:#e8f0ff; }
         QTableWidget::item:selected { color:#1e2b43; background:#e8f0ff; }
        QHeaderView::section { background:#f1f4f9; color:#5b6980; font-weight:800; border:0; border-right:1px solid #e1e7ef; border-bottom:1px solid #e1e7ef; padding:7px; }
        #flowPreview { padding:8px 11px; border-radius:8px; background:#e9f1ff; color:#2453a6; font-weight:700; }
        #statusLabel { padding:7px 10px; border-radius:8px; background:#f5f7fa; color:#4a586d; }
        #connectionLog { background:#0f172a; color:#a7f3d0; border:1px solid #1f2b45; border-radius:10px; padding:8px; font-family:"Consolas","Microsoft YaHei UI"; }
    )"));
}

void MainWindow::refreshClients(const QVector<ClientListItem>& clients)
{
    int listingQuantityTotal = 0;
    int connectedCoinCount = 0;
    int successfulListingClientCount = 0;
    for (const ClientListItem& client : clients)
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
    clientTable_->setRowCount(0);
    for (const ClientListItem& client : clients)
    {
        const int row = clientTable_->rowCount();
        clientTable_->insertRow(row);

        auto* ipItem = new QTableWidgetItem(client.ip);
        ipItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(client.id));
        clientTable_->setItem(row, 0, ipItem);

        // 备注类型使用互斥胶囊按钮，点击后仍复用现有角色更新与调度链路。
        auto* roleCell = new QWidget(clientTable_);
        roleCell->setObjectName(QStringLiteral("roleCell"));
        auto* roleCellLayout = new QHBoxLayout(roleCell);
        roleCellLayout->setContentsMargins(0, 0, 0, 0);
        roleCellLayout->setSpacing(0);
        auto* roleSelector = new QWidget(roleCell);
        roleSelector->setObjectName(QStringLiteral("roleSelector"));
        roleSelector->setFixedWidth(132);
        auto* roleLayout = new QHBoxLayout(roleSelector);
        roleLayout->setContentsMargins(2, 2, 2, 2);
        roleLayout->setSpacing(2);
        auto* coinButton = new QPushButton(QStringLiteral("币号"), roleSelector);
        coinButton->setObjectName(QStringLiteral("coinRoleButton"));
        coinButton->setCheckable(true);
        auto* bossButton = new QPushButton(QStringLiteral("老板"), roleSelector);
        bossButton->setObjectName(QStringLiteral("bossRoleButton"));
        bossButton->setCheckable(true);
        auto* roleGroup = new QButtonGroup(roleSelector);
        roleGroup->setExclusive(true);
        roleGroup->addButton(coinButton);
        roleGroup->addButton(bossButton);
        coinButton->setChecked(client.role == ClientRole::Coin);
        bossButton->setChecked(client.role == ClientRole::Boss);
        roleLayout->addWidget(coinButton);
        roleLayout->addWidget(bossButton);
        roleCellLayout->addWidget(roleSelector, 0, Qt::AlignCenter);
        connect(coinButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.setClientRole(clientId, ClientRole::Coin);
        });
        connect(bossButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.setClientRole(clientId, ClientRole::Boss);
        });
        clientTable_->setCellWidget(row, 1, roleCell);

        const QString coordinate = client.role == ClientRole::Coin
            ? QStringLiteral("X: 1756  Y: 1000")
            : QStringLiteral("X: 2262  Y: 1159");
        clientTable_->setItem(row, 2, new QTableWidgetItem(coordinate));

        auto* listingStatusItem = new QTableWidgetItem(client.role == ClientRole::Coin
            ? client.listingStatus : QStringLiteral("不适用"));
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
        clientTable_->setItem(row, 3, listingStatusItem);

        auto* actionCell = new QWidget(clientTable_);
        auto* actionLayout = new QHBoxLayout(actionCell);
        actionLayout->setContentsMargins(2, 2, 2, 2);
        actionLayout->setSpacing(3);
        auto* rebindButton = new QPushButton(QStringLiteral("重新绑定"), actionCell);
        auto* stopActionButton = new QPushButton(QStringLiteral("强制停止"), actionCell);
        auto* mailButton = new QPushButton(QStringLiteral("领取邮件"), actionCell);
        auto* kickButton = new QPushButton(QStringLiteral("断开"), actionCell);
        rebindButton->setMinimumWidth(70);
        stopActionButton->setMinimumWidth(70);
        mailButton->setMinimumWidth(70);
        kickButton->setMinimumWidth(50);
        connect(rebindButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.requestRebindWindow(clientId);
        });
        connect(stopActionButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.requestForceStopAction(clientId);
        });
        connect(mailButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.requestMail(clientId);
        });
        connect(kickButton, &QPushButton::clicked, this, [this, clientId = client.id]() {
            controller_.kickClient(clientId);
        });
        actionLayout->addWidget(rebindButton);
        actionLayout->addWidget(stopActionButton);
        actionLayout->addWidget(mailButton);
        actionLayout->addWidget(kickButton);
        clientTable_->setCellWidget(row, 4, actionCell);
    }
    clientCountBadge_->setText(QStringLiteral("%1 客户端").arg(clients.size()));
}

void MainWindow::updateListeningState(bool listening)
{
    portEdit_->setEnabled(!listening);
    listenButton_->setEnabled(!listening);
    stopButton_->setEnabled(listening);
    listenBadge_->setText(listening ? QStringLiteral("监听中") : QStringLiteral("未监听"));
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
    if (compensationTriggerRadio_ && compensationTriggerRadio_->isChecked())
    {
        const QString lead = compensationLeadEdit_->text().isEmpty() ? QStringLiteral("1500") : compensationLeadEdit_->text();
        flowPreviewLabel_->setText(QStringLiteral("延迟补偿：先同步时间（最多 10 秒）→ 提前准备 %1 ms → %2 ms 后执行第二组")
            .arg(lead, delay));
        return;
    }
    if (coinFirstRadio_->isChecked())
    {
        flowPreviewLabel_->setText(QStringLiteral("流程预览：币号客户端近同时触发 → 精确延迟 %1 ms → 老板触发").arg(delay));
    }
    else
    {
        flowPreviewLabel_->setText(QStringLiteral("流程预览：老板触发 → 精确延迟 %1 ms → 币号客户端近同时触发").arg(delay));
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
