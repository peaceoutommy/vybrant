#include "AppImage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>

namespace mt::appimage {

static QString dataDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation); }   // ~/.local/share
static QString desktopFile() { return dataDir() + "/applications/vybrant.desktop"; }
static QString iconFile() { return dataDir() + "/icons/hicolor/512x512/apps/vybrant.png"; }
static QString mimeFile() { return dataDir() + "/mime/packages/vybrant.xml"; }
static void refreshCaches() {   // menus and file managers pick it up without a new login; a missing tool only delays that
    QProcess::startDetached("update-desktop-database", {dataDir() + "/applications"});
    QProcess::startDetached("update-mime-database", {dataDir() + "/mime"});
}
static bool write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}
static bool same(const QString& a, const QString& b) { return QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath(); }

QString running() { return qEnvironmentVariable("APPIMAGE"); }
QString installedPath() { return QDir::homePath() + "/Applications/Vybrant.AppImage"; }
bool installed() {
    QFile f(desktopFile());   // a launcher from `make install` (a source build) is not ours to replace or remove
    return QFile::exists(installedPath()) && f.open(QIODevice::ReadOnly) && f.readAll().contains(installedPath().toUtf8());
}

bool install(const QString& appImage, QString* error) {
    auto fail = [error](const QString& e) {
        if (error) *error = e;
        return false;
    };
    const QString target = installedPath();
    if (!same(appImage, target)) {   // the installed copy has a fixed name, so a newer version simply replaces it
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(target);
        if (!QFile::copy(appImage, target)) return fail("Could not copy Vybrant to " + target);
        QFile::setPermissions(target, QFile::permissions(target) | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
    }
    QFile icon(":/vybrant.png");
    icon.open(QIODevice::ReadOnly);
    const QByteArray desktop = QString("[Desktop Entry]\nType=Application\nName=Vybrant\nComment=Digital audio workstation\n"
                                       "Exec=\"%1\" %f\nIcon=vybrant\nTerminal=false\nCategories=AudioVideo;Audio;Music;\n"
                                       "MimeType=application/x-vybrant-project;\n")
                                   .arg(target)
                                   .toUtf8();
    const QByteArray mime = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
                            "  <mime-type type=\"application/x-vybrant-project\">\n    <comment>Vybrant project</comment>\n"
                            "    <glob pattern=\"*.mtp\"/>\n  </mime-type>\n</mime-info>\n";
    if (!write(desktopFile(), desktop) || !write(iconFile(), icon.readAll()) || !write(mimeFile(), mime))
        return fail("Could not add the launcher in " + dataDir());
    refreshCaches();
    return true;
}

void uninstall() {
    for (const QString& f : {desktopFile(), iconFile(), mimeFile(), installedPath()}) QFile::remove(f);
    refreshCaches();
}

void offer(QWidget* parent) {
    const QString me = running();
    if (me.isEmpty() || same(me, installedPath()) || QSettings().value("appimage/neverAsk").toBool()) return;
    const bool update = installed();
    QMessageBox box(QMessageBox::Question, "Install Vybrant",
        update ? "Replace the Vybrant in your applications menu with this version?"
               : "Add Vybrant to your applications menu?\n\nIt is copied to ~/Applications and gets a launcher and icon, and .mtp "
                 "projects open in it. Help > Remove from applications menu undoes it.",
        QMessageBox::NoButton, parent);
    QPushButton* yes = box.addButton(update ? "Update" : "Install", QMessageBox::AcceptRole);
    box.addButton("Not now", QMessageBox::RejectRole);
    QPushButton* never = box.addButton("Don't ask again", QMessageBox::DestructiveRole);
    box.exec();
    if (box.clickedButton() == never) QSettings().setValue("appimage/neverAsk", true);
    QString err;
    if (box.clickedButton() == yes && !install(me, &err)) QMessageBox::warning(parent, "Install Vybrant", err);
}

}   // namespace mt::appimage
