// MusicThingy - Linux native DAW. Entry point.
#include "ui/App.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "plugins/PluginHost.h"
#include "audio/RenderEngine.h"
#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QFileInfo>
#include <cstring>
#include <cstdio>
namespace mt { int runSelfTest(App& app); int runPreviewBench(App& app, const QString& dir); int runUiTest(QMainWindow* win, const QString& png); int runDuplicateTest(App& app); }

int main(int argc, char** argv) {
    // Plugin scanning runs in a child process so a crashing plugin cannot take the DAW down.
    if (argc >= 3 && std::strcmp(argv[1], "--scan-plugin") == 0) { QCoreApplication core(argc, argv); QCoreApplication::setOrganizationName("MusicThingy"); QCoreApplication::setApplicationName("MusicThingy"); return mt::PluginScanner::scanChildMain(QString::fromLocal8Bit(argv[2])); }
    // Plugin GUIs need an X11 window; run under XWayland when a Wayland session is detected and nothing else was requested.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY")) qputenv("QT_QPA_PLATFORM", "xcb");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MusicThingy"); QCoreApplication::setApplicationName("MusicThingy"); QApplication::setStyle("Fusion"); app.setStyleSheet(mt::theme::stylesheet());
    mt::App daw; daw.initialize();
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) { int rc = mt::runSelfTest(daw); return rc; }
    if (argc >= 2 && std::strcmp(argv[1], "--duptest") == 0) return mt::runDuplicateTest(daw);
    if (argc >= 3 && std::strcmp(argv[1], "--preview-bench") == 0) return mt::runPreviewBench(daw, argv[2]);
    mt::MainWindow win(daw); win.show();
    QString openFile, screenshot, renderPath, panel; bool quitAfter = false;
    for (int i = 1; i < argc; ++i) { QString a = argv[i]; if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i]; else if (a == "--render" && i + 1 < argc) renderPath = argv[++i]; else if (a == "--panel" && i + 1 < argc) panel = argv[++i]; else if (a == "--quit") quitAfter = true; else if (QFileInfo(a).suffix() == "mtp") openFile = a; }
    if (quitAfter) win.setForceClose(true);
    if (!openFile.isEmpty()) daw.openProject(openFile); else if (renderPath.isEmpty()) QTimer::singleShot(0, &win, [&win] { win.startupChecks(); });
    if (!renderPath.isEmpty()) { // batch render: musicthingy song.mtp --render out.wav [--quit]
        QTimer::singleShot(500, &win, [&daw, renderPath, quitAfter] { mt::RenderOptions o; o.path = renderPath; o.format = QFileInfo(renderPath).suffix().toLower(); if (o.format.isEmpty()) o.format = "wav"; o.songMode = true; o.tailSeconds = 1.0; QString err; bool ok = daw.exportAudio(o, [](double p) { std::fprintf(stderr, "render %3d%%\r", (int)(p * 100)); return true; }, &err); std::fprintf(stderr, "\n%s\n", ok ? "Rendered OK" : ("Render failed: " + err).toUtf8().constData()); if (quitAfter) QApplication::exit(ok ? 0 : 1); });
    }
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--uitest") == 0) { QString png = argv[i + 1]; QTimer::singleShot(800, &win, [&win, png] { mt::runUiTest(&win, png); QApplication::quit(); }); }
    if (!panel.isEmpty()) QTimer::singleShot(100, &win, [&daw, &win, panel] { if (panel == "mixer") daw.commands().action("view.mixer")->trigger(); else if (panel == "pianoroll") { daw.commands().action("view.pianoRoll")->trigger(); } else if (panel == "editor") { win.openEditor(daw.project().channels.empty() ? 0 : daw.project().channels.back().id); } });
    if (!screenshot.isEmpty()) QTimer::singleShot(1500, &win, [&win, screenshot, quitAfter] { win.saveScreenshot(screenshot); if (quitAfter) QApplication::quit(); });
    else if (quitAfter) QTimer::singleShot(3000, &app, &QApplication::quit);
    return app.exec();
}
