// Licenses: made on the website (server/) and checked here offline. A license is "<payload>.<signature>" (base64url):
// an Ed25519 signature over a small JSON payload (who it is for), verified against the public key built into the app.
#pragma once
#include <QByteArray>
#include <QString>

class QDialog;
class QWidget;

namespace mt::license {

struct Info {
    bool valid = false;
    QString id, name, email;   // the account it was made for
};

Info verify(const QString& key, const QByteArray& publicKey);   // publicKey: the 32 raw bytes
Info current();   // the stored license, checked against Vybrant's own key
bool ensure();   // a valid license, or the activation dialog until one is entered; false: the user chose to quit
QDialog* makeDialog(QWidget* parent, bool required);   // required: Quit instead of Cancel (startup)

}   // namespace mt::license
