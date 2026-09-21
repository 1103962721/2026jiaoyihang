#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>

#include "ServerController.h"
#include "ServerModels.h"

class QLayout;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void connectSignals();
    QWidget* createAppHeader();
    QWidget* createCard(const QString& title, QLayout* contentLayout);
    QLabel* createBadge(const QString& text, const QString& background, const QString& color);
    void applyStyle();
    void refreshClients(const QVector<ClientListItem>& clients);
    void updateListeningState(bool listening);
    void updateStatus(const QString& text, ServerStatusTone tone);
    void updateFlowPreview();
    void showSettingsDialog();
    void appendConnectionLog(const QString& text);
    quint64 clientIdForRow(int row) const;

    ServerController controller_;
    QLabel* listenBadge_ = nullptr;
    QLabel* clientCountBadge_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* serverTimeLabel_ = nullptr;
    QLabel* flowPreviewLabel_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QLineEdit* countdownEdit_ = nullptr;
    QLineEdit* groupDelayEdit_ = nullptr;
    QLineEdit* compensationLeadEdit_ = nullptr;
    QLineEdit* listingTextEdit_ = nullptr;
    QLineEdit* listingPriceEdit_ = nullptr;
    QLineEdit* bossPriceEdit_ = nullptr;
    QSpinBox* listingPurchaseCountSpin_ = nullptr;
    QLabel* listingQuantityTotalLabel_ = nullptr;
    QLabel* listingSuccessClientsLabel_ = nullptr;
    bool coinFirst_ = true;
    bool compensationEnabled_ = true;
    bool bruteCaptcha_ = true;
    bool tradePurchase_ = true;
    QPushButton* listenButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* triggerAllButton_ = nullptr;
    QPushButton* listingButton_ = nullptr;
    QPushButton* rebindAllButton_ = nullptr;
    QPushButton* forceStopAllButton_ = nullptr;
    QPushButton* mailAllButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
    QPushButton* clientStatusButton_ = nullptr;
    QTableWidget* clientTable_ = nullptr;
    QTextEdit* connectionLogEdit_ = nullptr;
    QTimer serverTimeTimer_;
};
