#include "ui/BrowserPanel.h"
#include "project/MidiFile.h"
#include "ui/InAppDrag.h"
#include "ui/Theme.h"
#include "dsp/NativeRegistry.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QDir>
#include <QSettings>
#include <QMimeData>
#include <QUrl>
#include <QMenu>
#include <QInputDialog>
#include <QFileDialog>
#include <QStandardPaths>
#include <QHeaderView>
#include <QScrollBar>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QDrag>
#include <QCursor>
#include <QDragLeaveEvent>
#include <QEventLoop>
#include <QDirIterator>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QDateTime>
#include <QApplication>
#include <thread>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QHelpEvent>
#include "ui/widgets/Controls.h"

namespace mt {

// ---------------------------------------------------------------- model
BrowserModel::BrowserModel(App& app, QObject* parent)
    : QStandardItemModel(parent)
    , m_app(app) {
    QSettings s;
    auto loadFavorites = [this] {
        const QStringList f = QSettings().value("browser/favorites").toStringList();
        m_favorites = QSet<QString>(f.begin(), f.end());
    };
    loadFavorites();
    connect(&app, &App::favoritesChanged, this, [this, loadFavorites] {
        loadFavorites();
        rebuild();
    });
    auto tagMap = s.value("browser/tags").toMap();
    for (auto it = tagMap.begin(); it != tagMap.end(); ++it) m_tags[it.key()] = it.value().toStringList();
    m_dirs = s.value("browser/dirs").toStringList();
    if (m_dirs.isEmpty()) {
        m_dirs << QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        for (auto& d : {"/usr/share/sounds", "/usr/share/hydrogen/data/drumkits", "/usr/share/sounds/sf2"})
            if (QDir(d).exists()) m_dirs << d;
    }
    reindex();
}
// Background scan of every library folder: the search matches against this list instead of walking the disk per keystroke
void BrowserModel::reindex() {
    int gen = ++(*m_indexGen);
    auto token = m_indexGen;
    QStringList dirs = m_dirs;
    QPointer<BrowserModel> self(this);
    std::thread([gen, token, dirs, self] {
        auto idx = std::make_shared<Index>();
        for (auto& d : dirs) {
            QDirIterator it(d, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (token->load() != gen) return;
                QString p = it.next();
                bool audio = AudioFile::isAudioFile(p);
                if (!audio && !p.endsWith(".mtp") && !MidiFile::isMidiFile(p)) continue;
                QString name = it.fileName();
                idx->files.push_back({p, name, p.mid(d.size()).toLower(),
                    audio}); /* matched on the path inside the library so folder names count ("kick" finds Kicks/808.wav) */
                if (idx->files.size() > 400000) break;
            }
        }
        std::sort(idx->files.begin(), idx->files.end(),
            [](const IndexEntry& a, const IndexEntry& b) { return a.name.toLower() < b.name.toLower(); });
        QMetaObject::invokeMethod(
            qApp,
            [gen, token, idx, self] {
                if (!self || token->load() != gen) return;
                self->m_index = idx;
                Q_EMIT self->indexChanged();
            },
            Qt::QueuedConnection);
    }).detach();
}
void BrowserModel::setLibraryDirs(const QStringList& d) {
    m_dirs = d;
    QSettings s;
    s.setValue("browser/dirs", d);
    s.sync();
    reindex();
    rebuild();
}
void BrowserModel::toggleFavorite(const QString& key) {
    if (m_favorites.contains(key)) m_favorites.remove(key);
    else m_favorites.insert(key);
    QSettings().setValue("browser/favorites", QStringList(m_favorites.begin(), m_favorites.end()));
    rebuild();
}
void BrowserModel::setTags(const QString& key, const QStringList& t) {
    if (t.isEmpty()) m_tags.remove(key);
    else m_tags[key] = t;
    QVariantMap m;
    for (auto it = m_tags.begin(); it != m_tags.end(); ++it) m[it.key()] = it.value();
    QSettings().setValue("browser/tags", m);
    rebuild();
}

QStandardItem* BrowserModel::makeItem(const QString& text, Kind kind, const QString& key, const QString& path) {
    auto* it = new QStandardItem(text);
    it->setData(path, PathRole);
    it->setData((int)kind, KindRole);
    it->setData(key, KeyRole);
    it->setData(m_favorites.contains(key), FavRole);
    it->setData(m_tags.value(key).join(' '), TagsRole);
    if (m_favorites.contains(key)) it->setText("★ " + text);
    QColor c = theme::text();
    switch (kind) {
    case Folder:
    case Category: c = theme::textDim(); break;
    case AudioFile: c = QColor("#8fd0ff"); break;
    case NativeInstrument: c = QColor("#c9a2ff"); break;
    case NativeEffect: c = QColor("#a2ffb8"); break;
    case Plugin: c = QColor("#ffd27f"); break;
    case Preset: c = QColor("#ffb6a2"); break;
    case PatternItem: c = theme::accent(); break;
    default: break;
    }
    it->setForeground(c);
    it->setEditable(false);
    return it;
}
QStandardItem* BrowserModel::addCategory(const QString& name, Kind kind) {
    auto* it = makeItem(name, kind, "cat:" + name);
    QFont f = theme::boldFont();
    it->setFont(f);
    appendRow(it);
    return it;
}
QStandardItem* BrowserModel::addFolder(QStandardItem* parent, const QString& path, const QString& label) {
    auto* it = makeItem(label.isEmpty() ? QFileInfo(path).fileName() : label, Folder, "dir:" + path, path);
    it->setData(false, LoadedRole);
    it->appendRow(new QStandardItem("..."));
    parent->appendRow(it);
    return it;
}
void BrowserModel::populateFolder(QStandardItem* item) {
    if (item->data(KindRole).toInt() != Folder || item->data(LoadedRole).toBool()) return;
    item->removeRows(0, item->rowCount());
    item->setData(true, LoadedRole);
    QDir d(item->data(PathRole).toString());
    for (auto& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) addFolder(item, fi.absoluteFilePath());
    for (auto& fi : d.entryInfoList(QDir::Files, QDir::Name)) {
        QString p = fi.absoluteFilePath();
        if (AudioFile::isAudioFile(p)) item->appendRow(makeItem(fi.fileName(), AudioFile, "file:" + p, p));
        else if (fi.suffix() == "mtp" || MidiFile::isMidiFile(p)) item->appendRow(makeItem(fi.fileName(), ProjectFile, "file:" + p, p));
    }
}
void BrowserModel::rebuild() {
    if (m_search.isEmpty()) buildTree();
    else buildSearch();
}
void BrowserModel::setSearch(const QString& text) {
    m_search = text.trimmed();
    rebuild();
}
void BrowserModel::buildTree() {
    clear();
    setHorizontalHeaderLabels({"Browser"});
    // Current project
    auto* proj = addCategory("Current project");
    auto* pats = makeItem("Patterns", Category, "cat:patterns");
    proj->appendRow(pats);
    for (auto& p : m_app.project().patterns) {
        auto* it = makeItem(p.name, PatternItem, "pattern:" + QString::number(p.id), QString::number(p.id));
        it->setForeground(p.color);
        pats->appendRow(it);
    }
    auto* chans = makeItem("Channels", Category, "cat:channels");
    proj->appendRow(chans);
    for (auto& c : m_app.project().channels) {
        auto* it = makeItem(c.name, ChannelItem, "channel:" + QString::number(c.id), QString::number(c.id));
        it->setForeground(c.color);
        chans->appendRow(it);
    }
    auto* lanes = makeItem("Automation", Category, "cat:lanes");
    proj->appendRow(lanes);
    for (auto& l : m_app.project().automationLanes)
        lanes->appendRow(makeItem(l.name, LaneItem, "lane:" + QString::number(l.id), QString::number(l.id)));
    auto* mix = makeItem("Mixer tracks", Category, "cat:mixer");
    proj->appendRow(mix);
    for (auto& t : m_app.project().mixerTracks) {
        auto* it = makeItem(t.name + (t.effects.empty() ? "" : QString("  (%1 fx)").arg(t.effects.size())), MixerItem,
            "mixer:" + QString::number(t.id), QString::number(t.id));
        mix->appendRow(it);
    }
    // every sample the project references (audio clips - including ones removed from the playlist - and channel samples)
    auto* psamples = makeItem("Samples", Category, "cat:projsamples");
    proj->appendRow(psamples);
    QSet<QString> seen;
    for (auto& a : m_app.project().audioSources) {
        if (a.filePath.isEmpty() || seen.contains(a.filePath)) continue;
        seen.insert(a.filePath);
        int uses = 0;
        for (auto& c : m_app.project().clips)
            if (c.type == ClipType::Audio && c.sourceId == a.id) ++uses;
        auto* it = makeItem(QString("%1%2").arg(a.name.isEmpty() ? QFileInfo(a.filePath).fileName() : a.name,
                                uses ? QString("  (%1 clip%2)").arg(uses).arg(uses > 1 ? "s" : "") : "  (not in playlist)"),
            AudioFile, "file:" + a.filePath, a.filePath);
        if (a.missing) it->setForeground(theme::red());
        if (!uses) it->setForeground(theme::textDim());
        psamples->appendRow(it);
    }
    for (auto& c : m_app.project().channels) {
        if (c.samplePath.isEmpty() || c.samplePath.startsWith("builtin:") || seen.contains(c.samplePath)) continue;
        seen.insert(c.samplePath);
        psamples->appendRow(
            makeItem(QFileInfo(c.samplePath).fileName() + "  (channel: " + c.name + ")", AudioFile, "file:" + c.samplePath, c.samplePath));
    }
    if (psamples->rowCount() == 0) psamples->appendRow(makeItem("(no samples used yet)", Category, "hint0"));
    // Favorites
    m_favRoot = addCategory("Favorites");
    // Library folders: each one is a top-level entry (drag to reorder, right-click to remove)
    for (auto& d : m_dirs)
        if (QDir(d).exists()) {
            auto* it = makeItem(QFileInfo(d).fileName().isEmpty() ? d : QFileInfo(d).fileName(), Folder, "dir:" + d, d);
            QFont f = theme::boldFont();
            it->setFont(f);
            it->setForeground(theme::text());
            it->setData(false, LoadedRole);
            it->appendRow(new QStandardItem("..."));
            it->setToolTip(d);
            appendRow(it);
        }
    // Instruments & effects
    auto* inst = addCategory("Instruments");
    auto* fx = addCategory("Effects");
    std::map<QString, QStandardItem*> fxCats;
    for (auto& i : NativeRegistry::instance().all()) {
        if (i.kind == ProcessorKind::Instrument) {
            inst->appendRow(makeItem(i.name, NativeInstrument, "native:" + i.key, i.key));
        } else {
            auto& cat = fxCats[i.category];
            if (!cat) {
                cat = makeItem(i.category, Category, "cat:fx:" + i.category);
                fx->appendRow(cat);
            }
            cat->appendRow(makeItem(i.name, NativeEffect, "native:" + i.key, i.key));
        }
    }
    // Plugins
    auto* plugs = addCategory("Plugins");
    auto* pi = makeItem("Instruments", Category, "cat:plugins:inst");
    auto* pe = makeItem("Effects", Category, "cat:plugins:fx");
    auto* pf = makeItem("Failed", Category, "cat:plugins:failed");
    plugs->appendRow(pi);
    plugs->appendRow(pe);
    plugs->appendRow(pf);
    for (auto& p : m_app.pluginDb().plugins()) {
        if (p.hidden) continue;
        if (p.failed) {
            auto* it = makeItem(p.name + " - " + p.failReason, Plugin, "plugin:" + p.key(), p.path);
            it->setForeground(theme::red());
            it->setDragEnabled(false);
            pf->appendRow(it);
            continue;
        }
        auto* it = makeItem(QString("%1 (%2) [%3]").arg(p.name, p.vendor, p.format.toUpper()), Plugin, "plugin:" + p.key(), p.path);
        it->setData(p.key(), PluginKeyRole);
        (p.category == "Instrument" ? pi : pe)->appendRow(it);
    }
    if (pi->rowCount() + pe->rowCount() == 0) pi->appendRow(makeItem("(no plugins scanned - Options > Plugin Manager)", Category, "hint3"));
    // Presets
    auto* presets = addCategory("Presets");
    QString pd = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets";
    QDir pdir(pd);
    for (auto& sub : pdir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString key = sub.fileName();
        key.replace('_', ':');
        QString name = key;
        if (auto* info = NativeRegistry::instance().info(key)) name = info->name;
        auto* cat = makeItem(name, Category, "cat:preset:" + key);
        presets->appendRow(cat);
        for (auto& f : QDir(sub.absoluteFilePath()).entryInfoList({"*.json"}, QDir::Files))
            cat->appendRow(makeItem(f.completeBaseName(), Preset, "preset:" + f.absoluteFilePath(), f.absoluteFilePath()));
    }
    // Projects
    auto* projs = addCategory("Projects");
    for (auto& r : m_app.recentFiles()) projs->appendRow(makeItem(QFileInfo(r).fileName(), ProjectFile, "file:" + r, r));
    auto* tpl = makeItem("Templates", Category, "cat:templates");
    projs->appendRow(tpl);
    for (auto& t : m_app.templates()) tpl->appendRow(makeItem(QFileInfo(t).completeBaseName(), ProjectFile, "file:" + t, t));
    // favorites: files, plugins, natives
    for (auto& f : m_favorites) {
        if (f.startsWith("file:")) {
            QString p = f.mid(5);
            m_favRoot->appendRow(makeItem(QFileInfo(p).fileName(), AudioFile::isAudioFile(p) ? AudioFile : ProjectFile, f, p));
        } else if (f.startsWith("native:")) {
            QString k = f.mid(7);
            if (auto* info = NativeRegistry::instance().info(k))
                m_favRoot->appendRow(makeItem(info->name, info->kind == ProcessorKind::Instrument ? NativeInstrument : NativeEffect, f, k));
        } else if (f.startsWith("plugin:")) {
            for (auto& p : m_app.pluginDb().plugins())
                if ("plugin:" + p.key() == f) {
                    auto* it = makeItem(p.name, Plugin, f, p.path);
                    it->setData(p.key(), PluginKeyRole);
                    m_favRoot->appendRow(it);
                }
        }
    }
}
// Flat search results: matching project items / instruments / effects / plugins / presets from the normal tree, then matching library files from the index
void BrowserModel::buildSearch() {
    buildTree();
    QStringList words = m_search.toLower().split(' ', Qt::SkipEmptyParts);
    auto matches = [&](const QString& lower) {
        for (auto& w : words)
            if (!lower.contains(w)) return false;
        return true;
    };
    std::vector<QStandardItem*> hits;
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* it) {
        for (int r = 0; r < it->rowCount(); ++r) {
            QStandardItem* c = it->child(r);
            if (!c) continue;
            int kind = c->data(KindRole).toInt();
            if (kind == Folder) continue;
            if (kind != Category && !c->data(KeyRole).toString().startsWith("hint") &&
                matches((c->text() + " " + c->data(TagsRole).toString()).toLower()))
                hits.push_back(c->clone());
            walk(c);
        }
    };
    walk(invisibleRootItem());
    clear();
    setHorizontalHeaderLabels({"Browser"});
    auto* items = addCategory("Results");
    for (auto* h : hits) items->appendRow(h);
    auto* files = addCategory(m_index ? "Files" : "Files (indexing...)");
    int n = 0;
    if (m_index)
        for (auto& f : m_index->files) {
            if (!matches(f.lower)) continue;
            auto* it = makeItem(f.name, f.audio ? AudioFile : ProjectFile, "file:" + f.path, f.path);
            it->setToolTip(f.path);
            files->appendRow(it);
            if (++n >= 2000) {
                files->appendRow(makeItem("(more than 2000 matches - refine the search)", Category, "hint9"));
                break;
            }
        }
    if (hits.empty()) items->appendRow(makeItem("(no matches)", Category, "hint8"));
    if (n == 0 && m_index) files->appendRow(makeItem("(no matching files)", Category, "hint7"));
}
QMimeData* BrowserModel::mimeData(const QModelIndexList& indexes) const {
    auto* md = new QMimeData();
    if (indexes.isEmpty()) return md;
    auto idx = indexes.first();
    int kind = idx.data(KindRole).toInt();
    QString path = idx.data(PathRole).toString();
    if (isRootFolder(idx)) {
        md->setData("application/x-tactum-folder", path.toUtf8());
        return md;
    }
    if (kind == AudioFile || kind == ProjectFile) md->setUrls({QUrl::fromLocalFile(path)});
    QString payload;
    switch (kind) {
    case NativeInstrument:
    case NativeEffect: payload = "native|" + path; break;
    case Plugin: payload = "plugin|" + idx.data(PluginKeyRole).toString(); break;
    case PatternItem: payload = "pattern|" + path; break;
    case LaneItem: payload = "lane|" + path; break;
    case Preset: payload = "preset|" + path; break;
    case AudioFile: payload = "audio|" + path; break;
    default: break;
    }
    if (!payload.isEmpty()) setDropItem(md, payload);
    return md;
}

bool BrowserModel::dropMimeData(const QMimeData* data, Qt::DropAction, int row, int, const QModelIndex& parent) {
    if (!data->hasFormat("application/x-tactum-folder")) return false;
    QString path = QString::fromUtf8(data->data("application/x-tactum-folder"));
    if (!m_dirs.contains(path)) return false;
    // target position among the root library folders (drops on categories clamp to the folder block)
    int targetRow = parent.isValid() ? parent.row() : row;
    if (targetRow < 0) targetRow = rowCount();
    QStringList order;
    for (int r = 0; r < rowCount(); ++r) {
        auto* it = item(r);
        if (it->data(KindRole).toInt() == Folder && r != -1) order << it->data(PathRole).toString();
    }
    int before = 0;
    for (int r = 0; r < std::min(targetRow, rowCount()); ++r)
        if (item(r)->data(KindRole).toInt() == Folder && item(r)->data(PathRole).toString() != path) ++before;
    QStringList dirs = m_dirs;
    dirs.removeAll(path);
    dirs.insert(std::clamp(before, 0, (int)dirs.size()), path);
    // keep folders that don't exist on disk in their old relative place
    QTimer::singleShot(0, this, [this, dirs] { setLibraryDirs(dirs); });
    return true;   // rebuild after the drop completes
}
void BrowserModel::moveLibraryDir(const QString& path, int delta) {
    int i = m_dirs.indexOf(path);
    if (i < 0) return;
    int j = std::clamp(i + delta, 0, (int)m_dirs.size() - 1);
    if (i == j) return;
    QStringList d = m_dirs;
    d.move(i, j);
    setLibraryDirs(d);
}

// ---------------------------------------------------------------- preview bar
PreviewBar::PreviewBar(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    setToolTip("Sample preview: click to seek, right-click to stop. Previews stop when the transport starts.");
    setCursor(Qt::PointingHandCursor);
}
void PreviewBar::tick() {
    bool playing = m_app.engine().previewPlaying();
    double pos = m_app.engine().previewPosition();
    auto b = m_app.previewBuffer();
    bool loading = b && !b->done.load();
    if (playing != m_lastPlaying || loading || (playing && std::fabs(pos - m_lastPos) > 200)) {
        m_lastPlaying = playing;
        m_lastPos = pos;
        update();
    }
}
void PreviewBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(theme::border());
    p.setBrush(theme::surface());
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme::radius(), theme::radius());
    p.setRenderHint(QPainter::Antialiasing, false);
    QString path = m_app.previewPath();
    auto b = m_app.previewBuffer();
    QRect wave = rect().adjusted(2, 16, -2, -2);
    p.setFont(theme::smallFont());
    p.setPen(theme::textDim());
    if (path.isEmpty() || !b) return;   // nothing loaded: an empty bar
    QString name = QFileInfo(path).fileName();
    bool playing = m_app.engine().previewPlaying();
    bool loading = !b->done.load();
    p.setPen(playing ? theme::accent() : theme::textDim());
    p.drawText(QRect(4, 1, width() - 8, 14), Qt::AlignVCenter | Qt::AlignLeft,
        (playing ? "\u25B6 " : "\u25A0 ") + p.fontMetrics().elidedText(name, Qt::ElideMiddle, width() - 110) +
            (loading ? "  (loading)" : ""));
    p.setPen(theme::text());
    p.drawText(QRect(4, 1, width() - 8, 14), Qt::AlignVCenter | Qt::AlignRight, QString("%1 s").arg(b->seconds(), 0, 'f', 2));
    int bins = b->binsReady.load();
    int mid = wave.center().y();
    int hh = wave.height() / 2 - 1;
    double pos = m_app.engine().previewPosition();
    int px = wave.left() + (int)(pos / std::max<int64_t>(1, b->frames) * wave.width());
    for (int x = 0; x < wave.width(); ++x) {
        int bin = (int)((double)x / wave.width() * PreviewBuffer::kBins);
        if (bin >= bins) break;
        float mx = b->maxPk[bin], mn = b->minPk[bin];
        p.setPen(wave.left() + x <= px && playing ? theme::accent() : QColor("#4e8fb8"));
        p.drawLine(wave.left() + x, mid - (int)(mx * hh), wave.left() + x, mid - (int)(mn * hh));
    }
    if (playing) {
        p.setPen(QPen(theme::playhead(), 1.5));
        p.drawLine(px, wave.top(), px, wave.bottom());
    }
}
void PreviewBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        m_app.stopPreview();
        return;
    }
    auto b = m_app.previewBuffer();
    if (!b) return;
    QRect wave = rect().adjusted(2, 16, -2, -2);
    double frac = clamp((double)(e->pos().x() - wave.left()) / std::max(1, wave.width()), 0.0, 1.0);
    m_app.engine().previewSeek(frac * b->frames);
}

// ---------------------------------------------------------------- tree drag (in-app, see InAppDrag.h)
void BrowserTree::startDrag(Qt::DropActions) {
    QModelIndexList idx = selectedIndexes();
    if (idx.isEmpty()) idx << currentIndex();
    if (idx.isEmpty() || !idx.first().isValid()) return;
    std::unique_ptr<QMimeData> md(model()->mimeData(idx));
    if (!md) return;
    runInAppDrag(this, std::move(md), idx.first().data().toString());
    setState(NoState);   // the view never saw the release: reset its own drag bookkeeping
}

// ---------------------------------------------------------------- panel
// '+' at the right end of a hovered sample row: one click adds the sample to the channel rack
static QRect browserAddButtonRect(const QRect& row) { return QRect(row.right() - 21, row.center().y() - 8, 18, 17); }
class BrowserRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        QStyledItemDelegate::paint(p, opt, idx);
        if (idx.data(BrowserModel::KindRole).toInt() != BrowserModel::AudioFile || !(opt.state & QStyle::State_MouseOver)) return;
        QRect r = browserAddButtonRect(opt.rect);
        bool hot = false;
        if (auto* v = qobject_cast<const QAbstractItemView*>(opt.widget)) hot = r.contains(v->viewport()->mapFromGlobal(QCursor::pos()));
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(hot ? theme::accent() : theme::withAlpha(theme::accent(), 150));
        p->drawRoundedRect(QRectF(r), theme::radiusFor(r.height()), theme::radiusFor(r.height()));
        p->setPen(QPen(QColor(0x14, 0x16, 0x1a), 2));
        QPointF c = QRectF(r).center();
        p->drawLine(QPointF(c.x() - 4, c.y()), QPointF(c.x() + 4, c.y()));
        p->drawLine(QPointF(c.x(), c.y() - 4), QPointF(c.x(), c.y() + 4));
        p->restore();
    }
};

BrowserPanel::BrowserPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText("Search samples, plugins, presets, tags...");
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(26);
    m_search->setToolTip("Type to search the whole library (results appear as you type). Right-click the browser to add folders.");
    lay->addWidget(m_search);
    m_tree = new BrowserTree(this);
    m_model = new BrowserModel(app, this);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setDropIndicatorShown(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(14);
    m_tree->setAlternatingRowColors(true);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->installEventFilter(this);
    m_tree->setItemDelegate(new BrowserRowDelegate(m_tree));
    m_tree->setMouseTracking(true);   // hover state for the '+' button
    m_tree->viewport()->setAttribute(Qt::WA_Hover);
    m_tree->viewport()->installEventFilter(this);
    lay->addWidget(m_tree);
    theme::roundedFrame(m_tree);
    m_previewBar = new PreviewBar(app, this);
    lay->addWidget(m_previewBar);
    connect(&app, &App::previewChanged, this, [this](const QString& path) {
        highlightPlaying(path);
        m_previewBar->update();
    });
    m_model->rebuild();
    m_tree->expand(m_model->index(0, 0));
    // search is debounced so fast typing doesn't rebuild the result list on every character
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(120);
    connect(&m_searchTimer, &QTimer::timeout, this, &BrowserPanel::applySearch);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchTimer.start(); });
    connect(m_model, &BrowserModel::indexChanged, this, [this] {
        if (m_model->searching()) applySearch();
    });
    connect(m_tree, &QTreeView::clicked, this, &BrowserPanel::onClicked);
    connect(m_tree, &QTreeView::doubleClicked, this, &BrowserPanel::onDoubleClicked);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &BrowserPanel::onContext);
    connect(m_tree, &QTreeView::expanded, this, &BrowserPanel::onExpanded);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& cur, const QModelIndex&) { onCurrentChanged(cur); });
    connect(&m_app.pluginDb(), &PluginDatabase::changed, this, [this] { rebuildKeepingExpanded(); });
}
void BrowserPanel::applySearch() {
    QString t = m_search->text().trimmed();
    bool was = m_model->searching();
    if (t.isEmpty() && !was) return;
    if (!was) {
        m_savedExpanded.clear();
        std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& idx) {
            for (int r = 0; r < m_model->rowCount(idx); ++r) {
                auto c = m_model->index(r, 0, idx);
                if (m_tree->isExpanded(c)) {
                    m_savedExpanded << c.data(BrowserModel::KeyRole).toString();
                    collect(c);
                }
            }
        };
        collect(QModelIndex());
    }   // remember the tree layout while searching
    m_model->setSearch(t);
    if (m_model->searching()) m_tree->expandAll();
    else {
        std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& idx) {
            for (int r = 0; r < m_model->rowCount(idx); ++r) {
                auto c = m_model->index(r, 0, idx);
                if (m_savedExpanded.contains(c.data(BrowserModel::KeyRole).toString())) {
                    m_tree->expand(c);
                    restore(c);
                }
            }
        };
        restore(QModelIndex());
    }
}
void BrowserPanel::addFolderDialog() {
    QString d = QFileDialog::getExistingDirectory(this, "Add folder to browser");
    if (!d.isEmpty()) {
        auto dirs = m_model->libraryDirs();
        if (!dirs.contains(d)) dirs << d;
        m_model->setLibraryDirs(dirs);
    }
}
// Rebuild the model while keeping the expanded branches (by item key) and the current item
void BrowserPanel::rebuildKeepingExpanded() {
    QStringList expanded;
    std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& idx) {
        for (int r = 0; r < m_model->rowCount(idx); ++r) {
            auto c = m_model->index(r, 0, idx);
            if (m_tree->isExpanded(c)) {
                expanded << c.data(BrowserModel::KeyRole).toString();
                collect(c);
            }
        }
    };
    collect(QModelIndex());
    QString curKey = m_tree->currentIndex().data(BrowserModel::KeyRole).toString();
    int scroll = m_tree->verticalScrollBar()->value();
    m_model->rebuild();
    QModelIndex cur;
    std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& idx) {
        for (int r = 0; r < m_model->rowCount(idx); ++r) {
            auto c = m_model->index(r, 0, idx);
            if (!curKey.isEmpty() && !cur.isValid() && c.data(BrowserModel::KeyRole).toString() == curKey) cur = c;
            if (expanded.contains(c.data(BrowserModel::KeyRole).toString())) {
                m_tree->expand(c);
                restore(c);
            }
        }
    };
    restore(QModelIndex());
    if (m_model->searching()) m_tree->expandAll();
    // put the cursor back where it was (without re-triggering the preview) so Enter-to-load doesn't lose your place
    if (cur.isValid()) {
        m_restoring = true;
        m_tree->setCurrentIndex(cur);
        m_restoring = false;
    }
    m_tree->verticalScrollBar()->setValue(scroll);
}
void BrowserPanel::refresh(uint32_t flags) {
    if (flags & (ChangePatterns | ChangeChannels | ChangeAutomation | ChangeStructure | ChangeMixer | ChangeAudio | ChangePlaylist))
        rebuildKeepingExpanded();
}
// Enter = the double-click action for the current item (FL: Enter loads the browser item)
bool BrowserPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == m_tree->viewport() && (e->type() == QEvent::MouseMove || e->type() == QEvent::ToolTip)) {
        QPoint pos = e->type() == QEvent::MouseMove ? static_cast<QMouseEvent*>(e)->pos() : static_cast<QHelpEvent*>(e)->pos();
        QModelIndex idx = m_tree->indexAt(pos);
        bool sample = idx.data(BrowserModel::KindRole).toInt() == BrowserModel::AudioFile;
        if (e->type() == QEvent::MouseMove) {
            if (sample) m_tree->viewport()->update(m_tree->visualRect(idx));   // light the '+' when the pointer is on it
        } else if (sample && browserAddButtonRect(m_tree->visualRect(idx)).contains(pos)) {
            QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), "Add to the Channel Rack", m_tree);
            return true;
        }
    }
    // the '+' on a hovered sample row adds it to the channel rack: no preview, selection or drag from that press
    if (o == m_tree->viewport() && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick)) {
        auto* me = static_cast<QMouseEvent*>(e);
        QModelIndex idx = m_tree->indexAt(me->pos());
        if (me->button() == Qt::LeftButton && idx.data(BrowserModel::KindRole).toInt() == BrowserModel::AudioFile &&
            browserAddButtonRect(m_tree->visualRect(idx)).contains(me->pos())) {
            if (e->type() == QEvent::MouseButtonPress) addSampleChannel(idx.data(BrowserModel::PathRole).toString(), me->globalPosition().toPoint());
            return true;
        }
    }
    if (o == m_tree && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && m_tree->currentIndex().isValid()) {
            onDoubleClicked(m_tree->currentIndex());
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}
void BrowserPanel::uiTick() { m_previewBar->tick(); }
void BrowserPanel::highlightPlaying(const QString& path) {
    // bold + accent colour on the item that is being previewed (search the whole loaded tree)
    std::function<void(QStandardItem*)> walk = [&](QStandardItem* it) {
        for (int r = 0; r < it->rowCount(); ++r) {
            QStandardItem* c = it->child(r);
            if (!c) continue;
            if (c->data(BrowserModel::KindRole).toInt() == BrowserModel::AudioFile) {
                bool on = !path.isEmpty() && c->data(BrowserModel::PathRole).toString() == path;
                QFont f = c->font();
                if (f.bold() != on) {
                    f.setBold(on);
                    c->setFont(f);
                    c->setForeground(on ? theme::accent() : QColor("#8fd0ff"));
                }
            }
            walk(c);
        }
    };
    walk(m_model->invisibleRootItem());
    m_highlighted = path;
}
void BrowserPanel::onExpanded(const QModelIndex& idx) {
    if (auto* it = m_model->itemFromIndex(idx)) m_model->populateFolder(it);
}
// Any way of landing on a sample (click, arrow keys, search) previews it; folders and categories toggle with a single click
void BrowserPanel::onCurrentChanged(const QModelIndex& idx) {
    if (!idx.isValid() || m_restoring) return;
    int kind = idx.data(BrowserModel::KindRole).toInt();
    if (kind == BrowserModel::AudioFile) {
        m_lastAutoPreview = QDateTime::currentMSecsSinceEpoch();
        m_app.previewFile(idx.data(BrowserModel::PathRole).toString());
    } else if (!m_app.previewPath().isEmpty()) m_app.stopPreview();
}
void BrowserPanel::onClicked(const QModelIndex& idx) {
    int kind = idx.data(BrowserModel::KindRole).toInt();
    if (kind == BrowserModel::Folder || kind == BrowserModel::Category) {
        m_tree->setExpanded(idx, !m_tree->isExpanded(idx));
        return;
    }
    if (kind == BrowserModel::AudioFile && QDateTime::currentMSecsSinceEpoch() - m_lastAutoPreview > 250)
        m_app.previewFile(idx.data(BrowserModel::PathRole).toString());   // re-click restarts the preview
}
QWidget* BrowserPanel::debugViewport() const { return m_tree->viewport(); }
QPoint BrowserPanel::debugAddButton() const {
    std::function<QModelIndex(const QModelIndex&)> find = [&](const QModelIndex& parent) -> QModelIndex {
        for (int r = 0; r < m_model->rowCount(parent); ++r) {
            QModelIndex i = m_model->index(r, 0, parent);
            if (i.data(BrowserModel::KindRole).toInt() == BrowserModel::AudioFile) return i;
            if (QModelIndex f = find(i); f.isValid()) return f;
        }
        return {};
    };
    QModelIndex i = find(QModelIndex());
    if (!i.isValid()) return {-1, -1};
    m_tree->scrollTo(i);
    return browserAddButtonRect(m_tree->visualRect(i)).center();
}
void BrowserPanel::addSampleChannel(const QString& path, const QPoint& globalPos) {
    if (m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), path))
        showToast(QString("Added \u201c%1\u201d to the Channel Rack").arg(QFileInfo(path).completeBaseName()), globalPos);
}
void BrowserPanel::onDoubleClicked(const QModelIndex& idx) {
    int kind = idx.data(BrowserModel::KindRole).toInt();
    QString path = idx.data(BrowserModel::PathRole).toString();
    switch (kind) {
    case BrowserModel::AudioFile: addSampleChannel(path, QCursor::pos()); break;
    case BrowserModel::NativeInstrument:
        m_app.addChannel(channelTypeForKey(path), path);
        break;
    case BrowserModel::NativeEffect: {
        Id sel = m_app.project().selectedChannelId;
        Id track = m_app.project().masterId();
        if (auto* c = m_app.project().channel(sel)) track = c->mixerTrackId;
        m_app.addEffect(track, path);
        break;
    }
    case BrowserModel::Plugin: {
        for (auto& p : m_app.pluginDb().plugins())
            if (p.key() == idx.data(BrowserModel::PluginKeyRole).toString()) {
                if (p.category == "Instrument") m_app.addPluginChannel(p);
                else {
                    Id sel = m_app.project().selectedChannelId;
                    Id track = m_app.project().masterId();
                    if (auto* c = m_app.project().channel(sel)) track = c->mixerTrackId;
                    m_app.addPluginEffect(track, p);
                }
            }
        break;
    }
    case BrowserModel::PatternItem: m_app.selectPattern(path.toULongLong()); break;
    case BrowserModel::ChannelItem:
        m_app.selectChannel(path.toULongLong());
        Q_EMIT openEditor(path.toULongLong());
        break;
    case BrowserModel::ProjectFile:   // projects open, MIDI files import
        if (MidiFile::isMidiFile(path)) m_app.importMidiFile(path);
        else m_app.openProject(path);
        break;
    case BrowserModel::Preset: {
        Id sel = m_app.project().selectedChannelId;
        if (sel) m_app.loadPreset(sel, path);
        break;
    }
    default: break;
    }
}
void BrowserPanel::onContext(const QPoint& pos) {
    auto idx = m_tree->indexAt(pos);
    QMenu menu(this);
    if (idx.isValid()) {
        QString key = idx.data(BrowserModel::KeyRole).toString();
        int kind = idx.data(BrowserModel::KindRole).toInt();
        QString path = idx.data(BrowserModel::PathRole).toString();
        if (kind == BrowserModel::AudioFile) {
            menu.addAction("Preview", [this, path] { m_app.previewFile(path); });
            menu.addAction("Add as sampler channel", [this, path] { addSampleChannel(path, QCursor::pos()); });
            menu.addAction(
                "Add as drum channel", [this, path] { m_app.addChannel(ChannelType::DrumSampler, "native.drumsampler", QString(), path); });
            menu.addAction("Add to playlist at playhead", [this, path] {
                Id track = m_app.firstPlaylistTrack();
                m_app.addAudioClip(track, path, (Tick)m_app.engine().positionTicks());
            });
            menu.addSeparator();
        }
        if (kind == BrowserModel::PatternItem) {
            Id pid = path.toULongLong();
            addPatternActions(&menu, m_app, this, {pid});
            menu.addAction("Delete", [this, pid] { m_app.deletePattern(pid); });
            menu.addSeparator();
        }
        if (kind == BrowserModel::LaneItem) {
            Id lid = path.toULongLong();
            menu.addAction("Delete automation", [this, lid] { m_app.deleteLane(lid); });
        }
        if (kind == BrowserModel::Plugin) {
            menu.addAction(m_app.pluginDb().findByPath(path) && m_app.pluginDb().findByPath(path)->hidden ? "Unhide" : "Hide plugin",
                [this, idx] { m_app.pluginDb().setHidden(idx.data(BrowserModel::PluginKeyRole).toString(), true); });
        }
        if (kind == BrowserModel::Folder && m_model->isRootFolder(idx)) {
            menu.addAction("Move up", [this, path] { m_model->moveLibraryDir(path, -1); });
            menu.addAction("Move down", [this, path] { m_model->moveLibraryDir(path, 1); });
            menu.addAction("Remove from browser", [this, path] {
                auto d = m_model->libraryDirs();
                d.removeAll(path);
                m_model->setLibraryDirs(d);
            });
            menu.addSeparator();
        } else if (kind == BrowserModel::Folder) {
            menu.addAction("Add this folder to the browser", [this, path] {
                auto d = m_model->libraryDirs();
                if (!d.contains(path)) {
                    d << path;
                    m_model->setLibraryDirs(d);
                }
            });
        }
        if (kind != BrowserModel::Category && kind != BrowserModel::Folder) {
            menu.addAction(
                m_model->isFavorite(key) ? "Remove from favorites" : "Add to favorites", [this, key] { m_model->toggleFavorite(key); });
            menu.addAction("Edit tags...", [this, key] {
                bool ok;
                QString t = QInputDialog::getText(
                    this, "Tags", "Space-separated tags:", QLineEdit::Normal, m_model->tags(key).join(' '), &ok);
                if (ok) m_model->setTags(key, t.split(' ', Qt::SkipEmptyParts));
            });
        }
        menu.addSeparator();
    }
    menu.addAction("Add folder to browser...", [this] { addFolderDialog(); });
    menu.addAction("Refresh", [this] {
        m_model->reindex();
        rebuildKeepingExpanded();
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

}   // namespace mt
