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
#include <QPointer>
#include <QTimer>
#include <memory>
#include <atomic>
#include <vector>

namespace mt {

class BrowserModel : public QStandardItemModel {
    Q_OBJECT
public:
    enum Role { PathRole = Qt::UserRole + 1, KindRole, KeyRole, FavRole, TagsRole, LoadedRole, PluginKeyRole };
    enum Kind {
        Folder,
        AudioFile,
        ProjectFile,
        NativeInstrument,
        NativeEffect,
        Plugin,
        Preset,
        PatternItem,
        ChannelItem,
        LaneItem,
        Category,
        MixerItem
    };
    explicit BrowserModel(App& app, QObject* parent = nullptr);
    void rebuild();
    void populateFolder(QStandardItem* item);
    // Search: a flat result list built from the in-memory tree plus a background index of every library file (no disk access per keystroke)
    void setSearch(const QString& text);
    bool searching() const { return !m_search.isEmpty(); }
    void reindex();   // rescan the library folders in the background (called when they change)
    QStringList mimeTypes() const override {
        return {"text/uri-list", "application/x-musicthingy-item", "application/x-musicthingy-folder"};
    }
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const override {
        return data->hasFormat("application/x-musicthingy-folder") && !parent.parent().isValid();
    }
    Qt::DropActions supportedDropActions() const override {
        return Qt::CopyAction;
    }   // we reorder ourselves; the view must not remove rows
    Qt::ItemFlags flags(const QModelIndex& i) const override {
        auto f = QStandardItemModel::flags(i);
        if (i.isValid()) f |= Qt::ItemIsDragEnabled;
        if (!i.isValid() || !i.parent().isValid()) f |= Qt::ItemIsDropEnabled;
        else f &= ~Qt::ItemIsDropEnabled;
        return f & ~Qt::ItemIsEditable;
    }
    void moveLibraryDir(const QString& path, int delta);
Q_SIGNALS:
    void indexChanged();

public:
    bool isRootFolder(const QModelIndex& i) const { return i.isValid() && !i.parent().isValid() && i.data(KindRole).toInt() == Folder; }
    bool isFavorite(const QString& key) const { return m_favorites.contains(key); }
    void toggleFavorite(const QString& key);
    QStringList tags(const QString& key) const { return m_tags.value(key); }
    void setTags(const QString& key, const QStringList& t);
    QStringList libraryDirs() const { return m_dirs; }
    void setLibraryDirs(const QStringList& d);

private:
    QStandardItem* addCategory(const QString& name, Kind kind = Category);
    QStandardItem* addFolder(QStandardItem* parent, const QString& path, const QString& label = QString());
    QStandardItem* makeItem(const QString& text, Kind kind, const QString& key, const QString& path = QString());
    void buildTree();
    void buildSearch();
    App& m_app;
    QSet<QString> m_favorites;
    QMap<QString, QStringList> m_tags;
    QStringList m_dirs;
    QStandardItem* m_favRoot = nullptr;
    QString m_search;
    struct IndexEntry {
        QString path, name, lower;
        bool audio;
    };
    struct Index {
        std::vector<IndexEntry> files;
    };
    std::shared_ptr<const Index> m_index;
    std::shared_ptr<std::atomic<int>> m_indexGen = std::make_shared<std::atomic<int>>(0);
};

// "Now playing" strip: waveform of the previewed sample with a progress cursor
class PreviewBar : public QWidget {
    Q_OBJECT
public:
    explicit PreviewBar(App& app, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {200, 58}; }
    void tick();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    App& m_app;
    double m_lastPos = -1;
    bool m_lastPlaying = false;
};

// Tree with a custom drag: a name badge under the cursor and an explicit 'copy' cursor for every accepted action (the theme's
// dnd cursors are not consulted, some show a no-drop glyph for copy)
class BrowserTree : public QTreeView {
public:
    using QTreeView::QTreeView;

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

class BrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit BrowserPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags);
    void uiTick();
    void debugSearch(const QString& t) {
        m_search->setText(t);
        m_searchTimer.stop();
        applySearch();
    }
Q_SIGNALS:
    void openEditor(Id ownerId);

private:
    void onClicked(const QModelIndex& idx);
    void onDoubleClicked(const QModelIndex& idx);
    void onContext(const QPoint& pos);
    void onExpanded(const QModelIndex& idx);
    void onCurrentChanged(const QModelIndex& idx);
    void rebuildKeepingExpanded();
    void applySearch();
    void addFolderDialog();
    bool eventFilter(QObject* o, QEvent* e) override;
    App& m_app;
    BrowserTree* m_tree;
    BrowserModel* m_model;
    QLineEdit* m_search;
    QTimer m_searchTimer;
    PreviewBar* m_previewBar;
    QString m_highlighted;
    qint64 m_lastAutoPreview = 0;
    QStringList m_savedExpanded;
    bool m_restoring = false;
    void highlightPlaying(const QString& path);
};

}   // namespace mt
