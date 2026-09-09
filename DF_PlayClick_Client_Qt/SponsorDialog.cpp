#include "SponsorDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SponsorDialog::SponsorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("授权信息"));
    setWindowIcon(QApplication::windowIcon());
    setModal(true);
    setFixedSize(420, 260);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(28, 26, 28, 26);
    rootLayout->setSpacing(18);

    auto* titleLabel = new QLabel(QStringLiteral("DF_PlayClick 授权版本"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(QStringLiteral("color: #1e3a8a;"));
    rootLayout->addWidget(titleLabel);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("sponsorCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#sponsorCard { background: white; border: 1px solid #dbe4f0; border-radius: 18px; }"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 24, 24, 24);
    cardLayout->setSpacing(14);

    auto* primaryText = new QLabel(QStringLiteral("此版本为付费授权项目，请勿二次分发。"), card);
    QFont primaryFont = primaryText->font();
    primaryFont.setPointSize(13);
    primaryFont.setBold(true);
    primaryText->setFont(primaryFont);
    primaryText->setAlignment(Qt::AlignCenter);
    primaryText->setWordWrap(true);
    primaryText->setStyleSheet(QStringLiteral("color: #0f172a;"));
    cardLayout->addWidget(primaryText);

    rootLayout->addWidget(card, 1);

    auto* buttonBox = new QDialogButtonBox(this);
    auto* okButton = buttonBox->addButton(QStringLiteral("我知道了"), QDialogButtonBox::AcceptRole);
    okButton->setMinimumHeight(36);
    okButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: white; border: none; border-radius: 8px; padding: 8px 24px; }"
        "QPushButton:hover { background: #1d4ed8; }"));
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    rootLayout->addWidget(buttonBox, 0, Qt::AlignCenter);
}
