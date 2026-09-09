#pragma once

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>

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
    void appendConnectionLog(const QString& text);
    quint64 clientIdForRow(int row) const;

    ServerController controller_;
    QLabel* listenBadge_ = nullptr;
    QLabel* clientCountBadge_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* flowPreviewLabel_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QLineEdit* countdownEdit_ = nullptr;
    QLineEdit* groupDelayEdit_ = nullptr;
    QLineEdit* listingTextEdit_ = nullptr;
    QLineEdit* listingPriceEdit_ = nullptr;
    QSpinBox* listingPurchaseCountSpin_ = nullptr;
    QCheckBox* listingExchangeCheck_ = nullptr;
    QRadioButton* coinFirstRadio_ = nullptr;
    QRadioButton* bossFirstRadio_ = nullptr;
    QPushButton* listenButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* triggerAllButton_ = nullptr;
    QPushButton* listingButton_ = nullptr;
    QPushButton* rebindAllButton_ = nullptr;
    QPushButton* forceStopAllButton_ = nullptr;
    QPushButton* mailAllButton_ = nullptr;
    QPushButton* clientStatusButton_ = nullptr;
    QTableWidget* clientTable_ = nullptr;
    QTextEdit* connectionLogEdit_ = nullptr;
};
