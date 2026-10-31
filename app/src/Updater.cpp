#include "Updater.h"
#include "AppImage.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QVersionNumber>
#include <cstdio>

#ifndef VYBRANT_VERSION
#define VYBRANT_VERSION ""   // the release workflow sets it from the tag
#endif

namespace mt::updater {

static const QString kBase = "https://download.vybrant.studio", kDownloadPage = "https://vybrant.studio/download";
#ifdef Q_OS_WIN
static const QString kPlatform = "windows";
#else
static const QString kPlatform = "linux";
#endif

QString currentVersion() { return QStringLiteral(VYBRANT_VERSION); }

QJsonObject newerBuild(const QJsonObject& manifest, const QString& current, const QString& platform) {
    const QVersionNumber have = QVersionNumber::fromString(current), latest = QVersionNumber::fromString(manifest["version"].toString());
    const QJsonObject b = manifest[platform].toObject();
    if (have.isNull() || latest <= have || b["path"].toString().isEmpty() || b["sha256"].toString().isEmpty()) return {};
    return b;
}

// An installed Windows copy (Inno Setup's uninstaller sits next to it) runs the new installer; an AppImage swaps its own file
static bool canSelfUpdate() {
#ifdef Q_OS_WIN
    return QFile::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
#else
    return !appimage::running().isEmpty();
#endif
}

// Writes the verified download, closes the window (which asks to save the project) and starts the new version
static void install(QWidget* window, const QByteArray& data) {
#ifdef Q_OS_WIN
    const QString file = QDir::temp().filePath("Vybrant-update-setup.exe");
#else
    const QString app = appimage::running(), file = app + ".update";   // next to it: the swap is one rename
#endif
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.flush()) {
        QMessageBox::warning(window, "Update", "Couldn't save the update: " + f.errorString() + "\nGet it from " + kDownloadPage + ".");
        return;
    }
    f.close();
    f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);
#ifdef Q_OS_WIN
    if (!window->close()) {   // the user went back to the project
        QFile::remove(file);
        return;
    }
    // /SILENT: only the progress window; the installer replaces this copy where it is and starts the new one (vybrant.iss)
    if (!QProcess::startDetached(file, {"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART"}))
        QMessageBox::warning(nullptr, "Update", "Couldn't start the installer. It is at " + QDir::toNativeSeparators(file) + ".");
#else
    if (std::rename(QFile::encodeName(file).constData(), QFile::encodeName(app).constData()) != 0) {   // this process keeps its old copy
        QFile::remove(file);
        QMessageBox::warning(window, "Update", "Couldn't replace " + app + ". Get the new version from " + kDownloadPage + ".");
        return;
    }
    if (!window->close()) {
        QMessageBox::information(window, "Update", "The update is installed: the new version starts next time you open Vybrant.");
        return;
    }
    QProcess::startDetached(app, {});
#endif
    qApp->quit();
}

static void download(QWidget* window, const QJsonObject& build, const QString& version) {
    auto* net = new QNetworkAccessManager(window);
    QNetworkReply* r = net->get(QNetworkRequest(QUrl(kBase + "/" + build["path"].toString())));
    auto* progress = new QProgressDialog("Downloading Vybrant " + version + "...", "Cancel", 0, 100, window);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    QObject::connect(r, &QNetworkReply::downloadProgress, progress, [progress](qint64 got, qint64 total) {
        if (total > 0) progress->setValue(int(got * 100 / total));
    });
    QObject::connect(progress, &QProgressDialog::canceled, r, &QNetworkReply::abort);
    QObject::connect(r, &QNetworkReply::finished, window, [=] {
        progress->disconnect(r);
        progress->hide();
        progress->deleteLater();
        net->deleteLater();
        r->deleteLater();
        if (r->error() == QNetworkReply::OperationCanceledError) return;
        const QByteArray data = r->readAll();
        const bool intact = r->error() == QNetworkReply::NoError && data.size() == (qint64)build["size"].toDouble() &&
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() == build["sha256"].toString().toLatin1();
        if (!intact) {
            QMessageBox::warning(
                window, "Update", "The download didn't arrive intact. Try again later, or get it from " + kDownloadPage + ".");
            return;
        }
        install(window, data);
    });
}

void check(QWidget* window, bool manual) {
    const QString current = currentVersion();
    if (current.isEmpty()) {
        if (manual) QMessageBox::information(window, "Updates", "This is a development build: it doesn't update itself.");
        return;
    }
    auto* net = new QNetworkAccessManager(window);
    QNetworkReply* r = net->get(QNetworkRequest(QUrl(kBase + "/latest.json")));
    QObject::connect(r, &QNetworkReply::finished, window, [=] {
        net->deleteLater();
        r->deleteLater();
        const QJsonObject manifest = QJsonDocument::fromJson(r->readAll()).object();
        if (r->error() != QNetworkReply::NoError || manifest.isEmpty()) {
            if (manual) QMessageBox::warning(window, "Updates", "Couldn't check for updates: " + r->errorString());
            return;
        }
        const QJsonObject build = newerBuild(manifest, current, kPlatform);
        if (build.isEmpty()) {
            if (manual) QMessageBox::information(window, "Updates", "You have the latest version, " + current + ".");
            return;
        }
        const QString version = manifest["version"].toString();
        QMessageBox box(QMessageBox::Information, "Update available",
            QString("<b>Vybrant %1 is available.</b> You have %2.").arg(version, current), QMessageBox::NoButton, window);
        box.setInformativeText(
            "Updates fix bugs and crashes, so we strongly recommend installing it. Your projects, settings and license stay.");
        QPushButton* update = box.addButton(canSelfUpdate() ? "Update now" : "Download", QMessageBox::AcceptRole);
        box.addButton("Later", QMessageBox::RejectRole);
        box.setDefaultButton(update);
        box.exec();
        if (box.clickedButton() != update) return;
        if (canSelfUpdate()) download(window, build, version);
        else QDesktopServices::openUrl(QUrl(kDownloadPage));
    });
}

}   // namespace mt::updater
