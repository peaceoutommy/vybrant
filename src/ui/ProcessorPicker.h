// Picker for effects and instruments, native and plugin. The default view is "signal cards": one card per processor,
// grouped by what it does (dynamics, EQ, time, ...), each with a small drawing of what it does to a signal, its maker and
// format; favourites and the recently picked ones come first. A toggle (remembered) shows the same items as list rows.
#pragma once
#include "ui/App.h"
#include <QDialog>
#include <QSet>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QToolButton;

namespace mt {

class ProcessorPicker : public QDialog {
    Q_OBJECT
public:
    ProcessorPicker(App& app, ProcessorKind kind, QWidget* parent = nullptr);
    QString nativeKey() const { return m_nativeKey; }
    const PluginInfo* plugin() const { return m_plugin; }
    static QString categoryOf(const PluginInfo& p);   // what a plugin does, from its own categories and name
    static QString thumbnailPath(const QString& pluginKey);   // picture of a plugin's GUI (PluginInfo::key()), saved by PluginWindow
    bool cards() const { return m_cards; }
    bool isFavorite(const QString& key) const { return m_favorites.contains(key); }
    void toggleFavorite(const QString& key);

private:
    struct Entry {
        QString key, name, vendor, format, category;   // key: "native:<key>" or "plugin:<plugin key>"
    };
    void rebuild(bool keepPlace = false);   // keepPlace: same selection and scroll (a star toggled, the view switched)
    void setCards(bool on);
    void choose(QListWidgetItem* it);
    bool eventFilter(QObject* o, QEvent* e) override;
    App& m_app;
    ProcessorKind m_kind;
    std::vector<Entry> m_entries;
    QSet<QString> m_favorites;
    QStringList m_recent;
    QString m_category, m_format;   // rail and chip filters ("" = all)
    bool m_cards = true;
    QLineEdit* m_search;
    QListWidget* m_rail;
    QListWidget* m_items;
    QToolButton *m_cardsBtn, *m_listBtn;
    QString m_nativeKey;
    const PluginInfo* m_plugin = nullptr;
};

}   // namespace mt
