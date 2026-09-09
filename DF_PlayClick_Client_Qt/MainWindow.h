#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTextEdit>

#include "ClientController.h"

class QLayout;
class QShowEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    QWidget* createAppHeader();
    QWidget* createCard(const QString& title, QLayout* contentLayout);
    QWidget* createMetricPanel(const QString& caption, QLabel*& valueLabel);
    QLabel* createBadge(const QString& text, const QString& background, const QString& color);
    void buildUi();
    void connectSignals();
    void applyStyle();
    void saveOcrParameters();
    void refreshConnectionControls();
    void updateConnectedState(bool connected);
    void updateReconnectingState(bool reconnecting);
    void appendConnectionLog(const QString& text);
    void showControllerMessage(const QString& title, const QString& text, bool error);

    ClientController controller_;
    bool initialized_ = false;
    QLabel* connectionBadge_ = nullptr;
    QLabel* connectionDetailLabel_ = nullptr;
    QLineEdit* ipEdit_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QLineEdit* unitPriceEdit_ = nullptr;
    QLineEdit* bulletTotalEdit_ = nullptr;
    QLabel* ocrValueLabel_ = nullptr;
    QLabel* unitValueLabel_ = nullptr;
    QLabel* configuredUnitPriceLabel_ = nullptr;
    QLabel* ocrDecisionLabel_ = nullptr;
    QLabel* clientRoleLabel_ = nullptr;
    QLabel* clickStrategyLabel_ = nullptr;
    QLabel* executionStatusLabel_ = nullptr;
    QTextEdit* connectionLogEdit_ = nullptr;
};
