#include "ui/ProjectSamplesPanel.h"
#include <QSignalBlocker>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QColorDialog>
#include <QInputDialog>
#include <QPainter>
#include <QListWidget>
#include "ui/InAppDrag.h"
#include "ui/Theme.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMimeData>
#include <QUrl>
#include <QMenu>
#include <QFileDialog>
#include <QFileInfo>
#include <QToolButton>
#include <QHBoxLayout>

namespace mt {

void ProjectItemList::startDrag(Qt::DropActions) {
    auto items = selectedItems(); if (items.isEmpty() && currentItem()) items << currentItem(); if (items.isEmpty()) return;
    // the first item is the payload every drop target understands; all files also travel as URLs (the playlist places them all)
    auto md = std::make_unique<QMimeData>(); QList<QUrl> urls; for (auto* it : items) { QString path = it->data(PathRole).toString(); if (!path.isEmpty()) urls << QUrl::fromLocalFile(path); } if (!urls.isEmpty()) md->setUrls(urls); md->setData("application/x-musicthingy-item", items.first()->data(PayloadRole).toString().toUtf8());
    runInAppDrag(this, std::move(md), items.size() == 1 ? items.first()->text() : QString("%1 items").arg(items.size())); setState(NoState);
}

ProjectSamplesPanel::ProjectSamplesPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    setMinimumWidth(180);   // no maximum: the dock's outer edge resizes this strip alone
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(3, 3, 3, 3); lay->setSpacing(3);
    // three tabs styled like the browser's search box: one segmented control
    auto* tabs = new QWidget(this); tabs->setObjectName("projTabs"); theme::styled(tabs, "QWidget#projTabs { background: @surface@; border: 1px solid @border@; border-radius: 3px; }"); auto* tl = new QHBoxLayout(tabs); tl->setContentsMargins(2, 2, 2, 2); tl->setSpacing(2);
    const char* names[3] = {"Samples", "Patterns", "Autom."}; const char* tips[3] = {"Every audio file this project uses", "The project's patterns: click to select, drag into the playlist", "Automation clips: drag into the playlist"};
    for (int i = 0; i < 3; ++i) { auto* b = new QToolButton(tabs); b->setText(names[i]); b->setToolTip(tips[i]); b->setCheckable(true); b->setAutoExclusive(true); b->setCursor(Qt::PointingHandCursor); b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); theme::styled(b, "QToolButton { color:@textDim@; background: transparent; border: none; border-radius: 2px; padding: 3px 2px; font-size: 8pt; } QToolButton:hover { color:@text@; } QToolButton:checked { color:@text@; background:@raised@; font-weight: bold; }"); tl->addWidget(b); m_tabs[i] = b; connect(b, &QToolButton::clicked, this, [this, i] { setTab(i); }); }
    lay->addWidget(tabs);
    auto* box = new QWidget(this); theme::styled(box, "QWidget#sampleBox { background: @surface@; border: 1px solid @border@; border-radius: 3px; }"); box->setObjectName("sampleBox"); auto* bl = new QVBoxLayout(box); bl->setContentsMargins(2, 2, 2, 2); bl->setSpacing(2);
    m_list = new ProjectItemList(box); m_list->setDragEnabled(true); m_list->setSelectionMode(QAbstractItemView::ExtendedSelection); m_list->setContextMenuPolicy(Qt::CustomContextMenu); m_list->setUniformItemSizes(true); m_list->setTextElideMode(Qt::ElideMiddle); m_list->setFrameShape(QFrame::NoFrame); m_list->setSpacing(1); m_list->setStyleSheet("QListWidget { background: transparent; border: none; }"); m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_list->setWordWrap(false); bl->addWidget(m_list, 1);
    lay->addWidget(box, 1);
    // click: preview a sample / select a pattern; double-click: place at the playhead / rename
    auto activate = [this](QListWidgetItem* it) { if (!it) return; if (m_tab == Samples) m_app.previewFile(it->data(ProjectItemList::PathRole).toString()); else if (m_tab == Patterns) m_app.selectPattern(it->data(ProjectItemList::IdRole).toULongLong()); };
    connect(m_list, &QListWidget::itemClicked, this, activate);
    connect(m_list, &QListWidget::currentItemChanged, this, [this, activate](QListWidgetItem* it, QListWidgetItem*) { if (it && m_list->hasFocus()) activate(it); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; Tick at = (Tick)m_app.engine().positionTicks();
        if (m_tab == Samples) m_app.addAudioClip(track, it->data(ProjectItemList::PathRole).toString(), at); else if (m_tab == Patterns) m_app.addPatternClip(track, it->data(ProjectItemList::IdRole).toULongLong(), at); else m_app.addAutomationClip(track, it->data(ProjectItemList::IdRole).toULongLong(), at, 0); });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &ProjectSamplesPanel::onContext);
    m_list->onDelete = [this] { // Delete key: like the rack, more than one item asks first
        std::vector<Id> ids; QStringList paths; for (auto* x : m_list->selectedItems()) { if (!(x->flags() & Qt::ItemIsEnabled)) continue; ids.push_back(x->data(ProjectItemList::IdRole).toULongLong()); QString p = x->data(ProjectItemList::PathRole).toString(); if (!p.isEmpty()) paths << p; }
        int n = (int)(m_tab == Samples ? paths.size() : ids.size()); if (!n) return; if (n > 1 && QMessageBox::question(this, "Delete", QString("Delete %1 items?").arg(n)) != QMessageBox::Yes) return;
        if (m_tab == Patterns) { for (Id id : ids) m_app.deletePattern(id); } else if (m_tab == Automation) { for (Id id : ids) m_app.deleteLane(id); }
        else { std::vector<std::unique_ptr<Command>> cmds; for (auto& p : paths) for (auto& a : m_app.project().audioSources) if (a.filePath == p) { for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_app.model(), c.id, "")); cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove samples from project", std::move(cmds)); } };
    setTab(QSettings().value("ui/projectTab", 0).toInt());
}
void ProjectSamplesPanel::setTab(int t) { m_tab = std::clamp(t, 0, 2); m_tabs[m_tab]->setChecked(true); QSettings().setValue("ui/projectTab", m_tab); rebuild(); }
void ProjectSamplesPanel::refresh(uint32_t flags) { if (flags & (ChangeAudio | ChangePlaylist | ChangeChannels | ChangeStructure | ChangePatterns | ChangeAutomation | ChangeAll)) rebuild(); }
QListWidgetItem* ProjectSamplesPanel::addItem(const QString& text, const QColor& dot, const QString& tip) { auto* it = new QListWidgetItem(text); QPixmap pm(10, 10); pm.fill(Qt::transparent); { QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(dot); p.drawEllipse(1, 1, 8, 8); } it->setIcon(QIcon(pm)); it->setToolTip(tip); m_list->addItem(it); return it; }
void ProjectSamplesPanel::rebuild() {
    QString keep = m_list->currentItem() ? m_list->currentItem()->data(ProjectItemList::PayloadRole).toString() : QString(); QSet<QString> keepSel; for (auto* x : m_list->selectedItems()) keepSel.insert(x->data(ProjectItemList::PayloadRole).toString()); m_list->clear(); auto& proj = m_app.project();
    if (m_tab == Samples) { // one row per sample: blue = in the playlist, green = on a channel, grey = unused, red = missing
        QSet<QString> seen;
        auto addRow = [&](const QString& name, const QString& path, const QString& used, double secs, bool isMissing, bool isUnused, bool onChannel) { QColor dot = isMissing ? theme::red() : isUnused ? theme::textDim() : onChannel ? theme::green() : QColor("#4e8fb8"); auto* it = addItem(name, dot, QString("%1\n%2%3\n%4").arg(name, used, secs > 0 ? QString("  |  %1 s").arg(secs, 0, 'f', 2) : QString(), isMissing ? "MISSING: " + path : path)); it->setData(ProjectItemList::PathRole, path); it->setData(ProjectItemList::PayloadRole, "audio|" + path); if (isMissing) it->setForeground(theme::red()); else if (isUnused) it->setForeground(theme::textDim()); };
        for (auto& a : proj.audioSources) { if (a.filePath.isEmpty() || seen.contains(a.filePath)) continue; seen.insert(a.filePath); int uses = 0; for (auto& c : proj.clips) if (c.type == ClipType::Audio && c.sourceId == a.id) ++uses; bool miss = !QFileInfo::exists(a.filePath); addRow(a.name.isEmpty() ? QFileInfo(a.filePath).fileName() : a.name, a.filePath, uses ? QString("%1 clip%2 in the playlist").arg(uses).arg(uses > 1 ? "s" : "") : "not in the playlist", a.sampleRate > 0 ? a.frames / a.sampleRate : 0, miss, uses == 0, false); }
        for (auto& c : proj.channels) { if (c.samplePath.isEmpty() || c.samplePath.startsWith("builtin:") || seen.contains(c.samplePath)) continue; seen.insert(c.samplePath); double sr = 0; int ch = 0; int64_t fr = 0; AudioFile::probe(c.samplePath, &sr, &ch, &fr); addRow(QFileInfo(c.samplePath).fileName(), c.samplePath, "channel: " + c.name, sr > 0 ? fr / sr : 0, !QFileInfo::exists(c.samplePath), false, true); }
    } else if (m_tab == Patterns) {
        for (auto& p : proj.patterns) { int notes = 0; for (auto& c : p.channels) notes += (int)c.notes.size(); auto* it = addItem(p.name, p.color, QString("%1\n%2 channel%3, %4 note%5 - click to select, drag into the playlist").arg(p.name).arg(p.channels.size()).arg(p.channels.size() == 1 ? "" : "s").arg(notes).arg(notes == 1 ? "" : "s")); it->setData(ProjectItemList::IdRole, (qulonglong)p.id); it->setData(ProjectItemList::PayloadRole, "pattern|" + QString::number(p.id)); if (p.id == proj.selectedPatternId) { QFont f = it->font(); f.setBold(true); it->setFont(f); } }
    } else {
        for (auto& l : proj.automationLanes) { auto* it = addItem(l.name, l.color, l.name + "\ndrag into the playlist to place an automation clip"); it->setData(ProjectItemList::IdRole, (qulonglong)l.id); it->setData(ProjectItemList::PayloadRole, "lane|" + QString::number(l.id)); }
        if (proj.automationLanes.empty()) { auto* it = new QListWidgetItem("No automation yet - right-click a knob > Create automation clip"); it->setFlags(Qt::NoItemFlags); it->setForeground(theme::textDim()); m_list->addItem(it); }
    }
    { QSignalBlocker b(m_list); for (int i = 0; i < m_list->count(); ++i) { auto* x = m_list->item(i); QString pl = x->data(ProjectItemList::PayloadRole).toString(); if (pl == keep) m_list->setCurrentItem(x, QItemSelectionModel::NoUpdate); if (keepSel.contains(pl)) x->setSelected(true); } }
}
void ProjectSamplesPanel::onContext(const QPoint& pos) {
    auto* it = m_list->itemAt(pos); QMenu m(this);
    if (it && (it->flags() & Qt::ItemIsEnabled) && !it->isSelected()) { m_list->clearSelection(); it->setSelected(true); m_list->setCurrentItem(it); }
    auto sel = m_list->selectedItems(); std::vector<Id> ids; QStringList paths; for (auto* x : sel) { if (!(x->flags() & Qt::ItemIsEnabled)) continue; ids.push_back(x->data(ProjectItemList::IdRole).toULongLong()); QString p = x->data(ProjectItemList::PathRole).toString(); if (!p.isEmpty()) paths << p; }
    int n = (int)(m_tab == Samples ? paths.size() : ids.size()); QString many = n > 1 ? QString(" (%1)").arg(n) : QString();
    auto track = [this] { return m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; };
    if (m_tab == Patterns) { m.addAction("New pattern", [this] { Id id = m_app.addPattern(); m_app.selectPattern(id); }); if (n) m.addSeparator();
        if (n == 1) { Id pid = ids[0]; m.addAction("Select", [this, pid] { m_app.selectPattern(pid); }); m.addAction("Rename...", [this, pid] { bool ok; QString nn = QInputDialog::getText(this, "Rename pattern", "Name:", QLineEdit::Normal, m_app.project().pattern(pid) ? m_app.project().pattern(pid)->name : QString(), &ok); if (ok && !nn.isEmpty()) m_app.renamePattern(pid, nn); }); }
        if (n) { m.addAction("Color..." + many, [this, ids] { QColor c = QColorDialog::getColor(m_app.project().pattern(ids[0]) ? m_app.project().pattern(ids[0])->color : Qt::white, this); if (!c.isValid()) return; for (Id id : ids) m_app.setPatternColor(id, c); }); m.addAction("Clone" + many, [this, ids] { for (Id id : ids) m_app.clonePattern(id); }); m.addAction("Add to playlist at playhead" + many, [this, ids, track] { Id t = track(); Tick at = (Tick)m_app.engine().positionTicks(); for (Id id : ids) m_app.addPatternClip(t, id, at); }); m.addSeparator(); m.addAction("Delete" + many, [this, ids] { for (Id id : ids) m_app.deletePattern(id); }); }
        m.exec(m_list->viewport()->mapToGlobal(pos)); return; }
    if (m_tab == Automation) { if (!n) return; m.addAction("Add to playlist at playhead" + many, [this, ids, track] { Id t = track(); Tick at = (Tick)m_app.engine().positionTicks(); for (Id id : ids) m_app.addAutomationClip(t, id, at, 0); }); m.addSeparator(); m.addAction("Delete automation" + many, [this, ids] { for (Id id : ids) m_app.deleteLane(id); }); m.exec(m_list->viewport()->mapToGlobal(pos)); return; }
    auto removeUnused = [this] { std::vector<std::unique_ptr<Command>> cmds; for (auto& a : m_app.project().audioSources) { bool used = false; for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) used = true; if (!used) cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove unused samples", std::move(cmds)); };
    if (!n) { m.addAction("Remove all unused samples", removeUnused); m.exec(m_list->viewport()->mapToGlobal(pos)); return; }
    if (n > 1) { m.addAction("Add to playlist at playhead" + many, [this, paths, track] { Id t = track(); Tick at = (Tick)m_app.engine().positionTicks(); for (auto& p : paths) m_app.addAudioClip(t, p, at); }); m.addAction("Add as sampler channels" + many, [this, paths] { for (auto& p : paths) m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), p); }); m.addSeparator(); m.addAction("Remove from project" + many, [this, paths] { std::vector<std::unique_ptr<Command>> cmds; for (auto& p : paths) for (auto& a : m_app.project().audioSources) if (a.filePath == p) { for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_app.model(), c.id, "")); cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove samples from project", std::move(cmds)); }); m.addAction("Remove all unused samples", removeUnused); m.exec(m_list->viewport()->mapToGlobal(pos)); return; }
    QString path = paths.first();
    m.addAction("Preview", [this, path] { m_app.previewFile(path); });
    m.addAction("Add to playlist at playhead", [this, path] { Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; m_app.addAudioClip(track, path, (Tick)m_app.engine().positionTicks()); });
    m.addAction("Add as sampler channel", [this, path] { m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), path); });
    if (!QFileInfo::exists(path)) m.addAction("Locate...", [this, path] { QString p = QFileDialog::getOpenFileName(this, "Locate " + QFileInfo(path).fileName()); if (p.isEmpty()) return; for (auto& a : m_app.project().audioSources) if (a.filePath == path) m_app.relinkSample(a.id, p); });
    m.addAction("Remove from project", [this, path] { std::vector<std::unique_ptr<Command>> cmds; for (auto& a : m_app.project().audioSources) if (a.filePath == path) { for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_app.model(), c.id, "")); cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove sample from project", std::move(cmds)); });
    m.addSeparator(); m.addAction("Remove all unused samples", [this] { std::vector<std::unique_ptr<Command>> cmds; for (auto& a : m_app.project().audioSources) { bool used = false; for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) used = true; if (!used) cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove unused samples", std::move(cmds)); });
    m.exec(m_list->viewport()->mapToGlobal(pos));
}

} // namespace mt
