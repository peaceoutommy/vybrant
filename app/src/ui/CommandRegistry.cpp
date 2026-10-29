#include "ui/CommandRegistry.h"
#include <QSettings>

namespace mt {

CommandRegistry::CommandRegistry(QObject* parent)
    : QObject(parent) {
    load();
}
void CommandRegistry::load() {
    QSettings s;
    s.beginGroup("shortcuts");
    for (auto& k : s.allKeys()) {
        m_overrides[k] = QKeySequence(s.value(k).toString());
        m_userSet.insert(k);
    }
}
void CommandRegistry::save() const {
    QSettings s;
    s.beginGroup("shortcuts");
    s.remove("");
    for (auto it = m_overrides.begin(); it != m_overrides.end(); ++it) s.setValue(it.key(), it.value().toString());
}
QAction* CommandRegistry::add(const QString& id, const QString& text, const QString& category, const QKeySequence& def,
    std::function<void()> fn, QObject* parent, bool checkable, const QKeySequence& alt) {
    auto* a = new QAction(text, parent);
    a->setCheckable(checkable);
    a->setShortcutContext(Qt::ApplicationShortcut);
    if (fn) QObject::connect(a, &QAction::triggered, [fn](bool) { fn(); });
    CommandEntry e;
    e.id = id;
    e.description = text;
    e.category = category;
    e.defaultShortcut = def;
    e.altShortcut = alt;
    e.action = a;
    if (m_overrides.contains(id)) e.userShortcut = m_overrides[id];
    apply(e);
    m_entries[id] = e;
    return a;
}
void CommandRegistry::apply(CommandEntry& e) {
    QKeySequence seq = effectiveShortcut(e);
    QList<QKeySequence> all{seq};
    if (!m_userSet.contains(e.id) && !e.altShortcut.isEmpty()) all << e.altShortcut;
    e.action->setShortcuts(all);
    e.action->setToolTip(seq.isEmpty() ? e.description : e.description + " (" + seq.toString(QKeySequence::NativeText) + ")");
}
void CommandRegistry::setUserShortcut(const QString& id, const QKeySequence& seq) {
    if (!m_entries.contains(id)) return;
    m_overrides[id] = seq;
    m_userSet.insert(id);
    m_entries[id].userShortcut = seq;
    apply(m_entries[id]);
    save();
}
void CommandRegistry::resetShortcut(const QString& id) {
    if (!m_entries.contains(id)) return;
    m_overrides.remove(id);
    m_userSet.remove(id);
    m_entries[id].userShortcut = QKeySequence();
    apply(m_entries[id]);
    save();
}

}   // namespace mt
