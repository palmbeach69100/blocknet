// Copyright (c) 2018 The Blocknet Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blocknetsendfunds1.h"

#include <QMessageBox>
#include <QKeyEvent>

BlocknetSendFunds1::BlocknetSendFunds1(WalletModel *w, int id, QFrame *parent) : BlocknetSendFundsPage(w, id, parent),
                                                                                 layout(new QVBoxLayout) {
//    this->setStyleSheet("border: 1px solid red");
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->setLayout(layout);
    layout->setContentsMargins(15, 10, 35, 30);

    titleLbl = new QLabel(tr("Send Funds"));
    titleLbl->setObjectName("h4");

    QLabel *subtitleLbl = new QLabel(tr("Who would you like to send funds to?"));
    subtitleLbl->setObjectName("h2");

    addressTi = new BlocknetAddressEditor(675);
    addressTi->setPlaceholderText(tr("Enter Blocknet Address..."));

    continueBtn = new BlocknetFormBtn;
    continueBtn->setText(tr("Continue"));
    continueBtn->setDisabled(true);

    QLabel *hdiv = new QLabel;
    hdiv->setFixedHeight(1);
    hdiv->setObjectName("hdiv");

    layout->addWidget(titleLbl, 0, Qt::AlignTop | Qt::AlignLeft);
    layout->addSpacing(45);
    layout->addWidget(subtitleLbl, 0, Qt::AlignTop);
    layout->addSpacing(25);
    layout->addWidget(addressTi);
    layout->addSpacing(45);
    layout->addWidget(hdiv);
    layout->addSpacing(45);
    layout->addWidget(continueBtn);
    layout->addStretch(1);

    connect(addressTi, SIGNAL(textChanged()), this, SLOT(textChanged()));
    connect(addressTi, SIGNAL(addresses()), this, SLOT(onAddressesChanged()));
    connect(addressTi, SIGNAL(returnPressed()), this, SLOT(onNext()));
    connect(continueBtn, SIGNAL(clicked()), this, SLOT(onNext()));
}

void BlocknetSendFunds1::setData(BlocknetSendFundsModel *model) {
    BlocknetSendFundsPage::setData(model);
}

void BlocknetSendFunds1::clear() {
    addressTi->clear();
}

void BlocknetSendFunds1::focusInEvent(QFocusEvent *event) {
    QWidget::focusInEvent(event);
    addressTi->setFocus();
}

void BlocknetSendFunds1::keyPressEvent(QKeyEvent *event) {
    QWidget::keyPressEvent(event);
    if (this->isHidden())
        return;
    if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        onNext();
}

void BlocknetSendFunds1::textChanged() {
    continueBtn->setDisabled(addressTi->getAddresses().isEmpty());
}

bool BlocknetSendFunds1::validated() {
    if (addressTi->getAddresses().isEmpty()) {
        QMessageBox::warning(this->parentWidget(), tr("Issue"), tr("Please add a valid address to the send box."));
        return false;
    }

    // Use wallet to validate address
    auto addresses = addressTi->getAddresses().toList();
    QString invalidAddresses;
    for (QString &addr : addresses) {
        if (!walletModel->validateAddress(addr))
            invalidAddresses += QString("\n%1").arg(addr);
    }

    // Display invalid addresses
    if (!invalidAddresses.isEmpty()) {
        QMessageBox::warning(this->parentWidget(), tr("Issue"),
                             QString("%1:\n%2").arg(tr("Please correct the invalid addresses below"), invalidAddresses));
        return false;
    }

    return true;
}

void BlocknetSendFunds1::onAddressesChanged() {
    // First add any new addresses (do not overwrite existing)
    auto addresses = addressTi->getAddresses();
    QHash<QString, BlocknetTransaction> hash;
    for (const QString &addr : addresses) {
        BlocknetTransaction r(addr);
        hash[addr] = r;
        model->addRecipient(r);
    }
    // Remove any unspecified addresses
    auto txlist = model->recipients.toList();
    for (const BlocknetTransaction &r : txlist) {
        if (!hash.contains(r.address))
            model->removeRecipient(r);
    }
}