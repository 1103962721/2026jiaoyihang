#include "MainWindow.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleValidator>
#include <QApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QIntValidator>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QSizePolicy>
#include <QThread>
#include <QTimer>
#include <QTextCursor>
#include <QTextDocument>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

#include "../Shared/FramelessTitleBar.h"
#include "../Shared/FramelessWindowSupport.h"
#include "../Shared/Protocol.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    if (qApp)
    {
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
            clearCloudBossSessions();
            controller_.shutdownForExit();
        }, Qt::DirectConnection);
    }
    connectionLogFlushTimer_.setSingleShot(true);
    connectionLogFlushTimer_.setInterval(100);
    connect(&connectionLogFlushTimer_, &QTimer::timeout, this, &MainWindow::flushConnectionLog);
    buildUi();
    connectSignals();
    refreshConnectionControls();
}

MainWindow::~MainWindow()
{
    clearCloudBossSessions();
    controller_.shutdownForExit();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!initialized_)
    {
        initialized_ = true;
        // 控制器依赖真实 HWND，必须等窗口首次显示并创建原生句柄后再初始化。
        controller_.initialize(reinterpret_cast<HWND>(winId()));
        refreshCloudWindowOptions();
    }
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

QWidget* MainWindow::createAppHeader()
{
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("appHeader"));
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setSpacing(10);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(3);
    auto* title = new QLabel(QStringLiteral("客户端执行端"), header);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("服务器连接 · OCR 上架判定 · 本地点击"), header);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    titles->addWidget(title);
    titles->addWidget(subtitle);
    layout->addLayout(titles);
    layout->addStretch();

    connectionBadge_ = createBadge(QStringLiteral("未连接"), QStringLiteral("rgba(255,255,255,0.16)"), QStringLiteral("#ffffff"));
    connectionBadge_->setMinimumWidth(68);
    connectionBadge_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    layout->addWidget(connectionBadge_);
    return header;
}

QWidget* MainWindow::createCard(const QString& title, QLayout* contentLayout)
{
    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(7);
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    layout->addWidget(titleLabel);
    layout->addLayout(contentLayout);
    return card;
}

QWidget* MainWindow::createMetricPanel(const QString& caption, QLabel*& valueLabel)
{
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("metricPanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 7, 8, 7);
    layout->setSpacing(2);
    valueLabel = new QLabel(QStringLiteral("—"), panel);
    valueLabel->setObjectName(QStringLiteral("metricValue"));
    valueLabel->setAlignment(Qt::AlignCenter);
    auto* captionLabel = new QLabel(caption, panel);
    captionLabel->setObjectName(QStringLiteral("metricCaption"));
    captionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(valueLabel);
    layout->addWidget(captionLabel);
    return panel;
}

QLabel* MainWindow::createBadge(const QString& text, const QString& background, const QString& color)
{
    auto* badge = new QLabel(text, this);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral("padding:5px 10px;border-radius:11px;background:%1;color:%2;font-weight:700;")
                             .arg(background, color));
    return badge;
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("DF 倒子弹客户端"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_StyledBackground, true);
    resize(620, 760);
    // 内容区使用滚动容器承载，窗口缩放到较小高度时不再压缩卡片内部控件。
    // 宽度仍保留原有下限，高度不再设置 720px 的硬限制。
    setMinimumSize(580, 0);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("windowRoot"));
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(new FramelessTitleBar(QStringLiteral("DF 倒子弹客户端"), this, root));
    rootLayout->addWidget(createAppHeader());

    auto* body = new QWidget(root);
    body->setObjectName(QStringLiteral("body"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(13, 10, 13, 11);
    bodyLayout->setSpacing(8);
    bodyLayout->setSizeConstraint(QLayout::SetMinimumSize);

    auto* modeLayout = new QHBoxLayout();
    modeLayout->setSpacing(8);
    auto* modeLabel = new QLabel(QStringLiteral("绑定模式"), this);
    modeLabel->setObjectName(QStringLiteral("fieldLabel"));
    bindingModeCombo_ = new QComboBox(this);
    bindingModeCombo_->addItem(QStringLiteral("PC：Delta Force 窗口"));
    bindingModeCombo_->addItem(QStringLiteral("手机：红手指 ADB"));
    bindingModeCombo_->setMaximumWidth(220);
    modeLayout->addWidget(modeLabel);
    modeLayout->addWidget(bindingModeCombo_);
    modeLayout->addStretch();
    bodyLayout->addLayout(modeLayout);

    auto* connectionLayout = new QVBoxLayout();
    connectionLayout->setSpacing(7);
    auto* connectionRow = new QHBoxLayout();
    connectionRow->setSpacing(6);

    auto* ipBox = new QVBoxLayout();
    ipBox->setSpacing(3);
    auto* ipLabel = new QLabel(QStringLiteral("服务器 IP"), this);
    ipLabel->setObjectName(QStringLiteral("fieldLabel"));
    ipEdit_ = new QLineEdit(controller_.lastServerIp(), this);
    ipEdit_->setPlaceholderText(QStringLiteral("服务器 IP"));
    ipBox->addWidget(ipLabel);
    ipBox->addWidget(ipEdit_);

    auto* portBox = new QVBoxLayout();
    portBox->setSpacing(3);
    auto* portLabel = new QLabel(QStringLiteral("端口"), this);
    portLabel->setObjectName(QStringLiteral("fieldLabel"));
    portEdit_ = new QLineEdit(QString::number(Protocol::DefaultPort), this);
    portEdit_->setPlaceholderText(QStringLiteral("端口"));
    portEdit_->setValidator(new QIntValidator(1, 65535, portEdit_));
    portEdit_->setMaximumWidth(86);
    portBox->addWidget(portLabel);
    portBox->addWidget(portEdit_);

    connectButton_ = new QPushButton(QStringLiteral("连接"), this);
    connectButton_->setObjectName(QStringLiteral("connectButton"));
    disconnectButton_ = new QPushButton(QStringLiteral("断开"), this);
    disconnectButton_->setObjectName(QStringLiteral("disconnectButton"));
    connectButton_->setMinimumWidth(58);
    disconnectButton_->setMinimumWidth(58);
    connectionRow->addLayout(ipBox, 1);
    connectionRow->addLayout(portBox);
    connectionRow->addWidget(connectButton_, 0, Qt::AlignBottom);
    connectionRow->addWidget(disconnectButton_, 0, Qt::AlignBottom);
    connectionLayout->addLayout(connectionRow);

    auto* connectionStatusRow = new QHBoxLayout();
    auto* connectionProtocolLabel = new QLabel(QStringLiteral("TCP / 心跳 / 自动重连"), this);
    connectionProtocolLabel->setObjectName(QStringLiteral("hintText"));
    connectionDetailLabel_ = new QLabel(QStringLiteral("等待连接"), this);
    connectionDetailLabel_->setObjectName(QStringLiteral("connectionDetail"));
    connectionStatusRow->addWidget(connectionProtocolLabel);
    connectionStatusRow->addStretch();
    serverTimeLabel_ = new QLabel(QStringLiteral("服务器时间：未同步"), this);
    serverTimeLabel_->setObjectName(QStringLiteral("serverTimeLabel"));
    connectionStatusRow->addWidget(serverTimeLabel_);
    connectionStatusRow->addWidget(connectionDetailLabel_);
    connectionLayout->addLayout(connectionStatusRow);
    bodyLayout->addWidget(createCard(QStringLiteral("连接服务器"), connectionLayout));

    auto* ocrLayout = new QVBoxLayout();
    ocrLayout->setSpacing(7);
    auto* parameterRow = new QHBoxLayout();
    parameterRow->setSpacing(8);

    auto* unitPriceBox = new QVBoxLayout();
    unitPriceBox->setSpacing(3);
    auto* unitPriceLabel = new QLabel(QStringLiteral("单价 / 元"), this);
    unitPriceLabel->setObjectName(QStringLiteral("fieldLabel"));
    unitPriceEdit_ = new QLineEdit(QString::number(controller_.unitPrice(), 'g', 17), this);
    auto* priceValidator = new QDoubleValidator(0.0, 1000000000.0, 8, unitPriceEdit_);
    priceValidator->setNotation(QDoubleValidator::StandardNotation);
    unitPriceEdit_->setValidator(priceValidator);
    unitPriceBox->addWidget(unitPriceLabel);
    unitPriceBox->addWidget(unitPriceEdit_);

    auto* bulletBox = new QVBoxLayout();
    bulletBox->setSpacing(3);
    auto* bulletLabel = new QLabel(QStringLiteral("子弹总数 / 发"), this);
    bulletLabel->setObjectName(QStringLiteral("fieldLabel"));
    bulletTotalEdit_ = new QLineEdit(QString::number(controller_.bulletTotal()), this);
    bulletTotalEdit_->setValidator(new QIntValidator(1, 1000000000, bulletTotalEdit_));
    bulletBox->addWidget(bulletLabel);
    bulletBox->addWidget(bulletTotalEdit_);
    parameterRow->addLayout(unitPriceBox, 1);
    parameterRow->addLayout(bulletBox, 1);
    ocrLayout->addLayout(parameterRow);

    auto* metricsRow = new QHBoxLayout();
    metricsRow->setSpacing(7);
    metricsRow->addWidget(createMetricPanel(QStringLiteral("OCR 识别结果"), ocrValueLabel_), 1);
    metricsRow->addWidget(createMetricPanel(QStringLiteral("结果 ÷ 子弹数"), unitValueLabel_), 1);
    metricsRow->addWidget(createMetricPanel(QStringLiteral("设定单价"), configuredUnitPriceLabel_), 1);
    configuredUnitPriceLabel_->setText(QString::number(controller_.unitPrice(), 'g', 17));
    ocrLayout->addLayout(metricsRow);

    ocrDecisionLabel_ = new QLabel(QStringLiteral("等待 OCR 识别结果"), this);
    ocrDecisionLabel_->setObjectName(QStringLiteral("decisionLabel"));
    ocrLayout->addWidget(ocrDecisionLabel_);
    auto* ocrHint = new QLabel(QStringLiteral("OCR 区域按当前分辨率自动适配；按 F3 启动/停止 OCR 识别。"), this);
    ocrHint->setObjectName(QStringLiteral("hintText"));
    ocrHint->setWordWrap(true);
    ocrLayout->addWidget(ocrHint);
    pcOcrCard_ = createCard(QStringLiteral("OCR 上架判定"), ocrLayout);
    bodyLayout->addWidget(pcOcrCard_);

    auto* executionLayout = new QVBoxLayout();
    executionLayout->setSpacing(7);
    auto* executionPanels = new QHBoxLayout();
    executionPanels->setSpacing(7);

    auto* rolePanel = new QWidget(this);
    rolePanel->setObjectName(QStringLiteral("executionPanel"));
    auto* roleLayout = new QVBoxLayout(rolePanel);
    roleLayout->setContentsMargins(9, 7, 9, 7);
    roleLayout->setSpacing(2);
    auto* roleCaption = new QLabel(QStringLiteral("角色"), rolePanel);
    roleCaption->setObjectName(QStringLiteral("executionCaption"));
    clientRoleLabel_ = new QLabel(QStringLiteral("由服务端下发：%1")
        .arg(QString::fromStdWString(ClientRoleUtil::ToDisplayText(controller_.clientRole()))), rolePanel);
    clientRoleLabel_->setObjectName(QStringLiteral("executionValue"));
    roleLayout->addWidget(roleCaption);
    roleLayout->addWidget(clientRoleLabel_);

    auto* clickPanel = new QWidget(this);
    clickPanel->setObjectName(QStringLiteral("executionPanel"));
    auto* clickLayout = new QVBoxLayout(clickPanel);
    clickLayout->setContentsMargins(9, 7, 9, 7);
    clickLayout->setSpacing(2);
    auto* clickCaption = new QLabel(QStringLiteral("点击位置"), clickPanel);
    clickCaption->setObjectName(QStringLiteral("executionCaption"));
    clickStrategyLabel_ = new QLabel(QStringLiteral("币号固定坐标 · 已适配"), clickPanel);
    clickStrategyLabel_->setObjectName(QStringLiteral("executionValue"));
    clickLayout->addWidget(clickCaption);
    clickLayout->addWidget(clickStrategyLabel_);
    executionPanels->addWidget(rolePanel, 1);
    executionPanels->addWidget(clickPanel, 1);
    executionLayout->addLayout(executionPanels);

    executionStatusLabel_ = new QLabel(QStringLiteral("最近指令：等待服务端指令"), this);
    executionStatusLabel_->setObjectName(QStringLiteral("executionStatus"));
    executionLayout->addWidget(executionStatusLabel_);
    auto* executionHint = new QLabel(QStringLiteral("客户端不提供手动取点；接收服务端角色指令后执行对应固定坐标。"), this);
    executionHint->setObjectName(QStringLiteral("hintText"));
    executionHint->setWordWrap(true);
    executionLayout->addWidget(executionHint);
    pcExecutionCard_ = createCard(QStringLiteral("本机执行状态"), executionLayout);
    bodyLayout->addWidget(pcExecutionCard_);

    auto* cloudLayout = new QVBoxLayout();
    cloudLayout->setSpacing(6);
    auto* cloudRow = new QHBoxLayout();
    cloudRow->setSpacing(6);
    auto* cloudLabel = new QLabel(QStringLiteral("ADB 端口"), this);
    cloudLabel->setObjectName(QStringLiteral("fieldLabel"));
    cloudAdbPortEdit_ = new QLineEdit(QStringLiteral("63651"), this);
    cloudAdbPortEdit_->setValidator(new QIntValidator(1, 65535, cloudAdbPortEdit_));
    cloudAdbPortEdit_->setMinimumWidth(140);
    cloudAdbPortEdit_->setMaximumWidth(180);
    cloudAdbPortEdit_->setToolTip(QStringLiteral("用于标识对应的红手指云手机，例如 63651"));
    cloudRefreshButton_ = new QPushButton(QStringLiteral("刷新窗口"), this);
    cloudAddButton_ = new QPushButton(QStringLiteral("添加老板云手机"), this);
    cloudRow->addWidget(cloudLabel);
    cloudRow->addWidget(cloudAdbPortEdit_);
    cloudRow->addWidget(cloudRefreshButton_);
    cloudRow->addWidget(cloudAddButton_);
    cloudRow->addStretch();
    cloudLayout->addLayout(cloudRow);
    cloudWindowList_ = new QListWidget(this);
    cloudWindowList_->setObjectName(QStringLiteral("cloudWindowList"));
    cloudWindowList_->setSelectionMode(QAbstractItemView::SingleSelection);
    cloudWindowList_->setMinimumHeight(58);
    cloudWindowList_->setMaximumHeight(112);
    cloudWindowList_->setAlternatingRowColors(true);
    cloudWindowList_->setToolTip(QStringLiteral("选择要绑定的红手指云手机画面窗口"));
    cloudLayout->addWidget(cloudWindowList_);
    cloudSessionList_ = new QListWidget(this);
    cloudSessionList_->setObjectName(QStringLiteral("cloudSessionList"));
    cloudSessionList_->setMinimumHeight(52);
    cloudSessionList_->setAlternatingRowColors(true);
    cloudLayout->addWidget(cloudSessionList_);
    auto* cloudHint = new QLabel(QStringLiteral("每个 ADB 端口对应一个独立老板客户端；截图和 OCR 直接使用设备 1280×720 分辨率。"), this);
    cloudHint->setObjectName(QStringLiteral("hintText"));
    cloudHint->setWordWrap(true);
    cloudLayout->addWidget(cloudHint);
    cloudSessionCard_ = createCard(QStringLiteral("手机老板会话"), cloudLayout);
    cloudSessionCard_->hide();
    bodyLayout->addWidget(cloudSessionCard_);

    connectionLogEdit_ = new QTextEdit(this);
    connectionLogEdit_->setObjectName(QStringLiteral("connectionLog"));
    connectionLogEdit_->setReadOnly(true);
    connectionLogEdit_->setMinimumHeight(66);
    connectionLogEdit_->setPlaceholderText(QStringLiteral("连接、OCR 与本地执行日志将在这里显示。"));
    bodyLayout->addWidget(connectionLogEdit_, 1);

    auto* bodyScrollArea = new QScrollArea(root);
    bodyScrollArea->setObjectName(QStringLiteral("bodyScrollArea"));
    bodyScrollArea->setFrameShape(QFrame::NoFrame);
    bodyScrollArea->setWidgetResizable(true);
    bodyScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bodyScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bodyScrollArea->setWidget(body);
    rootLayout->addWidget(bodyScrollArea, 1);
    setCentralWidget(root);
    applyStyle();
}

void MainWindow::connectSignals()
{
    connect(connectButton_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const int port = portEdit_->text().toInt(&ok);
        if (!ok || ipEdit_->text().trimmed().isEmpty())
        {
            showControllerMessage(QStringLiteral("连接参数错误"), QStringLiteral("请输入有效的服务器 IP 和端口"), true);
            return;
        }
        controller_.connectOrDisconnect(ipEdit_->text().trimmed(), port);
    });
    connect(disconnectButton_, &QPushButton::clicked, this, [this]() {
        controller_.connectOrDisconnect(ipEdit_->text().trimmed(), portEdit_->text().toInt());
    });
    connect(unitPriceEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveOcrParameters);
    connect(bulletTotalEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveOcrParameters);
    connect(cloudAddButton_, &QPushButton::clicked, this, &MainWindow::addCloudBossSession);
    connect(cloudRefreshButton_, &QPushButton::clicked, this, &MainWindow::refreshCloudWindowOptions);
    connect(bindingModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &MainWindow::applyClientBindingMode);

    connect(&controller_, &ClientController::connectedChanged, this, &MainWindow::updateConnectedState);
    connect(&controller_, &ClientController::reconnectingChanged, this, &MainWindow::updateReconnectingState);
    connect(&controller_, &ClientController::statusChanged, this, [this](const QString& text) {
        executionStatusLabel_->setText(QStringLiteral("最近指令：%1").arg(text));
        const bool usageExceeded = text.contains(QStringLiteral("使用量超出"));
        const bool usageShort = text.contains(QStringLiteral("使用量少"));
        const bool usageSuccess = text.contains(QStringLiteral("使用量核对成功"));
        if (usageExceeded || usageShort || usageSuccess)
        {
            const QString color = usageExceeded ? QStringLiteral("#dc2626")
                : usageShort ? QStringLiteral("#d97706") : QStringLiteral("#16a34a");
            const QString background = usageExceeded ? QStringLiteral("#fee2e2")
                : usageShort ? QStringLiteral("#ffedd5") : QStringLiteral("#dcfce7");
            executionStatusLabel_->setStyleSheet(QStringLiteral("padding:7px 9px;border-radius:7px;background:%1;color:%2;font-weight:700;")
                .arg(background, color));
        }
    });
    connect(&controller_, &ClientController::countdownChanged, this, [this](const QString& text) {
        executionStatusLabel_->setText(QStringLiteral("服务端倒计时：%1").arg(text));
    });
    connect(&controller_, &ClientController::clientRoleChanged, this, [this](ClientRole role) {
        const QString roleText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(role));
        clientRoleLabel_->setText(QStringLiteral("由服务端下发：%1").arg(roleText));
        clickStrategyLabel_->setText(QStringLiteral("%1固定坐标 · 已适配").arg(roleText));
    });
    connect(&controller_, &ClientController::connectionLogAppended, this, &MainWindow::appendConnectionLog);
    connect(&controller_, &ClientController::serverTimeChanged, this, [this](const QString& text) {
        if (serverTimeLabel_)
            serverTimeLabel_->setText(QStringLiteral("服务器时间：%1").arg(text));
    });
    connect(&controller_, &ClientController::ocrParametersChanged, this, [this](double unitPrice, int bulletTotal) {
        unitPriceEdit_->setText(QString::number(unitPrice, 'g', 17));
        bulletTotalEdit_->setText(QString::number(bulletTotal));
        configuredUnitPriceLabel_->setText(QString::number(unitPrice, 'g', 17));
        ocrDecisionLabel_->setText(QStringLiteral("本机参数已保存；等待 OCR 识别结果"));
        ocrDecisionLabel_->setStyleSheet(QStringLiteral("padding:7px 9px;border-radius:7px;background:#dcfce7;color:#166534;font-weight:700;"));
    });
    connect(&controller_, &ClientController::bulletOcrRunningChanged, this, [this](bool running) {
        executionStatusLabel_->setText(running
            ? QStringLiteral("本机 OCR：运行中，再按 F3 可停止")
            : QStringLiteral("本机 OCR：已停止"));
        if (running)
        {
            ocrDecisionLabel_->setText(QStringLiteral("F3 OCR 识别中，再按 F3 停止"));
            ocrDecisionLabel_->setStyleSheet(QStringLiteral("padding:7px 9px;border-radius:7px;background:#dbeafe;color:#1d4ed8;font-weight:700;"));
        }
    });
    connect(&controller_, &ClientController::bulletOcrResultChanged, this,
        [this](const QString&, double recognizedPrice, double averagePrice, bool meetsUnitPrice) {
            ocrValueLabel_->setText(QString::number(recognizedPrice, 'f', 2));
            unitValueLabel_->setText(QString::number(averagePrice, 'f', 4));
            ocrDecisionLabel_->setText(meetsUnitPrice
                ? QStringLiteral("满足：OCR 结果 ÷ 子弹总数 <= 设定单价")
                : QStringLiteral("未满足：价格偏高，正在执行重试流程"));
            ocrDecisionLabel_->setStyleSheet(meetsUnitPrice
                ? QStringLiteral("padding:7px 9px;border-radius:7px;background:#dcfce7;color:#166534;font-weight:700;")
                : QStringLiteral("padding:7px 9px;border-radius:7px;background:#fef3c7;color:#92400e;font-weight:700;"));
        });
    connect(&controller_, &ClientController::bulletOcrLogAppended, this, [this](const QString& text) {
        appendConnectionLog(QStringLiteral("[F3 OCR] %1").arg(text));
    });
    connect(&controller_, &ClientController::messageRequested, this, &MainWindow::showControllerMessage);
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, #windowRoot { background:#edf5f3; color:#183231; font-family:"Microsoft YaHei UI"; font-size:12px; }
        #framelessTitleBar { background:#f8fafc; border-bottom:1px solid #dce4e2; }
        #windowTitleText { color:#172b2a; font-size:12px; font-weight:700; }
        #windowMinimizeDot, #windowMaximizeDot, #windowCloseDot { border:0; border-radius:4px; min-width:9px; min-height:9px; max-width:9px; max-height:9px; padding:0; }
        #windowMinimizeDot { background:#f4c84a; }
        #windowMaximizeDot { background:#56c779; }
        #windowCloseDot { background:#ef6a6a; }
        #appHeader { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #113f3e,stop:1 #16817b); }
        #appTitle { color:white; font-size:19px; font-weight:800; }
        #appSubtitle { color:#d5f5ef; font-size:12px; }
        #body { background:#edf5f3; }
        #bodyScrollArea { background:#edf5f3; border:0; }
        #bodyScrollArea > QWidget > QWidget { background:#edf5f3; }
        #card { background:#ffffff; border:1px solid #d9e7e3; border-radius:11px; }
        #cardTitle { color:#163b38; font-size:14px; font-weight:800; }
        #fieldLabel, #hintText { color:#738784; font-size:11px; }
        QLineEdit { min-height:29px; border:1px solid #cbdcd8; border-radius:7px; padding:0 9px; background:#f8fcfb; color:#173936; }
        QLineEdit:focus { border-color:#2f9b91; background:white; }
        QPushButton { min-height:29px; border:1px solid #c5d7d3; border-radius:7px; background:#ffffff; color:#31514e; padding:0 10px; font-weight:700; }
        QPushButton:hover { background:#e9f8f5; border-color:#75b9b1; }
        QPushButton:disabled { color:#9aaba8; background:#edf2f1; border-color:#d8e1df; }
        #connectButton { background:#16817b; color:white; border-color:#16817b; }
        #connectButton:hover { background:#116b66; }
        #metricPanel { background:#f0f8f6; border:1px solid #d8e9e5; border-radius:8px; }
        #metricValue { color:#164e49; font-size:16px; font-weight:800; }
        #metricCaption { color:#718682; font-size:10px; }
        #decisionLabel { padding:7px 9px; border-radius:7px; background:#eef4f3; color:#536965; font-weight:700; }
        #executionPanel { background:#f4f8f7; border:1px solid #dfe9e7; border-radius:8px; }
        #executionCaption { color:#748682; font-size:10px; font-weight:700; }
        #executionValue { color:#244944; font-weight:700; }
        #executionStatus { padding:7px 9px; border-radius:7px; background:#e3f3f0; color:#176c64; font-weight:700; }
        #connectionDetail { color:#16817b; font-weight:700; }
        #connectionLog { min-height:62px; background:#0f172a; color:#a7f3d0; border:1px solid #1f2b45; border-radius:9px; padding:8px; font-family:"Consolas","Microsoft YaHei UI"; }
        #cloudSessionList { background:#f8fcfb; border:1px solid #cbdcd8; border-radius:7px; color:#244944; }
        #cloudWindowList { background:#ffffff; border:1px solid #cbdcd8; border-radius:7px; color:#244944; padding:2px; }
        #cloudWindowList::item { min-height:26px; padding:3px 7px; }
        #cloudWindowList::item:selected { background:#d8f1ed; color:#124f4a; }
    )"));
}

void MainWindow::addCloudBossSession()
{
    if (!bindingModeCombo_ || bindingModeCombo_->currentIndex() != 1)
    {
        showControllerMessage(QStringLiteral("模式错误"), QStringLiteral("请先切换到手机模式"), false);
        return;
    }
    bool ok = false;
    const int adbPort = cloudAdbPortEdit_->text().trimmed().toInt(&ok);
    if (!ok || adbPort < 1 || adbPort > 65535)
    {
        showControllerMessage(QStringLiteral("端口错误"), QStringLiteral("请输入有效的 ADB 端口"), true);
        return;
    }
    QString adbDetail;
    const AdbPortCheckResult adbResult = checkAdbPort(adbPort, adbDetail);
    if (adbResult != AdbPortCheckResult::Online)
    {
        QString title;
        switch (adbResult)
        {
        case AdbPortCheckResult::ConnectFailed:
            title = QStringLiteral("ADB connect 失败");
            break;
        case AdbPortCheckResult::DeviceOffline:
            title = QStringLiteral("设备未上线");
            break;
        case AdbPortCheckResult::ValidationFailed:
        default:
            title = QStringLiteral("ADB 设备校验失败");
            break;
        }
        showControllerMessage(title,
            QStringLiteral("端口 %1 未创建客户端会话。\n\n%2")
                .arg(adbPort).arg(adbDetail), true);
        return;
    }
    for (int index = 0; index < cloudSessions_.size(); ++index)
    {
        if (cloudSessions_[index].adbPort == adbPort)
        {
            if (!cloudSessions_[index].kicked && cloudSessions_[index].connectedOnce)
            {
                showControllerMessage(QStringLiteral("设备已存在"), QStringLiteral("该 ADB 端口已经添加"), false);
                return;
            }
            // 连接服务端失败时，旧会话已经创建但从未成功上线；删除残留行，允许用新端口重试。
            removeCloudBossSession(index);
            break;
        }
    }
    const int serverPort = portEdit_->text().trimmed().toInt(&ok);
    if (!ok || ipEdit_->text().trimmed().isEmpty())
    {
        showControllerMessage(QStringLiteral("连接参数错误"), QStringLiteral("请先填写有效的服务端 IP 和端口"), true);
        return;
    }

    auto* controller = new ClientController(this);
    controller->initializeExternalBossSession(reinterpret_cast<HWND>(winId()));
    controller->setSessionDeviceId(QStringLiteral("ADB-%1").arg(adbPort));
    controller->setSessionAdbPort(adbPort);
    auto* item = new QListWidgetItem(QStringLiteral("ADB:%1 · 正在绑定 VM 窗口 · 未连接").arg(adbPort), cloudSessionList_);
    cloudSessions_.append({ controller, adbPort, item, false, false, false });
    const QString serverIp = ipEdit_->text().trimmed();
    connect(controller, &ClientController::statusChanged, this,
        [this, controller, item, adbPort, serverIp, serverPort](const QString& text) {
        auto sessionIt = std::find_if(cloudSessions_.begin(), cloudSessions_.end(), [controller](const CloudBossSession& session) {
            return session.controller == controller;
        });
        if (sessionIt == cloudSessions_.end())
            return;
        if (item)
            item->setText(QStringLiteral("ADB:%1 · %2").arg(adbPort).arg(text));
        if (text.contains(QStringLiteral("已被服务端踢出")))
            sessionIt->kicked = true;

        const bool initialConnectionFailure = !sessionIt->connectedOnce
            && (text == QStringLiteral("连接服务端失败")
                || text == QStringLiteral("服务端 IP 格式无效")
                || text.contains(QStringLiteral("连接建立后发送"))
                || text.contains(QStringLiteral("连接后发送默认准备状态")));
        if (initialConnectionFailure)
        {
            if (!sessionIt->connectionFailureShown)
            {
                sessionIt->connectionFailureShown = true;
                showControllerMessage(QStringLiteral("连接服务端失败"),
                    QStringLiteral("无法连接服务端 %1:%2，未创建客户端会话。\n\n%3")
                        .arg(serverIp).arg(serverPort).arg(text), true);
            }
            // Initial connection failures must not leave a reusable ADB port
            // occupied by a controller that never reached the server.
            QTimer::singleShot(0, this, [this, controller]() {
                const auto failedIt = std::find_if(cloudSessions_.begin(), cloudSessions_.end(),
                    [controller](const CloudBossSession& session) {
                        return session.controller == controller;
                    });
                if (failedIt == cloudSessions_.end() || failedIt->connectedOnce
                    || failedIt->kicked || failedIt->controller->isConnected()
                    || failedIt->controller->isReconnecting())
                {
                    return;
                }
                removeCloudBossSession(static_cast<int>(std::distance(cloudSessions_.begin(), failedIt)));
            });
        }
    });
    connect(controller, &ClientController::connectionLogAppended, this,
        [this, adbPort](const QString& text) {
        appendConnectionLog(QStringLiteral("[ADB:%1] %2").arg(adbPort).arg(text));
    });
    connect(controller, &ClientController::connectedChanged, this, [this, controller, item, adbPort](bool connected) {
        auto sessionIt = std::find_if(cloudSessions_.begin(), cloudSessions_.end(), [controller](const CloudBossSession& session) {
            return session.controller == controller;
        });
        if (sessionIt == cloudSessions_.end())
            return;
        if (connected)
            sessionIt->connectedOnce = true;
        if (item)
            item->setText(QStringLiteral("ADB:%1 · %2").arg(adbPort).arg(connected ? QStringLiteral("已连接") : QStringLiteral("未连接")));
    });
    bool bound = false;
    const QListWidgetItem* selectedWindow = cloudWindowList_->currentItem();
    if (!selectedWindow)
    {
        item->setText(QStringLiteral("ADB:%1 · 未找到 VM 窗口").arg(adbPort));
    }
    else
    {
        const quintptr hwndValue = selectedWindow->data(Qt::UserRole).value<quintptr>();
        const QString windowName = selectedWindow->text();
        bound = controller->bindExternalWindow(reinterpret_cast<HWND>(hwndValue), windowName);
        if (!bound)
            item->setText(QStringLiteral("ADB:%1 · VM 窗口绑定失败").arg(adbPort));
    }
    if (bound)
        controller->connectOrDisconnect(ipEdit_->text().trimmed(), serverPort);
    else
        item->setText(QStringLiteral("ADB:%1 · 等待选择有效 VM 窗口").arg(adbPort));
    cloudAdbPortEdit_->setText(QString::number(adbPort + 1));
}

void MainWindow::removeCloudBossSession(int index)
{
    if (index < 0 || index >= cloudSessions_.size())
        return;

    const CloudBossSession session = cloudSessions_.takeAt(index);
    if (session.controller)
    {
        disconnect(session.controller, nullptr, this, nullptr);
        session.controller->shutdownForExit();
        if (!session.controller->hasAdbWorker())
            delete session.controller;
    }
    if (session.item)
    {
        const int row = cloudSessionList_->row(session.item);
        if (row >= 0)
            delete cloudSessionList_->takeItem(row);
    }
}

void MainWindow::clearCloudBossSessions()
{
    while (!cloudSessions_.isEmpty())
        removeCloudBossSession(cloudSessions_.size() - 1);
}

void MainWindow::applyClientBindingMode(int modeIndex)
{
    const bool phoneMode = modeIndex == 1;
    connectionLogEdit_->document()->setMaximumBlockCount(phoneMode ? 600 : 0);
    if (cloudSessionCard_)
        cloudSessionCard_->setVisible(phoneMode);
    if (pcOcrCard_)
        pcOcrCard_->setVisible(!phoneMode);
    if (pcExecutionCard_)
        pcExecutionCard_->setVisible(!phoneMode);

    if (phoneMode)
    {
        if (controller_.isConnected() || controller_.isReconnecting())
            controller_.connectOrDisconnect(ipEdit_->text().trimmed(), portEdit_->text().toInt());
        controller_.forceStopCurrentAction();
        refreshCloudWindowOptions();
    }
    else
    {
        clearCloudBossSessions();
        controller_.rebindGameWindow();
    }
    refreshConnectionControls();
}

MainWindow::AdbPortCheckResult MainWindow::checkAdbPort(int adbPort, QString& detail) const
{
    const QString adbPath = controller_.loadAdbPath(&detail);
    if (adbPath.isEmpty()) return AdbPortCheckResult::ConnectFailed;
    const QString expectedSerial = QStringLiteral("127.0.0.1:%1").arg(adbPort);

    QProcess connectProcess;
    connectProcess.start(adbPath, { QStringLiteral("connect"), expectedSerial });
    if (!connectProcess.waitForFinished(4000))
    {
        detail = QStringLiteral("无法在 4 秒内执行 adb connect %1，请确认 adb.exe 已可用。")
            .arg(expectedSerial);
        return AdbPortCheckResult::ConnectFailed;
    }
    const QString connectOutput = QString::fromUtf8(connectProcess.readAllStandardOutput()).trimmed();
    const QString connectError = QString::fromUtf8(connectProcess.readAllStandardError()).trimmed();
    const QRegularExpression connectFailurePattern(
        QStringLiteral("failed to connect|unable to connect|cannot connect|could not connect|connection refused|connection timed out|no route to host"),
        QRegularExpression::CaseInsensitiveOption);
    if (connectProcess.exitCode() != 0 || connectFailurePattern.match(connectOutput).hasMatch()
        || connectFailurePattern.match(connectError).hasMatch())
    {
        detail = connectError.isEmpty()
            ? (connectOutput.isEmpty() ? QStringLiteral("adb connect 执行失败。") : connectOutput)
            : connectError;
        return AdbPortCheckResult::ConnectFailed;
    }

    QString latestOutput;
    QString latestError;
    bool devicesCommandFailed = false;
    // adb connect may return before the new endpoint appears in adb devices.
    // Give the adb server a short window to publish the endpoint, while
    // keeping the add-session action bounded and synchronous.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        QProcess process;
        process.start(adbPath, { QStringLiteral("devices") });
        if (!process.waitForFinished(1000))
        {
            devicesCommandFailed = true;
            latestError = QStringLiteral("无法在 1 秒内完成 adb devices 校验。");
        }
        else
        {
            latestOutput = QString::fromUtf8(process.readAllStandardOutput());
            latestError = QString::fromUtf8(process.readAllStandardError()).trimmed();
            if (process.exitCode() != 0)
            {
                devicesCommandFailed = true;
            }
            else
            {
                const QStringList lines = latestOutput.split(
                    QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
                for (const QString& rawLine : lines)
                {
                    const QString line = rawLine.trimmed();
                    const QStringList fields = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    if (fields.size() >= 2
                        && fields.at(0) == expectedSerial
                        && fields.at(1) == QStringLiteral("device"))
                    {
                        detail = QStringLiteral("已确认在线设备：%1").arg(expectedSerial);
                        return AdbPortCheckResult::Online;
                    }
                }
                devicesCommandFailed = false;
            }
        }
        if (attempt < 4)
            QThread::msleep(250);
    }

    if (devicesCommandFailed)
    {
        detail = latestError.isEmpty() ? QStringLiteral("adb devices 执行失败。") : latestError;
        return AdbPortCheckResult::ValidationFailed;
    }
    detail = QStringLiteral("adb connect 已执行，但在 adb devices 中未找到状态为 device 的设备 %1。当前列表：%2")
        .arg(expectedSerial, latestOutput.trimmed().isEmpty() ? QStringLiteral("空") : latestOutput.trimmed());
    return AdbPortCheckResult::DeviceOffline;
}

void MainWindow::refreshCloudWindowOptions()
{
    cloudWindowList_->clear();
    controller_.refreshWindows();
    const QVector<TargetWindowInfo> candidates = controller_.windows();
    bool hasSurface = false;
    for (const TargetWindowInfo& window : candidates)
    {
        if (window.isCloudPhoneSurface && window.hwnd)
        {
            hasSurface = true;
            break;
        }
    }
    for (const TargetWindowInfo& window : candidates)
    {
        if (!window.isCloudPhone || !window.hwnd || (hasSurface && !window.isCloudPhoneSurface))
            continue;
        const QString text = QStringLiteral("%1 · %2×%3 · HWND %4")
            .arg(window.title.isEmpty() ? QStringLiteral("红手指窗口") : window.title)
            .arg(window.clientWidth)
            .arg(window.clientHeight)
            .arg(window.hwndText);
        auto* item = new QListWidgetItem(text, cloudWindowList_);
        item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(window.hwnd)));
    }
    if (cloudWindowList_->count() == 0)
    {
        auto* item = new QListWidgetItem(QStringLiteral("未检测到 VM 云手机画面窗口，请确认红手指已打开"), cloudWindowList_);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    else
    {
        cloudWindowList_->setCurrentRow(0);
    }
}

void MainWindow::saveOcrParameters()
{
    if (!unitPriceEdit_->hasAcceptableInput() || !bulletTotalEdit_->hasAcceptableInput())
    {
        ocrDecisionLabel_->setText(QStringLiteral("参数无效：单价不能为负数，子弹总数必须至少为 1"));
        ocrDecisionLabel_->setStyleSheet(QStringLiteral("padding:7px 9px;border-radius:7px;background:#fee2e2;color:#991b1b;font-weight:700;"));
        return;
    }

    bool priceOk = false;
    bool totalOk = false;
    const double unitPrice = unitPriceEdit_->text().toDouble(&priceOk);
    const int bulletTotal = bulletTotalEdit_->text().toInt(&totalOk);
    if (!priceOk || !totalOk || unitPrice < 0.0 || bulletTotal < 1)
    {
        ocrDecisionLabel_->setText(QStringLiteral("参数无效，未覆盖已保存配置"));
        ocrDecisionLabel_->setStyleSheet(QStringLiteral("padding:7px 9px;border-radius:7px;background:#fee2e2;color:#991b1b;font-weight:700;"));
        return;
    }
    controller_.setOcrParameters(unitPrice, bulletTotal);
}

void MainWindow::refreshConnectionControls()
{
    const bool phoneMode = bindingModeCombo_ && bindingModeCombo_->currentIndex() == 1;
    const bool connected = controller_.isConnected();
    const bool reconnecting = controller_.isReconnecting();
    connectButton_->setEnabled(!phoneMode && !connected && !reconnecting);
    disconnectButton_->setEnabled(!phoneMode && (connected || reconnecting));
    ipEdit_->setEnabled(phoneMode || (!connected && !reconnecting));
    portEdit_->setEnabled(phoneMode || (!connected && !reconnecting));

    if (phoneMode)
    {
        connectionBadge_->setText(QStringLiteral("手机模式"));
        connectionDetailLabel_->setText(QStringLiteral("各 ADB 会话分别连接"));
    }
    else if (connected)
    {
        connectionBadge_->setText(QStringLiteral("已连接"));
        connectionDetailLabel_->setText(QStringLiteral("连接正常"));
    }
    else if (reconnecting)
    {
        connectionBadge_->setText(QStringLiteral("重连中"));
        connectionDetailLabel_->setText(QStringLiteral("正在自动重连"));
    }
    else
    {
        connectionBadge_->setText(QStringLiteral("未连接"));
        connectionDetailLabel_->setText(QStringLiteral("等待连接"));
    }
}

void MainWindow::updateConnectedState(bool connected)
{
    Q_UNUSED(connected);
    refreshConnectionControls();
}

void MainWindow::updateReconnectingState(bool reconnecting)
{
    Q_UNUSED(reconnecting);
    refreshConnectionControls();
}

void MainWindow::appendConnectionLog(const QString& text)
{
    // 仅 ADB 老板流程使用批量刷新，保持 PC/币号流程原有日志时序。
    if (!text.startsWith(QStringLiteral("[ADB:")))
    {
        flushConnectionLog();
        connectionLogEdit_->append(text);
        return;
    }
    // ADB OCR can produce results faster than a rich-text control can reflow.
    // Coalesce updates and retain only a bounded recent history.
    pendingConnectionLogs_.append(text);
    constexpr int MaxPendingLines = 160;
    if (pendingConnectionLogs_.size() > MaxPendingLines)
        pendingConnectionLogs_.erase(pendingConnectionLogs_.begin(),
            pendingConnectionLogs_.begin() + (pendingConnectionLogs_.size() - MaxPendingLines));
    if (!connectionLogFlushTimer_.isActive())
        connectionLogFlushTimer_.start();
}

void MainWindow::flushConnectionLog()
{
    if (!connectionLogEdit_ || pendingConnectionLogs_.isEmpty())
        return;
    const bool followTail = connectionLogEdit_->verticalScrollBar()->value()
        >= connectionLogEdit_->verticalScrollBar()->maximum() - 2;
    QTextCursor cursor(connectionLogEdit_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(pendingConnectionLogs_.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    pendingConnectionLogs_.clear();
    if (followTail)
        connectionLogEdit_->verticalScrollBar()->setValue(connectionLogEdit_->verticalScrollBar()->maximum());
}

void MainWindow::showControllerMessage(const QString& title, const QString& text, bool error)
{
    if (error)
    {
        QMessageBox::critical(this, title, text);
    }
    else
    {
        QMessageBox::information(this, title, text);
    }
}
