// Central registry of user commands with customisable keyboard shortcuts (persisted in QSettings).
#pragma once
#include <QObject>
#include <QAction>
#include <QKeySequence>
#include <QMap>
#include <functional>

namespace mt {

struct CommandEntry { QString id; QString description; QString category; QKeySequence defaultShortcut; QKeySequence altShortcut; QKeySequence userShortcut; QAction* action = nullptr; };

class CommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit CommandRegistry(QObject* parent = nullptr);
    QAction* add(const QString& id, const QString& text, const QString& category, const QKeySequence& def, std::function<void()> fn, QObject* parent, bool checkable = false, const QKeySequence& alt = QKeySequence());
    QAction* action(const QString& id) const { auto it = m_entries.find(id); return it == m_entries.end() ? nullptr : it->action; }
    const QMap<QString, CommandEntry>& entries() const { return m_entries; }
    void setUserShortcut(const QString& id, const QKeySequence& seq);
    void resetShortcut(const QString& id);
    QKeySequence effectiveShortcut(const CommandEntry& e) const { return e.userShortcut.isEmpty() && !m_userSet.contains(e.id) ? e.defaultShortcut : e.userShortcut; }
private:
    void load(); void save() const; void apply(CommandEntry& e);
    QMap<QString, CommandEntry> m_entries; QMap<QString, QKeySequence> m_overrides; QSet<QString> m_userSet;
};

} // namespace mt
