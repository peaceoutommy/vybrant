// Local control socket for scripting and automated testing: `vybrant-ctl <command>` talks to the running instance.
// Socket: $XDG_RUNTIME_DIR/vybrant.sock (one line per command, one line reply).
#pragma once
#include <QObject>
#include <QLocalServer>

namespace mt {
class App;
class MainWindow;

class ControlServer : public QObject {
    Q_OBJECT
public:
    ControlServer(App& app, MainWindow& win, QObject* parent = nullptr);
    static QString socketPath();

private:
    QString handle(const QString& line);
    App& m_app;
    MainWindow& m_win;
    QLocalServer m_server;
};
}   // namespace mt
