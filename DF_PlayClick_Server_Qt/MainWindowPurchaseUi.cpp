#include "MainWindow.h"

#include <algorithm>
#include <QApplication>
#include <QCloseEvent>
#include <QButtonGroup>
#include <QDoubleValidator>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWheelEvent>
#include "../Shared/FramelessTitleBar.h"

namespace
{
class ClickOnlyComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;
protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

QWidget* field(const QString& text, QWidget* control, QWidget* parent)
{
    auto* widget = new QWidget(parent);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    auto* label = new QLabel(text, widget);
    label->setObjectName(QStringLiteral("fieldLabel"));
    label->setBuddy(control);
    label->setFixedHeight(21);
    layout->addWidget(label);
    layout->addWidget(control);
    return widget;
}

void configureTable(QTableWidget* table)
{
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(65);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    table->setShowGrid(false);
    table->setAlternatingRowColors(false);
    table->horizontalHeader()->setFixedHeight(29);
}

void addSectionHint(QWidget* card, const QString& text, const QString& name = QStringLiteral("sectionHint"))
{
    auto* layout = qobject_cast<QVBoxLayout*>(card->layout());
    auto* titleItem = layout->takeAt(0);
    auto* title = titleItem->widget();
    delete titleItem;
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(title);
    header->addStretch();
    auto* hint = new QLabel(text, card);
    hint->setObjectName(name);
    header->addWidget(hint);
    layout->insertLayout(0, header);
}
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("DF 倒子弹服务端 · PCsoftf"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_StyledBackground, true);
    resize(1024, qMin(1196, QApplication::primaryScreen()->availableGeometry().height() - 40));
    setMinimumSize(940, 700);
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("windowRoot"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    auto* titleBar = new FramelessTitleBar(QStringLiteral("DF 倒子弹服务端 · PCsoftf"), this, central);
    titleBar->setFixedHeight(55);
    titleBar->layout()->setContentsMargins(20, 0, 0, 0);
    root->addWidget(titleBar);

    auto* body = new QWidget(central);
    body->setObjectName(QStringLiteral("body"));
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    auto* sidebar = new QWidget(body);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(155);
    auto* navigation = new QVBoxLayout(sidebar);
    navigation->setContentsMargins(12, 18, 12, 18);
    navigation->setSpacing(4);
    auto* pageStack = new QStackedWidget(body);
    auto* overviewPage = new QWidget(pageStack);
    auto* clientsPage = new QWidget(pageStack);
    auto* loadoutPage = new QWidget(pageStack);
    auto* accountsPage = new QWidget(pageStack);
    auto* logsPage = new QWidget(pageStack);
    for (auto* page : {overviewPage, clientsPage, loadoutPage, accountsPage, logsPage}) pageStack->addWidget(page);
    auto* navGroup = new QButtonGroup(sidebar);
    navGroup->setExclusive(true);
    const QStringList navNames = {QStringLiteral("总览与执行"), QStringLiteral("客户端列表"),
        QStringLiteral("配装与上架"), QStringLiteral("账号管理"), QStringLiteral("日志与状态")};
    for (int index = 0; index < navNames.size(); ++index)
    {
        auto* button = new QPushButton(navNames[index], sidebar);
        button->setObjectName(QStringLiteral("pageNavButton"));
        button->setCheckable(true);
        navGroup->addButton(button, index);
        navigation->addWidget(button);
        connect(button, &QPushButton::clicked, pageStack, [pageStack, index]() { pageStack->setCurrentIndex(index); });
        button->setChecked(index == 0);
    }
    navigation->addStretch();

    countdownEdit_ = new QLineEdit(QStringLiteral("5"), this);
    countdownEdit_->setObjectName(QStringLiteral("countdownEdit"));
    countdownEdit_->setValidator(new QIntValidator(0, 86400, countdownEdit_));
    groupDelayEdit_ = new QLineEdit(QStringLiteral("100"), this);
    groupDelayEdit_->setValidator(new QIntValidator(0, 600000, groupDelayEdit_));
    compensationLeadEdit_ = new QLineEdit(QStringLiteral("1500"), this);
    compensationLeadEdit_->setValidator(new QIntValidator(0, 600000, compensationLeadEdit_));
    triggerAllButton_ = new QPushButton(QStringLiteral("执行"), this);
    triggerAllButton_->setAccessibleName(QStringLiteral("全部出发"));
    listingButton_ = new QPushButton(QStringLiteral("上架"), this);
    forceStopAllButton_ = new QPushButton(QStringLiteral("停止所有动作"), this);
    settingsButton_ = new QPushButton(QStringLiteral("设置"), this);
    saveLoadoutButton_ = new QPushButton(QStringLiteral("保存配装"), this);
    rebindAllButton_ = new QPushButton(QStringLiteral("全部重新绑定"), this);
    mailAllButton_ = new QPushButton(QStringLiteral("全部领取邮件"), this);
    bossPriceEdit_ = new QLineEdit(this);
    bossPriceEdit_->setPlaceholderText(QStringLiteral("执行时下发给老板客户端的价格"));
    auto* bossValidator = new QDoubleValidator(0.01, 999999999.0, 4, bossPriceEdit_);
    bossValidator->setNotation(QDoubleValidator::StandardNotation);
    bossPriceEdit_->setValidator(bossValidator);
    accountSwitchAmountEdit_ = new QLineEdit(accountSwitchAmount_, this);
    accountSwitchAmountEdit_->setPlaceholderText(QStringLiteral("空 / 0 关闭"));
    highPriceStopThresholdSpin_ = new QSpinBox(this);
    highPriceStopThresholdSpin_->setRange(1, 99);
    highPriceStopThresholdSpin_->setValue(3);
    autoExecuteCheck_ = new QCheckBox(QStringLiteral("全自动执行"), this);
    autoBossThresholdSpin_ = new QSpinBox(this);
    autoCoinThresholdSpin_ = new QSpinBox(this);
    for (auto* spin : {autoBossThresholdSpin_, autoCoinThresholdSpin_})
    { spin->setRange(1, 99); spin->setValue(1); spin->setFixedWidth(68); spin->setObjectName(QStringLiteral("inlineSpin")); }
    rebindAllButton_->setObjectName(QStringLiteral("inlineButton"));
    mailAllButton_->setObjectName(QStringLiteral("inlineButton"));

    auto* overviewLayout = new QVBoxLayout(overviewPage);
    overviewLayout->setContentsMargins(0, 0, 0, 0);
    overviewLayout->setSpacing(16);
    auto* overviewScroll = new QScrollArea(overviewPage);
    overviewScroll->setObjectName(QStringLiteral("overviewScroll"));
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setFrameShape(QFrame::NoFrame);
    auto* overviewContent = new QWidget;
    overviewContent->setObjectName(QStringLiteral("overviewContent"));
    auto* overviewContentLayout = new QVBoxLayout(overviewContent);
    overviewContentLayout->setContentsMargins(0, 0, 0, 0);
    overviewContentLayout->setSpacing(16);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto* heading = new QLabel(QStringLiteral("总览与执行"), overviewContent);
    heading->setObjectName(QStringLiteral("pageHeading"));
    actions->addWidget(heading);
    actions->addStretch();
    actions->addWidget(triggerAllButton_);
    actions->addWidget(listingButton_);
    actions->addWidget(forceStopAllButton_);
    overviewContentLayout->addLayout(actions);

    purchaseDraft_ = controller_.auctionPurchase().config();
    purchaseModeCombo_ = new ClickOnlyComboBox(this);
    purchaseModeCombo_->setObjectName(QStringLiteral("purchaseMode"));
    purchaseSlotCombo_ = new ClickOnlyComboBox(this);
    purchaseSlotCombo_->setObjectName(QStringLiteral("purchaseSlot"));
    for (int slot = 1; slot <= 5; ++slot) purchaseSlotCombo_->addItem(QStringLiteral("购买第 %1 款").arg(slot), slot);
    purchaseAmmoCombo_ = new ClickOnlyComboBox(this);
    purchaseAmmoCombo_->setObjectName(QStringLiteral("purchaseAmmo"));
    purchaseAmmoCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    purchaseAmmoCombo_->setMinimumContentsLength(10);
    purchaseThresholdEdit_ = new QLineEdit(this);
    purchaseThresholdEdit_->setObjectName(QStringLiteral("purchaseThreshold"));
    purchaseThresholdEdit_->setPlaceholderText(QStringLiteral("输入购买阈值"));
    purchaseThresholdEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,15}(?:\\.\\d{0,4})?")), purchaseThresholdEdit_));
    auto* purchaseFields = new QVBoxLayout;
    purchaseFields->setSpacing(0);
    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(14);
    modeRow->addWidget(field(QStringLiteral("购买方式"), purchaseModeCombo_, this), 17);
    auto* modeHint = new QLabel(QStringLiteral("可随时选回原购买方式\n执行中等本轮结束后统一应用"), this);
    modeHint->setObjectName(QStringLiteral("sectionHint"));
    modeRow->addWidget(modeHint, 10, Qt::AlignBottom);
    purchaseFields->addLayout(modeRow);
    purchaseFields->addSpacing(14);
    auto* itemRow = new QHBoxLayout;
    itemRow->setSpacing(14);
    itemRow->addWidget(field(QStringLiteral("购买款式"), purchaseSlotCombo_, this), 100);
    itemRow->addWidget(field(QStringLiteral("子弹名称"), purchaseAmmoCombo_, this), 165);
    auto* thresholdBox = new QWidget(this);
    auto* thresholdLayout = new QVBoxLayout(thresholdBox);
    thresholdLayout->setContentsMargins(0, 0, 0, 0);
    thresholdLayout->setSpacing(7);
    purchaseThresholdLabel_ = new QLabel(this);
    purchaseThresholdLabel_->setObjectName(QStringLiteral("fieldLabel"));
    purchaseThresholdLabel_->setBuddy(purchaseThresholdEdit_);
    purchaseThresholdLabel_->setFixedHeight(21);
    thresholdLayout->addWidget(purchaseThresholdLabel_);
    thresholdLayout->addWidget(purchaseThresholdEdit_);
    itemRow->addWidget(thresholdBox, 120);
    purchaseFields->addLayout(itemRow);
    purchaseFields->addSpacing(13);
    auto* memoryRow = new QHBoxLayout;
    purchaseMemoryLabel_ = new QLabel(this);
    purchaseMemoryLabel_->setObjectName(QStringLiteral("sectionHint"));
    purchaseMemoryLabel_->setFixedHeight(18);
    purchaseStatusLabel_ = new QLabel(this);
    purchaseStatusLabel_->setObjectName(QStringLiteral("sectionHint"));
    memoryRow->addWidget(purchaseMemoryLabel_);
    memoryRow->addStretch();
    memoryRow->addWidget(purchaseStatusLabel_);
    purchaseFields->addLayout(memoryRow);
    purchaseFields->addSpacing(12);
    purchaseWarningLabel_ = new QLabel(this);
    purchaseWarningLabel_->setObjectName(QStringLiteral("purchaseWarning"));
    purchaseWarningLabel_->setFixedHeight(31);
    purchaseWarningLabel_->setWordWrap(true);
    purchaseFields->addWidget(purchaseWarningLabel_);
    auto* purchaseCard = createCard(QStringLiteral("购买参数"), purchaseFields);
    purchaseCard->setProperty("uiSection", QStringLiteral("purchase"));
    addSectionHint(purchaseCard, QStringLiteral("统一同步至绑定 Qt 的币号与老板"), QStringLiteral("purchaseScope"));
    overviewContentLayout->addWidget(purchaseCard);

    auto* execution = new QVBoxLayout;
    execution->setSpacing(14);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);
    grid->addWidget(field(QStringLiteral("倒计时 / 秒"), countdownEdit_, this), 0, 0);
    grid->addWidget(field(QStringLiteral("组间延迟 / ms"), groupDelayEdit_, this), 0, 1);
    grid->addWidget(field(QStringLiteral("补偿提前量 / ms"), compensationLeadEdit_, this), 0, 2);
    grid->addWidget(field(QStringLiteral("老板上架价格"), bossPriceEdit_, this), 1, 0);
    grid->addWidget(field(QStringLiteral("换号金额"), accountSwitchAmountEdit_, this), 1, 1);
    grid->addWidget(field(QStringLiteral("连续高价停止次数"), highPriceStopThresholdSpin_, this), 1, 2);
    for (int column = 0; column < 3; ++column) grid->setColumnStretch(column, 1);
    execution->addLayout(grid);
    auto* options = new QHBoxLayout;
    options->setSpacing(10);
    options->addWidget(autoExecuteCheck_);
    const auto addInlineNumber = [this, options](const QString& text, QSpinBox* spin) {
        auto* group = new QWidget(this);
        auto* row = new QHBoxLayout(group);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        row->addWidget(new QLabel(text, group));
        row->addWidget(spin);
        options->addWidget(group);
    };
    addInlineNumber(QStringLiteral("自动执行老板数"), autoBossThresholdSpin_);
    addInlineNumber(QStringLiteral("自动执行币号数"), autoCoinThresholdSpin_);
    options->addWidget(rebindAllButton_);
    options->addWidget(mailAllButton_);
    options->addStretch();
    execution->addLayout(options);
    auto* executionCard = createCard(QStringLiteral("执行参数"), execution);
    executionCard->setProperty("uiSection", QStringLiteral("execution"));
    addSectionHint(executionCard, QStringLiteral("现有参数与操作完整保留"));
    overviewContentLayout->addWidget(executionCard);

    clientTable_ = new QTableWidget(0, 5, this);
    clientTable_->setObjectName(QStringLiteral("purchaseClientTable"));
    clientTable_->setHorizontalHeaderLabels({QStringLiteral("客户端"), QStringLiteral("角色 / 绑定"), QStringLiteral("款式与子弹"), QStringLiteral("参数同步"), QStringLiteral("执行回报")});
    configureTable(clientTable_);
    for (int column = 0; column < 4; ++column) clientTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
    const auto resizeOverviewColumns = [this]() {
        const int width = clientTable_->viewport()->width();
        const double ratios[] = {0.15, 0.15, 0.25, 0.20};
        for (int column = 0; column < 4; ++column) clientTable_->setColumnWidth(column, qRound(width * ratios[column]));
    };
    connect(clientTable_->horizontalHeader(), &QHeaderView::geometriesChanged, this, resizeOverviewColumns);
    clientTable_->horizontalHeader()->setFixedHeight(28);
    clientTable_->setMinimumHeight(223);
    clientTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    auto* clientLayout = new QVBoxLayout;
    clientLayout->setContentsMargins(0, 0, 0, 0);
    clientLayout->addWidget(clientTable_);
    auto* clientCard = createCard(QStringLiteral("客户端状态"), clientLayout);
    clientCard->setProperty("uiSection", QStringLiteral("clients"));
    overviewContentLayout->addWidget(clientCard);
    overviewContentLayout->addStretch();
    overviewScroll->setWidget(overviewContent);
    overviewLayout->addWidget(overviewScroll, 1);
    auto* footer = new QWidget(overviewPage);
    footer->setObjectName(QStringLiteral("overviewFooter"));
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(12);
    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(12, 0, 12, 0);
    statusLabel_ = new QLabel(QStringLiteral("等待操作"), footer);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    serverTimeLabel_ = new QLabel(footer);
    serverTimeLabel_->setObjectName(QStringLiteral("serverTimeLabel"));
    statusRow->addWidget(statusLabel_, 1);
    statusRow->addWidget(serverTimeLabel_);
    footerLayout->addLayout(statusRow);
    connectionLogEdit_ = new QTextEdit(footer);
    connectionLogEdit_->setObjectName(QStringLiteral("connectionLog"));
    connectionLogEdit_->setReadOnly(true);
    connectionLogEdit_->document()->setMaximumBlockCount(2000);
    connectionLogEdit_->document()->setDocumentMargin(0);
    auto logPalette = connectionLogEdit_->palette();
    logPalette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#a6c7ca")));
    connectionLogEdit_->setPalette(logPalette);
    connectionLogEdit_->setPlaceholderText(QStringLiteral("监听、连接与调度日志将在这里显示。"));
    connectionLogEdit_->setFixedHeight(110);
    footerLayout->addWidget(connectionLogEdit_);
    overviewLayout->addWidget(footer);

    auto* clientsLayout = new QVBoxLayout(clientsPage);
    clientsLayout->setContentsMargins(0, 0, 0, 0);
    auto* clientsHeading = new QLabel(QStringLiteral("客户端列表"), clientsPage);
    clientsHeading->setObjectName(QStringLiteral("pageHeading"));
    clientsLayout->addWidget(clientsHeading);
    clientListTable_ = new QTableWidget(0, 7, clientsPage);
    clientListTable_->setHorizontalHeaderLabels({QStringLiteral("客户端 IP / ADB端口"), QStringLiteral("角色 / 备注"), QStringLiteral("上架/准备状态"),
        QStringLiteral("期望金额"), QStringLiteral("当前金额"), QStringLiteral("累计收益"), QStringLiteral("低价/高价")});
    configureTable(clientListTable_);
    clientsLayout->addWidget(clientListTable_);

    listingTextEdit_ = new QLineEdit(this);
    listingTextEdit_->setPlaceholderText(QStringLiteral("请输入要粘贴的上架文本"));
    listingPriceEdit_ = new QLineEdit(this);
    listingPriceEdit_->setPlaceholderText(QStringLiteral("请输入价格"));
    auto* priceValidator = new QDoubleValidator(0.01, 999999999.0, 4, listingPriceEdit_);
    priceValidator->setNotation(QDoubleValidator::StandardNotation);
    listingPriceEdit_->setValidator(priceValidator);
    listingPurchaseCountSpin_ = new QSpinBox(this);
    listingPurchaseCountSpin_->setRange(1, 99);
    listingPurchaseCountSpin_->setValue(5);
    auto* loadoutLayout = new QVBoxLayout(loadoutPage);
    loadoutLayout->setContentsMargins(0, 0, 0, 0);
    auto* loadoutActions = new QHBoxLayout;
    auto* loadoutHeading = new QLabel(QStringLiteral("配装与上架"), loadoutPage);
    loadoutHeading->setObjectName(QStringLiteral("pageHeading"));
    loadoutActions->addWidget(loadoutHeading);
    loadoutActions->addStretch();
    loadoutActions->addWidget(saveLoadoutButton_);
    loadoutLayout->addLayout(loadoutActions);
    auto* listingParameters = new QHBoxLayout;
    listingParameters->addWidget(field(QStringLiteral("上架文本"), listingTextEdit_, this), 1);
    listingParameters->addWidget(field(QStringLiteral("上架价格"), listingPriceEdit_, this), 1);
    listingParameters->addWidget(field(QStringLiteral("购买次数"), listingPurchaseCountSpin_, this), 1);
    loadoutLayout->addWidget(createCard(QStringLiteral("上架参数"), listingParameters));
    loadoutLayout->addStretch();

    importAccountsButton_ = new QPushButton(QStringLiteral("导入账号"), this);
    addAccountsButton_ = new QPushButton(QStringLiteral("增加账号"), this);
    clearAccountsButton_ = new QPushButton(QStringLiteral("清空账号"), this);
    exportAccountsButton_ = new QPushButton(QStringLiteral("导出账号"), this);
    accountLoginAllButton_ = new QPushButton(QStringLiteral("一键上号"), this);
    scriptLoginAllButton_ = new QPushButton(QStringLiteral("脚本上号"), this);
    accountQueueLabel_ = new QLabel(QStringLiteral("账号队列：0/0"), this);
    auto* accountsLayout = new QVBoxLayout(accountsPage);
    accountsLayout->setContentsMargins(0, 0, 0, 0);
    auto* accountsHeading = new QLabel(QStringLiteral("账号管理"), accountsPage);
    accountsHeading->setObjectName(QStringLiteral("pageHeading"));
    accountsLayout->addWidget(accountsHeading);
    auto* accountActions = new QHBoxLayout;
    for (auto* button : {importAccountsButton_, addAccountsButton_, clearAccountsButton_, exportAccountsButton_, accountLoginAllButton_, scriptLoginAllButton_}) accountActions->addWidget(button);
    accountActions->addWidget(accountQueueLabel_);
    accountActions->addStretch();
    accountsLayout->addLayout(accountActions);
    accountsLayout->addStretch();

    flowPreviewLabel_ = new QLabel(this);
    flowPreviewLabel_->setObjectName(QStringLiteral("flowPreview"));
    flowPreviewLabel_->setWordWrap(true);
    portEdit_ = new QLineEdit(controller_.defaultPortText(), this);
    portEdit_->setValidator(new QIntValidator(1, 65535, portEdit_));
    listenButton_ = new QPushButton(QStringLiteral("启动监听"), this);
    stopButton_ = new QPushButton(QStringLiteral("停止监听"), this);
    listingQuantityTotalLabel_ = new QLabel(QStringLiteral("上架总数:0"), this);
    listingSuccessClientsLabel_ = new QLabel(QStringLiteral("成功上架客户端:0/0"), this);
    auto* logsLayout = new QVBoxLayout(logsPage);
    logsLayout->setContentsMargins(0, 0, 0, 0);
    auto* logsHeading = new QLabel(QStringLiteral("日志与状态"), logsPage);
    logsHeading->setObjectName(QStringLiteral("pageHeading"));
    logsLayout->addWidget(logsHeading);
    logsLayout->addWidget(createAppHeader());
    auto* listenRow = new QHBoxLayout;
    listenRow->addWidget(portEdit_, 1);
    listenRow->addWidget(listenButton_);
    listenRow->addWidget(stopButton_);
    auto* listenLayout = new QVBoxLayout;
    listenLayout->addLayout(listenRow);
    listenLayout->addWidget(listingQuantityTotalLabel_);
    listenLayout->addWidget(listingSuccessClientsLabel_);
    logsLayout->addWidget(createCard(QStringLiteral("服务器连接"), listenLayout));
    logsLayout->addWidget(settingsButton_, 0, Qt::AlignLeft);
    logsLayout->addWidget(flowPreviewLabel_);
    logsLayout->addStretch();

    bodyLayout->addWidget(sidebar);
    auto* pageArea = new QWidget(body);
    auto* pageAreaLayout = new QVBoxLayout(pageArea);
    pageAreaLayout->setContentsMargins(20, 20, 20, 20);
    pageAreaLayout->addWidget(pageStack);
    bodyLayout->addWidget(pageArea, 1);
    root->addWidget(body, 1);
    setCentralWidget(central);
    applyStyle();
    refreshPurchaseControls();
}

void MainWindow::refreshPurchaseControls()
{
    const QSignalBlocker modeBlock(purchaseModeCombo_), slotBlock(purchaseSlotCombo_), ammoBlock(purchaseAmmoCombo_), thresholdBlock(purchaseThresholdEdit_);
    const auto catalog = controller_.auctionPurchase().catalog();
    purchaseModeCombo_->clear();
    const auto modes = catalog.purchaseModes.isEmpty() ? AuctionPurchase::modes() : catalog.purchaseModes;
    purchaseModeCombo_->addItems(modes);
    if (purchaseModeCombo_->findText(purchaseDraft_.mode) < 0) purchaseModeCombo_->addItem(purchaseDraft_.mode);
    purchaseModeCombo_->setCurrentText(purchaseDraft_.mode);
    purchaseSlotCombo_->setCurrentIndex(purchaseDraft_.selected - 1);
    purchaseThresholdLabel_->setText(QStringLiteral("第 %1 款购买阈值").arg(purchaseDraft_.selected));
    const auto& entry = purchaseDraft_.entries[purchaseDraft_.selected - 1];
    purchaseAmmoCombo_->clear();
    purchaseAmmoCombo_->addItem(QStringLiteral("请选择弹药"), QString());
    for (const auto& item : catalog.ammunition[purchaseDraft_.selected - 1]) purchaseAmmoCombo_->addItem(item, item);
    if (!entry.ammo.isEmpty() && purchaseAmmoCombo_->findData(entry.ammo) < 0)
    {
        purchaseAmmoCombo_->addItem(entry.ammo, entry.ammo);
        auto* model = qobject_cast<QStandardItemModel*>(purchaseAmmoCombo_->model());
        if (model) model->item(purchaseAmmoCombo_->count() - 1)->setEnabled(false);
    }
    purchaseAmmoCombo_->setCurrentIndex(qMax(0, purchaseAmmoCombo_->findData(entry.ammo)));
    purchaseAmmoCombo_->setToolTip(entry.ammo);
    purchaseThresholdEdit_->setText(entry.threshold);
    const auto count = catalog.ammunition[purchaseDraft_.selected - 1].size();
    purchaseMemoryLabel_->setText(count > 0 ? QStringLiteral("每款分别记忆子弹和阈值 · Qt 列表 %1 项").arg(count)
        : QStringLiteral("每款分别记忆子弹和阈值 · 等待客户端 Qt 列表"));
    purchaseStatusLabel_->setText(QStringLiteral("当前选中第 %1 款 · 其他 4 款取消勾选").arg(purchaseDraft_.selected));
    purchaseWarningLabel_->setText(purchaseDraft_.mode == AuctionPurchase::modes().at(2)
        ? QStringLiteral("当前方式不启用购买阈值；各款阈值继续记忆，切回阈值模式后应用。")
        : QStringLiteral("按所选购买方式同步；阈值仅在目标控件允许编辑时写入，并回读核对。"));
}

void MainWindow::connectPurchaseSignals()
{
    auto& sync = controller_.auctionPurchase();
    purchaseEditTimer_.setSingleShot(true);
    purchaseEditTimer_.setInterval(350);
    const auto commit = [this]() { purchaseEditTimer_.stop(); controller_.auctionPurchase().updateConfig(purchaseDraft_); };
    connect(&purchaseEditTimer_, &QTimer::timeout, this, commit);
    connect(purchaseThresholdEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        purchaseDraft_.entries[purchaseDraft_.selected - 1].threshold = text;
        purchaseEditTimer_.start();
    });
    connect(purchaseThresholdEdit_, &QLineEdit::editingFinished, this, commit);
    connect(purchaseSlotCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, commit](int index) {
        if (index < 0) return;
        purchaseDraft_.selected = index + 1;
        commit();
        refreshPurchaseControls();
    });
    connect(purchaseAmmoCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, commit](int index) {
        if (index < 0) return;
        purchaseDraft_.entries[purchaseDraft_.selected - 1].ammo = purchaseAmmoCombo_->itemData(index).toString();
        commit();
    });
    connect(purchaseModeCombo_, &QComboBox::currentTextChanged, this, [this, commit](const QString& mode) {
        if (mode.isEmpty()) return;
        purchaseDraft_.mode = mode;
        commit();
        refreshPurchaseControls();
    });
    connect(&sync, &AuctionPurchaseCoordinator::catalogChanged, this, &MainWindow::refreshPurchaseControls);
    connect(&sync, &AuctionPurchaseCoordinator::statesChanged, this, [this]() {
        auto clients = controller_.clients();
        std::stable_sort(clients.begin(), clients.end(), [](const ClientListItem& left, const ClientListItem& right) {
            if (left.role != right.role) return left.role == ClientRole::Boss;
            return left.id < right.id;
        });
        refreshPurchaseClients(clients);
    });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    purchaseEditTimer_.stop();
    controller_.auctionPurchase().updateConfig(purchaseDraft_);
    QMainWindow::closeEvent(event);
}

void MainWindow::refreshPurchaseClients(const QVector<ClientListItem>& clients)
{
    clientTable_->setRowCount(clients.size());
    const auto& config = controller_.auctionPurchase().config();
    const auto& entry = config.entries[config.selected - 1];
    for (int row = 0; row < clients.size(); ++row)
    {
        const auto& client = clients[row];
        const QString savedNote = resolvedClientNote(client, clients);
        const QString note = savedNote.isEmpty() ? client.ip : savedNote;
        const QStringList cells = {note, QStringLiteral("%1\n%2").arg(QString::fromStdWString(ClientRoleUtil::ToDisplayText(client.role)),
                client.auctionBound ? QStringLiteral("已绑定 Qt") : QStringLiteral("未绑定 Qt")),
            client.auctionBound ? QStringLiteral("第 %1 款 · %2").arg(config.selected).arg(entry.ammo.isEmpty() ? QStringLiteral("未选择弹药") : QString(entry.ammo).remove(QRegularExpression(QStringLiteral("\\s*\\[\\d+\\]$")))) : QStringLiteral("—"),
            client.auctionBound ? controller_.auctionPurchase().syncStatus(client.id) : QStringLiteral("不参与 Qt 同步"),
            client.actionRunning ? client.runningAction : (!client.executionStatus.isEmpty() ? client.executionStatus
                : (client.role == ClientRole::Coin ? client.listingStatus : client.teamReadyText))};
        for (int column = 0; column < cells.size(); ++column)
        {
            auto* item = new QTableWidgetItem(cells[column]);
            item->setToolTip(column == 3 ? controller_.auctionPurchase().syncError(client.id) : cells[column]);
            if (column == 0) { item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(client.id)); item->setData(Qt::UserRole + 1, client.deviceId); }
            if (column == 3 && client.auctionBound) item->setForeground(QColor(controller_.auctionPurchase().syncSuccessful(client.id) ? QStringLiteral("#1c7452") : QStringLiteral("#925813")));
            clientTable_->setItem(row, column, item);
        }
    }
}
