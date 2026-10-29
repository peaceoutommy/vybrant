// Linux AppImage: the downloaded file runs as it is, and on its first start offers to install itself like an app: a copy in
// ~/Applications, a launcher and icon in the applications menu, and .mtp projects opening in it. Help menu removes it again.
#pragma once
#include <QString>

class QWidget;

namespace mt::appimage {
QString running();   // the AppImage this process runs from ($APPIMAGE); empty when it is not one (Windows, a source build)
QString installedPath();   // ~/Applications/Vybrant.AppImage
bool installed();   // our launcher (pointing at installedPath()) is in the applications menu
bool install(const QString& appImage, QString* error = nullptr);   // copy it there and add the launcher, icon and file type
void uninstall();
void offer(QWidget* parent);   // on start: running an AppImage that is not the installed one, unless the user said never
}   // namespace mt::appimage
