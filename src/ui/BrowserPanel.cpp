#include "ui/BrowserPanel.h"
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

namespace mt {

// ---------------------------------------------------------------- model
BrowserModel::BrowserModel(App& app, QObject* parent) : QStandardItemModel(parent), m_app(app) {
    QSettings s; m_favorites = QSet<QString>(s.value("browser/favorites").toStringList().begin(), s.value("browser/favorites").toStringList().end());
    auto tagMap = s.value("browser/tags").toMap(); for (auto it = tagMap.begin(); it != tagMap.end(); ++it) m_tags[it.key()] = it.value().toStringList();
    m_dirs = s.value("browser/dirs").toStringList(); if (m_dirs.isEmpty()) { m_dirs << QStandardPaths::writableLocation(QStandardPaths::MusicLocation); for (auto& d : {"/usr/share/sounds", "/usr/share/hydrogen/data/drumkits", "/usr/share/sounds/sf2"}) if (QDir(d).exists()) m_dirs << d; }
}
void BrowserModel::setLibraryDirs(const QStringList& d) { m_dirs = d; QSettings().setValue("browser/dirs", d); rebuild(); }
void BrowserModel::toggleFavorite(const QString& key) { if (m_favorites.contains(key)) m_favorites.remove(key); else m_favorites.insert(key); QSettings().setValue("browser/favorites", QStringList(m_favorites.begin(), m_favorites.end())); rebuild(); }
void BrowserModel::setTags(const QString& key, const QStringList& t) { if (t.isEmpty()) m_tags.remove(key); else m_tags[key] = t; QVariantMap m; for (auto it = m_tags.begin(); it != m_tags.end(); ++it) m[it.key()] = it.value(); QSettings().setValue("browser/tags", m); rebuild(); }

QStandardItem* BrowserModel::makeItem(const QString& text, Kind kind, const QString& key, const QString& path) {
    auto* it = new QStandardItem(text); it->setData(path, PathRole); it->setData((int)kind, KindRole); it->setData(key, KeyRole); it->setData(m_favorites.contains(key), FavRole); it->setData(m_tags.value(key).join(' '), TagsRole);
    if (m_favorites.contains(key)) it->setText("★ " + text);
    QColor c = theme::text(); switch (kind) { case Folder: case Category: c = theme::textDim(); break; case AudioFile: c = QColor("#8fd0ff"); break; case NativeInstrument: c = QColor("#c9a2ff"); break; case NativeEffect: c = QColor("#a2ffb8"); break; case Plugin: c = QColor("#ffd27f"); break; case Preset: c = QColor("#ffb6a2"); break; case PatternItem: c = theme::accent(); break; default: break; }
    it->setForeground(c); it->setEditable(false); return it;
}
QStandardItem* BrowserModel::addCategory(const QString& name, Kind kind) { auto* it = makeItem(name, kind, "cat:" + name); QFont f = theme::boldFont(); it->setFont(f); appendRow(it); return it; }
QStandardItem* BrowserModel::addFolder(QStandardItem* parent, const QString& path, const QString& label) { auto* it = makeItem(label.isEmpty() ? QFileInfo(path).fileName() : label, Folder, "dir:" + path, path); it->setData(false, LoadedRole); it->appendRow(new QStandardItem("..."));  parent->appendRow(it); return it; }
void BrowserModel::populateFolder(QStandardItem* item) {
    if (item->data(KindRole).toInt() != Folder || item->data(LoadedRole).toBool()) return;
    item->removeRows(0, item->rowCount()); item->setData(true, LoadedRole);
    QDir d(item->data(PathRole).toString());
    for (auto& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) addFolder(item, fi.absoluteFilePath());
    for (auto& fi : d.entryInfoList(QDir::Files, QDir::Name)) { QString p = fi.absoluteFilePath(); if (AudioFile::isAudioFile(p)) item->appendRow(makeItem(fi.fileName(), AudioFile, "file:" + p, p)); else if (fi.suffix() == "mtp") item->appendRow(makeItem(fi.fileName(), ProjectFile, "file:" + p, p)); }
}
void BrowserModel::rebuild() {
    clear(); setHorizontalHeaderLabels({"Browser"});
    // Current project
    auto* proj = addCategory("Current project");
    auto* pats = makeItem("Patterns", Category, "cat:patterns"); proj->appendRow(pats); for (auto& p : m_app.project().patterns) { auto* it = makeItem(p.name, PatternItem, "pattern:" + QString::number(p.id), QString::number(p.id)); it->setForeground(p.color); pats->appendRow(it); }
    auto* chans = makeItem("Channels", Category, "cat:channels"); proj->appendRow(chans); for (auto& c : m_app.project().channels) { auto* it = makeItem(c.name, ChannelItem, "channel:" + QString::number(c.id), QString::number(c.id)); it->setForeground(c.color); chans->appendRow(it); }
    auto* lanes = makeItem("Automation", Category, "cat:lanes"); proj->appendRow(lanes); for (auto& l : m_app.project().automationLanes) lanes->appendRow(makeItem(l.name, LaneItem, "lane:" + QString::number(l.id), QString::number(l.id)));
    auto* mix = makeItem("Mixer tracks", Category, "cat:mixer"); proj->appendRow(mix); for (auto& t : m_app.project().mixerTracks) { auto* it = makeItem(t.name + (t.effects.empty() ? "" : QString("  (%1 fx)").arg(t.effects.size())), MixerItem, "mixer:" + QString::number(t.id), QString::number(t.id)); mix->appendRow(it); }
    // Favorites
    m_favRoot = addCategory("Favorites");
    // Samples / loops / one-shots: folders from the library
    auto* samples = addCategory("Samples"); for (auto& d : m_dirs) if (QDir(d).exists()) addFolder(samples, d);
    auto* loops = addCategory("Loops"); auto* ones = addCategory("One-shots");
    for (auto& d : m_dirs) { QDir dir(d); for (auto& sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) { QString n = sub.fileName().toLower(); if (n.contains("loop")) addFolder(loops, sub.absoluteFilePath()); if (n.contains("one") || n.contains("shot") || n.contains("drum") || n.contains("kick") || n.contains("snare")) addFolder(ones, sub.absoluteFilePath()); } }
    if (loops->rowCount() == 0) loops->appendRow(makeItem("(add folders containing 'loop' to your library)", Category, "hint1"));
    if (ones->rowCount() == 0) ones->appendRow(makeItem("(add folders named 'drums', 'one-shots'... to your library)", Category, "hint2"));
    // Instruments & effects
    auto* inst = addCategory("Instruments"); auto* fx = addCategory("Effects"); std::map<QString, QStandardItem*> fxCats;
    for (auto& i : NativeRegistry::instance().all()) { if (i.kind == ProcessorKind::Instrument) { inst->appendRow(makeItem(i.name, NativeInstrument, "native:" + i.key, i.key)); } else { auto& cat = fxCats[i.category]; if (!cat) { cat = makeItem(i.category, Category, "cat:fx:" + i.category); fx->appendRow(cat); } cat->appendRow(makeItem(i.name, NativeEffect, "native:" + i.key, i.key)); } }
    // Plugins
    auto* plugs = addCategory("Plugins"); auto* pi = makeItem("Instruments", Category, "cat:plugins:inst"); auto* pe = makeItem("Effects", Category, "cat:plugins:fx"); auto* pf = makeItem("Failed", Category, "cat:plugins:failed"); plugs->appendRow(pi); plugs->appendRow(pe); plugs->appendRow(pf);
    for (auto& p : m_app.pluginDb().plugins()) { if (p.hidden) continue; if (p.failed) { auto* it = makeItem(p.name + " - " + p.failReason, Plugin, "plugin:" + p.key(), p.path); it->setForeground(theme::red()); it->setDragEnabled(false); pf->appendRow(it); continue; } auto* it = makeItem(QString("%1 (%2) [%3]").arg(p.name, p.vendor, p.format.toUpper()), Plugin, "plugin:" + p.key(), p.path); it->setData(p.key(), PluginKeyRole); (p.category == "Instrument" ? pi : pe)->appendRow(it); }
    if (pi->rowCount() + pe->rowCount() == 0) pi->appendRow(makeItem("(no plugins scanned - Options > Plugin Manager)", Category, "hint3"));
    // Presets
    auto* presets = addCategory("Presets"); QString pd = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets"; QDir pdir(pd);
    for (auto& sub : pdir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) { QString key = sub.fileName(); key.replace('_', ':'); QString name = key; if (auto* info = NativeRegistry::instance().info(key)) name = info->name; auto* cat = makeItem(name, Category, "cat:preset:" + key); presets->appendRow(cat); for (auto& f : QDir(sub.absoluteFilePath()).entryInfoList({"*.json"}, QDir::Files)) cat->appendRow(makeItem(f.completeBaseName(), Preset, "preset:" + f.absoluteFilePath(), f.absoluteFilePath())); }
    // Projects
    auto* projs = addCategory("Projects"); for (auto& r : m_app.recentFiles()) projs->appendRow(makeItem(QFileInfo(r).fileName(), ProjectFile, "file:" + r, r)); auto* tpl = makeItem("Templates", Category, "cat:templates"); projs->appendRow(tpl); for (auto& t : m_app.templates()) tpl->appendRow(makeItem(QFileInfo(t).completeBaseName(), ProjectFile, "file:" + t, t));
    // User library (extra dirs beyond the first)
    auto* user = addCategory("User Library"); for (int i = 1; i < m_dirs.size(); ++i) if (QDir(m_dirs[i]).exists()) addFolder(user, m_dirs[i]); if (user->rowCount() == 0) user->appendRow(makeItem("(right-click to add folders)", Category, "hint4"));
    // favorites: files, plugins, natives
    for (auto& f : m_favorites) { if (f.startsWith("file:")) { QString p = f.mid(5); m_favRoot->appendRow(makeItem(QFileInfo(p).fileName(), AudioFile::isAudioFile(p) ? AudioFile : ProjectFile, f, p)); } else if (f.startsWith("native:")) { QString k = f.mid(7); if (auto* info = NativeRegistry::instance().info(k)) m_favRoot->appendRow(makeItem(info->name, info->kind == ProcessorKind::Instrument ? NativeInstrument : NativeEffect, f, k)); } else if (f.startsWith("plugin:")) { for (auto& p : m_app.pluginDb().plugins()) if ("plugin:" + p.key() == f) { auto* it = makeItem(p.name, Plugin, f, p.path); it->setData(p.key(), PluginKeyRole); m_favRoot->appendRow(it); } } }
}
QMimeData* BrowserModel::mimeData(const QModelIndexList& indexes) const {
    auto* md = new QMimeData(); if (indexes.isEmpty()) return md; auto idx = indexes.first(); int kind = idx.data(KindRole).toInt(); QString path = idx.data(PathRole).toString();
    if (kind == AudioFile || kind == ProjectFile) md->setUrls({QUrl::fromLocalFile(path)});
    QString payload; switch (kind) { case NativeInstrument: case NativeEffect: payload = "native|" + path; break; case Plugin: payload = "plugin|" + idx.data(PluginKeyRole).toString(); break; case PatternItem: payload = "pattern|" + path; break; case LaneItem: payload = "lane|" + path; break; case Preset: payload = "preset|" + path; break; case AudioFile: payload = "audio|" + path; break; default: break; }
    if (!payload.isEmpty()) md->setData("application/x-musicthingy-item", payload.toUtf8());
    return md;
}

// ---------------------------------------------------------------- filter
bool BrowserFilter::filterAcceptsRow(int row, const QModelIndex& parent) const {
    QString pat = filterRegularExpression().pattern(); if (pat.isEmpty()) return true;
    QModelIndex idx = sourceModel()->index(row, 0, parent);
    QString text = idx.data().toString() + " " + idx.data(BrowserModel::TagsRole).toString();
    if (text.contains(pat, Qt::CaseInsensitive)) return true;
    int kind = idx.data(BrowserModel::KindRole).toInt();
    if (kind == BrowserModel::Folder && m_model && !idx.data(BrowserModel::LoadedRole).toBool()) { m_model->populateFolder(m_model->itemFromIndex(idx)); }
    for (int r = 0; r < sourceModel()->rowCount(idx); ++r) if (filterAcceptsRow(r, idx)) return true;
    return false;
}

// ---------------------------------------------------------------- panel
BrowserPanel::BrowserPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(2, 2, 2, 2); lay->setSpacing(2);
    auto* top = new QHBoxLayout(); m_search = new QLineEdit(this); m_search->setPlaceholderText("Search samples, plugins, presets, tags..."); m_search->setClearButtonEnabled(true); top->addWidget(m_search);
    auto* prev = new QToolButton(this); prev->setText("♫"); prev->setCheckable(true); prev->setChecked(true); prev->setToolTip("Preview audio files on click"); top->addWidget(prev); connect(prev, &QToolButton::toggled, this, [this](bool b) { m_previewEnabled = b; });
    auto* addDir = new QToolButton(this); addDir->setText("+"); addDir->setToolTip("Add a folder to the library"); top->addWidget(addDir);
    lay->addLayout(top);
    m_tree = new QTreeView(this); m_model = new BrowserModel(app, this); m_filter = new BrowserFilter(this); m_filter->setModelRef(m_model); m_filter->setSourceModel(m_model); m_filter->setRecursiveFilteringEnabled(false);
    m_tree->setModel(m_filter); m_tree->setHeaderHidden(true); m_tree->setDragEnabled(true); m_tree->setDragDropMode(QAbstractItemView::DragOnly); m_tree->setContextMenuPolicy(Qt::CustomContextMenu); m_tree->setUniformRowHeights(true); m_tree->setIndentation(14); m_tree->setAlternatingRowColors(true);
    lay->addWidget(m_tree);
    m_model->rebuild(); m_tree->expand(m_filter->mapFromSource(m_model->index(0, 0)));
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) { m_filter->setFilterRegularExpression(QRegularExpression::escape(t)); if (!t.isEmpty()) m_tree->expandAll(); });
    connect(m_tree, &QTreeView::clicked, this, &BrowserPanel::onClicked); connect(m_tree, &QTreeView::doubleClicked, this, &BrowserPanel::onDoubleClicked); connect(m_tree, &QTreeView::customContextMenuRequested, this, &BrowserPanel::onContext); connect(m_tree, &QTreeView::expanded, this, &BrowserPanel::onExpanded);
    connect(addDir, &QToolButton::clicked, this, [this] { QString d = QFileDialog::getExistingDirectory(this, "Add library folder"); if (!d.isEmpty()) { auto dirs = m_model->libraryDirs(); dirs << d; m_model->setLibraryDirs(dirs); } });
    connect(&m_app.pluginDb(), &PluginDatabase::changed, this, [this] { m_model->rebuild(); });
}
void BrowserPanel::refresh(uint32_t flags) { if (flags & (ChangePatterns | ChangeChannels | ChangeAutomation | ChangeStructure | ChangeMixer)) { QStringList expanded; std::function<void(const QModelIndex&)> collect = [&](const QModelIndex& idx) { for (int r = 0; r < m_filter->rowCount(idx); ++r) { auto c = m_filter->index(r, 0, idx); if (m_tree->isExpanded(c)) { expanded << c.data(BrowserModel::KeyRole).toString(); collect(c); } } }; collect(QModelIndex()); m_model->rebuild(); std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& idx) { for (int r = 0; r < m_filter->rowCount(idx); ++r) { auto c = m_filter->index(r, 0, idx); if (expanded.contains(c.data(BrowserModel::KeyRole).toString())) { m_tree->expand(c); restore(c); } } }; restore(QModelIndex()); } }
void BrowserPanel::onExpanded(const QModelIndex& idx) { auto src = m_filter->mapToSource(idx); if (auto* it = m_model->itemFromIndex(src)) m_model->populateFolder(it); }
void BrowserPanel::onClicked(const QModelIndex& idx) { int kind = idx.data(BrowserModel::KindRole).toInt(); if (kind == BrowserModel::AudioFile && m_previewEnabled) m_app.previewFile(idx.data(BrowserModel::PathRole).toString()); }
void BrowserPanel::onDoubleClicked(const QModelIndex& idx) {
    int kind = idx.data(BrowserModel::KindRole).toInt(); QString path = idx.data(BrowserModel::PathRole).toString();
    switch (kind) {
    case BrowserModel::AudioFile: m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), path); break;
    case BrowserModel::NativeInstrument: m_app.addChannel(path == "native.drumsampler" ? ChannelType::DrumSampler : path == "native.midiout" ? ChannelType::MidiOut : path == "native.tonegen" ? ChannelType::AudioGenerator : path == "native.sampler" ? ChannelType::Sampler : ChannelType::Synth, path); break;
    case BrowserModel::NativeEffect: { Id sel = m_app.project().selectedChannelId; Id track = m_app.project().masterId(); if (auto* c = m_app.project().channel(sel)) track = c->mixerTrackId; m_app.addEffect(track, path); break; }
    case BrowserModel::Plugin: { for (auto& p : m_app.pluginDb().plugins()) if (p.key() == idx.data(BrowserModel::PluginKeyRole).toString()) { if (p.category == "Instrument") m_app.addPluginChannel(p); else { Id sel = m_app.project().selectedChannelId; Id track = m_app.project().masterId(); if (auto* c = m_app.project().channel(sel)) track = c->mixerTrackId; m_app.addPluginEffect(track, p); } } break; }
    case BrowserModel::PatternItem: m_app.selectPattern(path.toULongLong()); break;
    case BrowserModel::ChannelItem: m_app.selectChannel(path.toULongLong()); Q_EMIT openEditor(path.toULongLong()); break;
    case BrowserModel::ProjectFile: m_app.openProject(path); break;
    case BrowserModel::Preset: { Id sel = m_app.project().selectedChannelId; if (sel) m_app.loadPreset(sel, path); break; }
    default: break;
    }
}
void BrowserPanel::onContext(const QPoint& pos) {
    auto idx = m_tree->indexAt(pos); QMenu menu(this);
    if (idx.isValid()) {
        QString key = idx.data(BrowserModel::KeyRole).toString(); int kind = idx.data(BrowserModel::KindRole).toInt(); QString path = idx.data(BrowserModel::PathRole).toString();
        if (kind == BrowserModel::AudioFile) { menu.addAction("Preview", [this, path] { m_app.previewFile(path); }); menu.addAction("Add as sampler channel", [this, path] { m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), path); }); menu.addAction("Add as drum channel", [this, path] { m_app.addChannel(ChannelType::DrumSampler, "native.drumsampler", QString(), path); }); menu.addAction("Add to playlist at playhead", [this, path] { Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; m_app.addAudioClip(track, path, m_app.snapTick((Tick)m_app.engine().positionTicks())); }); menu.addSeparator(); }
        if (kind == BrowserModel::PatternItem) { Id pid = path.toULongLong(); menu.addAction("Rename", [this, pid] { bool ok; QString n = QInputDialog::getText(this, "Rename pattern", "Name:", QLineEdit::Normal, m_app.project().pattern(pid)->name, &ok); if (ok) m_app.renamePattern(pid, n); }); menu.addAction("Clone", [this, pid] { m_app.clonePattern(pid); }); menu.addAction("Delete", [this, pid] { m_app.deletePattern(pid); }); menu.addSeparator(); }
        if (kind == BrowserModel::LaneItem) { Id lid = path.toULongLong(); menu.addAction("Delete automation", [this, lid] { m_app.deleteLane(lid); }); }
        if (kind == BrowserModel::Plugin) { menu.addAction(m_app.pluginDb().findByPath(path) && m_app.pluginDb().findByPath(path)->hidden ? "Unhide" : "Hide plugin", [this, idx] { m_app.pluginDb().setHidden(idx.data(BrowserModel::PluginKeyRole).toString(), true); }); }
        if (kind == BrowserModel::Folder) { menu.addAction("Remove from library", [this, path] { auto d = m_model->libraryDirs(); d.removeAll(path); m_model->setLibraryDirs(d); }); }
        if (kind != BrowserModel::Category && kind != BrowserModel::Folder) { menu.addAction(m_model->isFavorite(key) ? "Remove from favorites" : "Add to favorites", [this, key] { m_model->toggleFavorite(key); }); menu.addAction("Edit tags...", [this, key] { bool ok; QString t = QInputDialog::getText(this, "Tags", "Space-separated tags:", QLineEdit::Normal, m_model->tags(key).join(' '), &ok); if (ok) m_model->setTags(key, t.split(' ', Qt::SkipEmptyParts)); }); }
        menu.addSeparator();
    }
    menu.addAction("Add library folder...", [this] { QString d = QFileDialog::getExistingDirectory(this, "Add library folder"); if (!d.isEmpty()) { auto dirs = m_model->libraryDirs(); dirs << d; m_model->setLibraryDirs(dirs); } });
    menu.addAction("Refresh", [this] { m_model->rebuild(); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

} // namespace mt
