// Browser: samples/loops/one-shots folders, instruments, effects, plugins, presets, projects and the user library.
// Supports search, favorites, tags, preview and drag-and-drop into the rack, playlist and mixer.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QMimeData>
#include <QLineEdit>
#include <QSet>
#include <QMap>

namespace mt {

class BrowserModel : public QStandardItemModel {
    Q_OBJECT
public:
    enum Role { PathRole = Qt::UserRole + 1, KindRole, KeyRole, FavRole, TagsRole, LoadedRole, PluginKeyRole };
    enum Kind { Folder, AudioFile, ProjectFile, NativeInstrument, NativeEffect, Plugin, Preset, PatternItem, ChannelItem, LaneItem, Category, MixerItem };
    explicit BrowserModel(App& app, QObject* parent = nullptr);
    void rebuild();
    void populateFolder(QStandardItem* item);
    QStringList mimeTypes() const override { return {"text/uri-list", "application/x-musicthingy-item", "application/x-musicthingy-folder"}; }
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const override { return data->hasFormat("application/x-musicthingy-folder") && !parent.parent().isValid(); }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }   // we reorder ourselves; the view must not remove rows
    Qt::ItemFlags flags(const QModelIndex& i) const override { auto f = QStandardItemModel::flags(i); if (i.isValid()) f |= Qt::ItemIsDragEnabled; if (!i.isValid() || !i.parent().isValid()) f |= Qt::ItemIsDropEnabled; else f &= ~Qt::ItemIsDropEnabled; return f & ~Qt::ItemIsEditable; }
    void moveLibraryDir(const QString& path, int delta);
    bool isRootFolder(const QModelIndex& i) const { return i.isValid() && !i.parent().isValid() && i.data(KindRole).toInt() == Folder; }
    bool isFavorite(const QString& key) const { return m_favorites.contains(key); }
    void toggleFavorite(const QString& key); QStringList tags(const QString& key) const { return m_tags.value(key); } void setTags(const QString& key, const QStringList& t);
    QStringList libraryDirs() const { return m_dirs; } void setLibraryDirs(const QStringList& d);
private:
    QStandardItem* addCategory(const QString& name, Kind kind = Category);
    QStandardItem* addFolder(QStandardItem* parent, const QString& path, const QString& label = QString());
    QStandardItem* makeItem(const QString& text, Kind kind, const QString& key, const QString& path = QString());
    App& m_app; QSet<QString> m_favorites; QMap<QString, QStringList> m_tags; QStringList m_dirs; QStandardItem* m_favRoot = nullptr;
};

class BrowserFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setModelRef(BrowserModel* m) { m_model = m; }
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
private:
    BrowserModel* m_model = nullptr;
};

// "Now playing" strip: waveform of the previewed sample with a progress cursor
class PreviewBar : public QWidget {
    Q_OBJECT
public:
    explicit PreviewBar(App& app, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {200, 58}; }
    void tick();
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override;
private:
    App& m_app; double m_lastPos = -1; bool m_lastPlaying = false;
};

class BrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrowserPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
Q_SIGNALS:
    void openEditor(Id ownerId);
private:
    void onClicked(const QModelIndex& idx); void onDoubleClicked(const QModelIndex& idx); void onContext(const QPoint& pos); void onExpanded(const QModelIndex& idx);
    App& m_app; QTreeView* m_tree; BrowserModel* m_model; BrowserFilter* m_filter; QLineEdit* m_search; bool m_previewEnabled = true; PreviewBar* m_previewBar; QString m_highlighted;
    void highlightPlaying(const QString& path);
};

} // namespace mt
