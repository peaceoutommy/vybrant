#include "License.h"
#include "ui/Theme.h"
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <openssl/evp.h>

namespace mt::license {

// The public half of the key the license server signs with (server: npm run keygen)
static const QByteArray kProductKey = QByteArray::fromHex("f5a35f1d803d86005dced8508e786ce6f0d661f5df19acda71fff534e5021c7e");
// ponytail: the website's local dev address until it has a domain; set the real one before a release
static const char* kAccountUrl = "https://vybrant.studio/account";

Info verify(const QString& key, const QByteArray& publicKey) {
    const QStringList parts = key.simplified().remove(' ').split('.');
    if (parts.size() != 2) return {};
    const auto b64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    const QByteArray body = QByteArray::fromBase64(parts[0].toLatin1(), b64), sig = QByteArray::fromBase64(parts[1].toLatin1(), b64);
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, (const unsigned char*)publicKey.data(), publicKey.size());
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = pk && ctx && sig.size() == 64 && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) == 1 &&
        EVP_DigestVerify(ctx, (const unsigned char*)sig.data(), sig.size(), (const unsigned char*)body.data(), body.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (!ok || o["v"].toInt() != 1) return {};
    return {true, o["sub"].toString(), o["name"].toString(), o["email"].toString()};
}

Info current() { return verify(QSettings().value("license/key").toString(), kProductKey); }

QDialog* makeDialog(QWidget* parent, bool required) {
    auto* d = new QDialog(parent);
    d->setWindowTitle("Vybrant license");
    d->setMinimumWidth(460);
    auto* lay = new QVBoxLayout(d);
    auto* intro = new QLabel(
        QString("Paste your license key. No key yet? <a href=\"%1\" style=\"color:%2\">Get a free license</a> with your account.")
            .arg(kAccountUrl, theme::accent().name()),
        d);
    intro->setWordWrap(true);
    intro->setOpenExternalLinks(true);
    lay->addWidget(intro);
    auto* edit = new QPlainTextEdit(QSettings().value("license/key").toString(), d);
    edit->setObjectName("licenseKey");
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setPlaceholderText("eyJ2IjoxLC...");
    edit->setFixedHeight(84);
    lay->addWidget(edit);
    auto* status = new QLabel(d);
    status->setObjectName("licenseStatus");
    lay->addWidget(status);
    auto* bb = new QDialogButtonBox(d);
    auto* get = bb->addButton("Get a license", QDialogButtonBox::HelpRole);
    auto* ok = bb->addButton(required ? "Activate" : "Save", QDialogButtonBox::AcceptRole);
    bb->addButton(required ? "Quit" : "Cancel", QDialogButtonBox::RejectRole);
    lay->addWidget(bb);
    auto check = [=] {
        const Info i = verify(edit->toPlainText(), kProductKey);
        ok->setEnabled(i.valid);
        status->setText(edit->toPlainText().trimmed().isEmpty() ? QString()
                : i.valid ? QString("Licensed to %1 (%2)").arg(i.name.toHtmlEscaped(), i.email.toHtmlEscaped())
                          : QString("<span style='color:#e06c5a'>This is not a valid Vybrant license key.</span>"));
    };
    QObject::connect(edit, &QPlainTextEdit::textChanged, d, check);
    QObject::connect(get, &QPushButton::clicked, d, [] { QDesktopServices::openUrl(QUrl(kAccountUrl)); });
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [=] {
        QSettings().setValue("license/key", edit->toPlainText().simplified().remove(' '));
        d->accept();
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    check();
    return d;
}

bool ensure() {
    if (current().valid) return true;
    QDialog* d = makeDialog(nullptr, true);
    const bool ok = d->exec() == QDialog::Accepted;
    delete d;
    return ok;
}

}   // namespace mt::license
