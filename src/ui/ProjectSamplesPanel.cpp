#include "ui/ProjectSamplesPanel.h"
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

QMimeData* SampleTable::mimeData(const QList<QTableWidgetItem*>& items) const {
    auto* md = new QMimeData(); if (items.isEmpty()) return md; QString path = items.first()->data(Qt::UserRole).toString();
    md->setUrls({QUrl::fromLocalFile(path)}); md->setData("application/x-musicthingy-item", ("audio|" + path).toUtf8()); return md;
}

ProjectSamplesPanel::ProjectSamplesPanel(App& app, QWidget* parent) : QWidget(parent), m_app(app) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(4, 4, 4, 4); lay->setSpacing(4);
    auto* top = new QHBoxLayout(); m_summary = new QLabel(this); m_summary->setStyleSheet("color:#8a919a;"); top->addWidget(m_summary, 1);
    auto* purge = new QToolButton(this); purge->setText("Remove unused"); purge->setToolTip("Forget samples that are not used by any clip or channel"); top->addWidget(purge); lay->addLayout(top);
    m_table = new SampleTable(this); m_table->setColumnCount(4); m_table->setHorizontalHeaderLabels({"Sample", "Used", "Length", "Location"}); m_table->horizontalHeader()->setStretchLastSection(true); m_table->verticalHeader()->setVisible(false); m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setDragEnabled(true); m_table->setDragDropMode(QAbstractItemView::DragOnly); m_table->setContextMenuPolicy(Qt::CustomContextMenu); m_table->setShowGrid(false); m_table->setAlternatingRowColors(true); m_table->verticalHeader()->setDefaultSectionSize(20);
    lay->addWidget(m_table, 1);
    auto* hint = new QLabel("Click to preview - drag a row onto the Playlist to place it again - double-click adds it at the playhead", this); hint->setStyleSheet("color:#6a717a; font-size:8pt;"); lay->addWidget(hint);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int r, int) { if (auto* it = m_table->item(r, 0)) m_app.previewFile(it->data(Qt::UserRole).toString()); });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { if (auto* it = m_table->item(r, 0)) { QString p = it->data(Qt::UserRole).toString(); Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; m_app.addAudioClip(track, p, m_app.snapTick((Tick)m_app.engine().positionTicks())); } });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, &ProjectSamplesPanel::onContext);
    connect(purge, &QToolButton::clicked, this, [this] { std::vector<std::unique_ptr<Command>> cmds; for (auto& a : m_app.project().audioSources) { bool used = false; for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) used = true; if (!used) cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove unused samples", std::move(cmds)); });
    rebuild();
}
void ProjectSamplesPanel::refresh(uint32_t flags) { if (flags & (ChangeAudio | ChangePlaylist | ChangeChannels | ChangeStructure | ChangeAll)) rebuild(); }
void ProjectSamplesPanel::rebuild() {
    m_table->setRowCount(0); int total = 0, unused = 0, missing = 0;
    auto addRow = [&](const QString& name, const QString& path, const QString& used, double secs, bool isMissing, bool isUnused) { int r = m_table->rowCount(); m_table->insertRow(r); auto* n = new QTableWidgetItem(name); n->setData(Qt::UserRole, path); if (isMissing) n->setForeground(theme::red()); else if (isUnused) n->setForeground(theme::textDim()); m_table->setItem(r, 0, n); m_table->setItem(r, 1, new QTableWidgetItem(used)); m_table->setItem(r, 2, new QTableWidgetItem(secs > 0 ? QString("%1 s").arg(secs, 0, 'f', 2) : "")); auto* loc = new QTableWidgetItem(isMissing ? "MISSING: " + path : path); loc->setToolTip(path); m_table->setItem(r, 3, loc); ++total; if (isUnused) ++unused; if (isMissing) ++missing; };
    QSet<QString> seen;
    for (auto& a : m_app.project().audioSources) { if (a.filePath.isEmpty() || seen.contains(a.filePath)) continue; seen.insert(a.filePath); int uses = 0; for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) ++uses; bool miss = !QFileInfo::exists(a.filePath); addRow(a.name.isEmpty() ? QFileInfo(a.filePath).fileName() : a.name, a.filePath, uses ? QString("%1 clip%2").arg(uses).arg(uses > 1 ? "s" : "") : "not in playlist", a.sampleRate > 0 ? a.frames / a.sampleRate : 0, miss, uses == 0); }
    for (auto& c : m_app.project().channels) { if (c.samplePath.isEmpty() || c.samplePath.startsWith("builtin:") || seen.contains(c.samplePath)) continue; seen.insert(c.samplePath); double sr = 0; int ch = 0; int64_t fr = 0; AudioFile::probe(c.samplePath, &sr, &ch, &fr); addRow(QFileInfo(c.samplePath).fileName(), c.samplePath, "channel: " + c.name, sr > 0 ? fr / sr : 0, !QFileInfo::exists(c.samplePath), false); }
    m_table->resizeColumnsToContents(); m_table->horizontalHeader()->setStretchLastSection(true);
    m_summary->setText(QString("%1 sample%2 in this project - %3 not in the playlist%4").arg(total).arg(total == 1 ? "" : "s").arg(unused).arg(missing ? QString(", %1 missing").arg(missing) : ""));
}
void ProjectSamplesPanel::onContext(const QPoint& pos) {
    auto* it = m_table->itemAt(pos); if (!it) return; int r = it->row(); QString path = m_table->item(r, 0)->data(Qt::UserRole).toString(); QMenu m(this);
    m.addAction("Preview", [this, path] { m_app.previewFile(path); });
    m.addAction("Add to playlist at playhead", [this, path] { Id track = m_app.project().playlistTracks.empty() ? m_app.addPlaylistTrack() : m_app.project().playlistTracks.front().id; m_app.addAudioClip(track, path, m_app.snapTick((Tick)m_app.engine().positionTicks())); });
    m.addAction("Add as sampler channel", [this, path] { m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), path); });
    if (!QFileInfo::exists(path)) m.addAction("Locate...", [this, path] { QString p = QFileDialog::getOpenFileName(this, "Locate " + QFileInfo(path).fileName()); if (p.isEmpty()) return; for (auto& a : m_app.project().audioSources) if (a.filePath == path) m_app.relinkSample(a.id, p); });
    m.addAction("Remove from project", [this, path] { std::vector<std::unique_ptr<Command>> cmds; for (auto& a : m_app.project().audioSources) if (a.filePath == path) { for (auto& c : m_app.project().clips) if (c.type == ClipType::Audio && c.sourceId == a.id) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_app.model(), c.id, "")); cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, "")); } if (!cmds.empty()) m_app.model().macro("Remove sample from project", std::move(cmds)); });
    m.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace mt
