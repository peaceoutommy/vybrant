// Vybrant - Linux native DAW. Entry point.
#include "ui/App.h"
#include "ui/MainWindow.h"
#include "ui/ControlServer.h"
#include "ui/Theme.h"
#include "plugins/PluginHost.h"
#include "plugins/PluginBridge.h"
#include "audio/RenderEngine.h"
#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <cstring>
#include "License.h"
#include "AppImage.h"
#include "Updater.h"
#include <cstdio>
namespace mt {
int runSelfTest(App& app);
int runEmbedTest(App& app);
int runPluginTest(App& app, const QString& filter, bool editors, bool bridge);
int runEditorShot(App& app, const QString& key, const QString& out);
int runPreviewBench(App& app, const QString& dir);
int runUiTest(QMainWindow* win, const QString& png);
int runDuplicateTest(App& app);
int runBackupTest(App& app);
int runDockTest(QMainWindow* win);
}   // namespace mt

// The app was called MusicThingy, then Tactum (until 2026-10): its settings (license included), plugin database and pictures,
// presets, templates and autosaves move over from the newest old name found
static void migrateOldName() {
    QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    for (const QString old : {"Tactum", "MusicThingy"}) {
        if (QDir(cfg + "/" + old).exists() && !QDir(cfg + "/Vybrant").exists()) {
            QDir().rename(cfg + "/" + old, cfg + "/Vybrant");
            QFile::rename(cfg + "/Vybrant/" + old + ".conf", cfg + "/Vybrant/Vybrant.conf");
        }
        if (QDir(data + "/" + old + "/" + old).exists() && !QDir(data + "/Vybrant").exists()) {
            QDir().rename(data + "/" + old, data + "/Vybrant");
            QDir().rename(data + "/Vybrant/" + old, data + "/Vybrant/Vybrant");
        }
    }
}

int main(int argc, char** argv) {
    // Plugin scanning runs in a child process so a crashing plugin cannot take the DAW down.
    if (argc >= 3 && std::strcmp(argv[1], "--scan-plugin") == 0) {
        QCoreApplication core(argc, argv);
        QCoreApplication::setOrganizationName("Vybrant");
        QCoreApplication::setApplicationName("Vybrant");
        return mt::PluginScanner::scanChildMain(QString::fromLocal8Bit(argv[2]));
    }
#ifndef _WIN32
    // Plugin GUIs need an X11 window; run under XWayland when a Wayland session is detected and nothing else was requested.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY")) qputenv("QT_QPA_PLATFORM", "xcb");
    // yabridge: a user-installed build in ~/.local/share/yabridge (yabridge's own user install location) takes precedence over the
    // distro package - its chainloader searches PATH first, so put that directory in front for this process and its plugin hosts.
    {
        QString yab = QDir::homePath() + "/.local/share/yabridge";
        if (QFileInfo(yab + "/yabridge-host.exe").exists()) {
            QByteArray path = qgetenv("PATH");
            if (!path.startsWith(yab.toUtf8() + ":")) qputenv("PATH", yab.toUtf8() + ":" + path);
        }
    }
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Vybrant");
    QCoreApplication::setApplicationName("Vybrant");
    migrateOldName();
    QApplication::setStyle("Fusion");
    // Geist, the website's typeface (SIL OFL, resources/fonts): bundled so every system draws the same UI
    for (const char* f : {"Geist-Light", "Geist-Regular", "Geist-Medium", "Geist-SemiBold", "GeistMono-Regular", "GeistMono-Medium"})
        QFontDatabase::addApplicationFont(QString(":/fonts/%1.ttf").arg(f));
    QApplication::setFont(QFont("Geist", 9));
    // The window carries its own icon and names its launcher, so taskbars show the logo without an icon-theme lookup
    // (the AppImage installs no icon) and group the window with vybrant.desktop
    QApplication::setWindowIcon(QIcon(":/vybrant.png"));
    QGuiApplication::setDesktopFileName("vybrant");
    mt::theme::setPalette(mt::theme::loadSavedTheme());
    app.setStyleSheet(mt::theme::stylesheet());
    mt::theme::installClickCursors();
    if (argc >= 4 && std::strcmp(argv[1], "--plugin-bridge") == 0)
        return mt::pluginBridgeMain(argv[2], argv[3]);   // a plugin's own process
    mt::App daw;
    daw.initialize();
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) {
        daw.newProject(true);
        int rc = mt::runSelfTest(daw);
        return rc;
    } /* the tests exercise the demo content */
    if (argc >= 2 && std::strcmp(argv[1], "--embed-test") == 0) return mt::runEmbedTest(daw);
    if (argc >= 4 && std::strcmp(argv[1], "--editor-shot") == 0) return mt::runEditorShot(daw, argv[2], argv[3]);
    if (argc >= 3 && std::strcmp(argv[1], "--plugin-test") == 0) {
        auto has = [&](const char* f) { return std::any_of(argv + 3, argv + argc, [f](const char* a) { return !std::strcmp(a, f); }); };
        return mt::runPluginTest(daw, argv[2], !has("--no-editor"), !has("--no-bridge"));
    }
    if (argc >= 2 && std::strcmp(argv[1], "--duptest") == 0) {
        daw.newProject(true);
        return mt::runDuplicateTest(daw);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--backuptest") == 0) return mt::runBackupTest(daw);
    if (argc >= 3 && std::strcmp(argv[1], "--preview-bench") == 0) return mt::runPreviewBench(daw, argv[2]);
    // A license before the main window; an offscreen run (the UI tests: nothing on a screen) has nothing to license
    if (QGuiApplication::platformName() != "offscreen" && !mt::license::ensure()) return 0;
    mt::MainWindow win(daw);
    win.show();
    QTimer::singleShot(0, &win, [&win] { mt::appimage::offer(&win); });   // an AppImage's first start: install it like an app?
    if (QGuiApplication::platformName() != "offscreen") QTimer::singleShot(4000, &win, [&win] { mt::updater::check(&win, false); });
    mt::ControlServer ctl(daw, win);   // local scripting / test socket ($XDG_RUNTIME_DIR/vybrant.sock)
    QString openFile, screenshot, renderPath, panel;
    bool quitAfter = false;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (a == "--render" && i + 1 < argc) renderPath = argv[++i];
        else if (a == "--panel" && i + 1 < argc) panel = argv[++i];
        else if (a == "--quit") quitAfter = true;
        else if (QFileInfo(a).suffix() == "mtp") openFile = a;
    }
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--demo") == 0) daw.newProject(true);   // demo content (drums + synth + clip) for tests and screenshots
    if (quitAfter) win.setForceClose(true);
    if (!openFile.isEmpty()) daw.openProject(openFile);
    else if (renderPath.isEmpty()) QTimer::singleShot(0, &win, [&win] { win.startupChecks(); });
    if (!renderPath.isEmpty()) {   // batch render: vybrant song.mtp --render out.wav [--quit]
        QTimer::singleShot(500, &win, [&daw, renderPath, quitAfter] {
            mt::RenderOptions o;
            o.path = renderPath;
            o.format = QFileInfo(renderPath).suffix().toLower();
            if (o.format.isEmpty()) o.format = "wav";
            o.songMode = true;
            o.tailSeconds = 1.0;
            QString err;
            bool ok = daw.exportAudio(
                o,
                [](double p) {
                    std::fprintf(stderr, "render %3d%%\r", (int)(p * 100));
                    return true;
                },
                &err);
            std::fprintf(stderr, "\n%s\n", ok ? "Rendered OK" : ("Render failed: " + err).toUtf8().constData());
            if (quitAfter) QApplication::exit(ok ? 0 : 1);
        });
    }
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--docktest") == 0)
            QTimer::singleShot(800, &win, [&win] {
                mt::runDockTest(&win);
                QApplication::quit();
            });
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--uitest") == 0) {
            QString png = argv[i + 1];
            QTimer::singleShot(800, &win, [&win, png] {
                mt::runUiTest(&win, png);
                QApplication::quit();
            });
        }
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--dock") == 0) {
            QString a = argv[i + 1];
            QTimer::singleShot(300, &win, [&win, a] { win.debugDock(a == "top" ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea); });
        }
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--add-plugin") == 0) {
            QString name = argv[i + 1];
            for (auto& p : daw.pluginDb().plugins())
                if (!p.failed && p.name.contains(name, Qt::CaseInsensitive) &&
                    (p.format != "vst2" || name.contains("vst2", Qt::CaseInsensitive))) {
                    mt::Id cid = daw.addPluginChannel(p); /* plus a bar of notes in a clip so --render has something to play */
                    mt::Id pid = daw.project().selectedPatternId;
                    daw.edit<mt::Pattern>(pid, "test notes", [cid](mt::Pattern& pat) {
                        pat.ensureChannel(cid);
                        auto* d = pat.channelData(cid);
                        for (int i = 0; i < 4; ++i) {
                            mt::Note n;
                            n.id = 90000 + i;
                            n.pitch = 60 + i * 2;
                            n.start = (mt::Tick)i * mt::PPQ;
                            n.duration = mt::PPQ / 2;
                            n.velocity = 0.9f;
                            d->notes.push_back(n);
                        }
                    });
                    if (!daw.project().playlistTracks.empty()) daw.addPatternClip(daw.project().playlistTracks.front().id, pid, 0);
                    break;
                }
        }   // test hook: add the first scanned plugin matching the name
    if (!panel.isEmpty())
        QTimer::singleShot(100, &win, [&daw, &win, panel] {
            if (panel == "mixer") daw.commands().action("view.mixer")->trigger();
            else if (panel == "pianoroll") {
                daw.commands().action("view.pianoRoll")->trigger();
            } else if (panel == "plugins") {
                daw.commands().action("options.plugins")->trigger();
            } else if (panel == "editor") {
                win.openEditor(daw.project().channels.empty() ? 0 : daw.project().channels.back().id);
            }
        });
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--search") == 0) {
            QString t = argv[i + 1];
            QTimer::singleShot(300, &win, [&win, t] { win.debugSearch(t); });
        }   // type into the browser search (test hook)
    if (!screenshot.isEmpty())
        QTimer::singleShot(1500, &win, [&win, screenshot, quitAfter] {
            win.saveScreenshot(screenshot);
            if (quitAfter) QApplication::quit();
        });
    else if (quitAfter) QTimer::singleShot(3000, &app, &QApplication::quit);
    return app.exec();
}
