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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QCursor>
#include "ui/widgets/Controls.h"

namespace mt {

void ProjectItemList::startDrag(Qt::DropActions) {
    auto items = selectedItems();
    if (items.isEmpty() && currentItem()) items << currentItem();
    if (items.isEmpty()) return;
    // the first item is the payload every drop target understands; all files also travel as URLs (the playlist places them all)
    auto md = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    for (auto* it : items) {
        QString path = it->data(PathRole).toString();
        if (!path.isEmpty()) urls << QUrl::fromLocalFile(path);
    }
    if (!urls.isEmpty()) md->setUrls(urls);
    setDropItem(md.get(), items.first()->data(PayloadRole).toString());
    m_draggingOut = true;   // in-app drag events carry no source: this is how our own rows are told apart from browser drops
    runInAppDrag(this, std::move(md), items.size() == 1 ? items.first()->text() : QString("%1 items").arg(items.size()));
    m_draggingOut = false;
    setState(NoState);
}

ProjectSamplesPanel::ProjectSamplesPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    setMinimumWidth(180);   // no maximum: the dock's outer edge resizes this strip alone
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);   // same margins as the browser so the header lines up with its search field
    // three tabs styled like the browser's search box: one segmented control
    auto* tabs = new QWidget(this);
    tabs->setObjectName("projTabs");
    theme::styled(tabs, "QWidget#projTabs { background: @surface@; border: 1px solid @border@; border-radius: 3px; }");
    tabs->setFixedHeight(26);
    auto* tl = new QHBoxLayout(tabs);
    tl->setContentsMargins(2, 2, 2, 2);
    tl->setSpacing(2);
    const char* names[3] = {"Samples", "Patterns", "Autom."};
    const char* tips[3] = {"Every audio file this project uses", "The project's patterns: click to select, drag into the playlist",
        "Automation clips: drag into the playlist"};
    for (int i = 0; i < 3; ++i) {
        auto* b = new QToolButton(tabs);
        b->setText(names[i]);
        b->setToolTip(tips[i]);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        theme::styled(b,
            "QToolButton { color:@textDim@; background: transparent; border: none; border-radius: 3px; padding: 0 2px; font-size: 8pt; } QToolButton:hover { color:@text@; } QToolButton:checked { color:@text@; background:@raised@; font-weight: bold; }");
        tl->addWidget(b);
        m_tabs[i] = b;
        connect(b, &QToolButton::clicked, this, [this, i] { setTab(i); });
    }
    lay->addWidget(tabs);
    auto* box = new QWidget(this);
    theme::styled(box, "QWidget#sampleBox { background: @surface@; border: 1px solid @border@; border-radius: 3px; }");
    box->setObjectName("sampleBox");
    auto* bl = new QVBoxLayout(box);
    bl->setContentsMargins(2, 2, 2, 2);
    bl->setSpacing(2);
    m_list = new ProjectItemList(box);
    m_list->setDragEnabled(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setUniformItemSizes(true);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSpacing(1);
    m_list->setStyleSheet("QListWidget { background: transparent; border: none; }");
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setWordWrap(false);
    bl->addWidget(m_list, 1);
    lay->addWidget(box, 1);
    // click: select a pattern (samples stay silent - right-click > Preview plays one); double-click: place at the playhead
    auto activate = [this](QListWidgetItem* it) {
        if (it && m_tab == Patterns) m_app.selectPattern(it->data(ProjectItemList::IdRole).toULongLong());
    };
    connect(m_list, &QListWidget::itemClicked, this, activate);
    connect(m_list, &QListWidget::currentItemChanged, this, [this, activate](QListWidgetItem* it, QListWidgetItem*) {
        if (it && m_list->hasFocus()) activate(it);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (m_tab == Samples) {   // a sample opens its details, as from the playlist; it is not placed
            openDetails(it->data(ProjectItemList::PathRole).toString());
            return;
        }
        Id track = m_app.firstPlaylistTrack();
        Tick at = (Tick)m_app.engine().positionTicks();
        if (m_tab == Patterns) m_app.addPatternClip(track, it->data(ProjectItemList::IdRole).toULongLong(), at);
        else m_app.addAutomationClip(track, it->data(ProjectItemList::IdRole).toULongLong(), at, 0);
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &ProjectSamplesPanel::onContext);
    m_list->setAcceptDrops(true);
    m_list->onDrop = [this](const QMimeData* md) { return takeDrop(md); };
    m_list->onDelete = [this] {   // Delete key: like the rack, more than one item asks first
        auto [ids, paths] = selection();
        int n = (int)(m_tab == Samples ? paths.size() : ids.size());
        if (!n) return;
        if (n > 1 && QMessageBox::question(this, "Delete", QString("Delete %1 items?").arg(n)) != QMessageBox::Yes) return;
        if (m_tab == Patterns) {
            for (Id id : ids) m_app.deletePattern(id);
        } else if (m_tab == Automation) {
            for (Id id : ids) m_app.deleteLane(id);
        } else removeSamples(paths);
    };
    setTab(QSettings().value("ui/projectTab", 0).toInt());
}
void ProjectSamplesPanel::setTab(int t) {
    m_tab = std::clamp(t, 0, 2);
    m_tabs[m_tab]->setChecked(true);
    QSettings().setValue("ui/projectTab", m_tab);
    rebuild();
}
void ProjectSamplesPanel::refresh(uint32_t flags) {
    if (flags & (ChangeAudio | ChangePlaylist | ChangeChannels | ChangeStructure | ChangePatterns | ChangeAutomation | ChangeAll))
        rebuild();
}
QListWidgetItem* ProjectSamplesPanel::addItem(const QString& text, const QColor& dot, const QString& tip) {
    auto* it = new QListWidgetItem(text);
    QPixmap pm(10, 10);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(1, 1, 8, 8);
    }
    it->setIcon(QIcon(pm));
    it->setToolTip(tip);
    m_list->addItem(it);
    return it;
}
void ProjectSamplesPanel::rebuild() {
    QString keep = m_list->currentItem() ? m_list->currentItem()->data(ProjectItemList::PayloadRole).toString() : QString();
    QSet<QString> keepSel;
    for (auto* x : m_list->selectedItems()) keepSel.insert(x->data(ProjectItemList::PayloadRole).toString());
    m_list->clear();
    auto& proj = m_app.project();
    if (m_tab == Samples) {   // one row per sample: blue = in the playlist, green = on a channel, grey = unused, red = missing
        QSet<QString> seen;
        auto addRow = [&](const QString& name, const QString& path, const QString& used, double secs, bool isMissing, bool isUnused,
                          bool onChannel) {
            QColor dot = isMissing ? theme::red() : isUnused ? theme::textDim() : onChannel ? theme::green() : QColor("#4e8fb8");
            auto* it = addItem(name, dot,
                QString("%1\n%2%3\n%4")
                    .arg(name, used, secs > 0 ? QString("  |  %1 s").arg(secs, 0, 'f', 2) : QString(),
                        isMissing ? "MISSING: " + path : path));
            it->setData(ProjectItemList::PathRole, path);
            it->setData(ProjectItemList::PayloadRole, "audio|" + path);
            if (isMissing) it->setForeground(theme::red());
            else if (isUnused) it->setForeground(theme::textDim());
        };
        for (auto& a : proj.audioSources) {
            if (a.filePath.isEmpty() || seen.contains(a.filePath)) continue;
            seen.insert(a.filePath);
            int uses = (int)clipsOf(a.filePath).size();
            bool miss = !QFileInfo::exists(a.filePath);
            addRow(a.name.isEmpty() ? QFileInfo(a.filePath).fileName() : a.name, a.filePath,
                uses ? QString("%1 clip%2 in the playlist").arg(uses).arg(uses > 1 ? "s" : "") : "not in the playlist",
                a.sampleRate > 0 ? a.frames / a.sampleRate : 0, miss, uses == 0, false);
        }
        for (auto& c : proj.channels) {
            if (c.samplePath.isEmpty() || c.samplePath.startsWith("builtin:") || seen.contains(c.samplePath)) continue;
            seen.insert(c.samplePath);
            double sr = 0;
            int ch = 0;
            int64_t fr = 0;
            AudioFile::probe(c.samplePath, &sr, &ch, &fr);
            addRow(QFileInfo(c.samplePath).fileName(), c.samplePath, "channel: " + c.name, sr > 0 ? fr / sr : 0,
                !QFileInfo::exists(c.samplePath), false, true);
        }
    } else if (m_tab == Patterns) {
        for (auto& p : proj.patterns) {
            int notes = 0;
            for (auto& c : p.channels) notes += (int)c.notes.size();
            auto* it = addItem(p.name, p.color,
                QString("%1\n%2 channel%3, %4 note%5 - click to select, drag into the playlist")
                    .arg(p.name)
                    .arg(p.channels.size())
                    .arg(p.channels.size() == 1 ? "" : "s")
                    .arg(notes)
                    .arg(notes == 1 ? "" : "s"));
            it->setData(ProjectItemList::IdRole, (qulonglong)p.id);
            it->setData(ProjectItemList::PayloadRole, "pattern|" + QString::number(p.id));
            if (p.id == proj.selectedPatternId) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
            }
        }
    } else {
        for (auto& l : proj.automationLanes) {
            auto* it = addItem(l.name, l.color, l.name + "\ndrag into the playlist to place an automation clip");
            it->setData(ProjectItemList::IdRole, (qulonglong)l.id);
            it->setData(ProjectItemList::PayloadRole, "lane|" + QString::number(l.id));
        }
        if (proj.automationLanes.empty()) {
            auto* it = new QListWidgetItem("No automation yet - right-click a knob > Create automation clip");
            it->setFlags(Qt::NoItemFlags);
            it->setForeground(theme::textDim());
            m_list->addItem(it);
        }
    }
    {
        QSignalBlocker b(m_list);
        for (int i = 0; i < m_list->count(); ++i) {
            auto* x = m_list->item(i);
            QString pl = x->data(ProjectItemList::PayloadRole).toString();
            if (pl == keep) m_list->setCurrentItem(x, QItemSelectionModel::NoUpdate);
            if (keepSel.contains(pl)) x->setSelected(true);
        }
    }
}
void ProjectSamplesPanel::onContext(const QPoint& pos) {
    auto* it = m_list->itemAt(pos);
    QMenu m(this);
    if (it && (it->flags() & Qt::ItemIsEnabled) && !it->isSelected()) {
        m_list->clearSelection();
        it->setSelected(true);
        m_list->setCurrentItem(it);
    }
    auto [ids, paths] = selection();
    int n = (int)(m_tab == Samples ? paths.size() : ids.size());
    QString many = n > 1 ? QString(" (%1)").arg(n) : QString();
    if (m_tab == Patterns) {
        m.addAction("New pattern", [this] {
            Id id = m_app.addPattern();
            m_app.selectPattern(id);
        });
        if (n) m.addSeparator();
        if (n == 1) m.addAction("Select", [this, pid = ids[0]] { m_app.selectPattern(pid); });
        addPatternActions(&m, m_app, this, ids);
        if (n) {
            m.addAction("Add to playlist at playhead" + many, [this, ids] {
                Id t = m_app.firstPlaylistTrack();
                Tick at = (Tick)m_app.engine().positionTicks();
                for (Id id : ids) m_app.addPatternClip(t, id, at);
            });
            m.addSeparator();
            m.addAction("Delete" + many, [this, ids] {
                for (Id id : ids) m_app.deletePattern(id);
            });
        }
        m.exec(m_list->viewport()->mapToGlobal(pos));
        return;
    }
    if (m_tab == Automation) {
        if (!n) return;
        m.addAction("Add to playlist at playhead" + many, [this, ids] {
            Id t = m_app.firstPlaylistTrack();
            Tick at = (Tick)m_app.engine().positionTicks();
            for (Id id : ids) m_app.addAutomationClip(t, id, at, 0);
        });
        m.addSeparator();
        m.addAction("Delete automation" + many, [this, ids] {
            for (Id id : ids) m_app.deleteLane(id);
        });
        m.exec(m_list->viewport()->mapToGlobal(pos));
        return;
    }
    // samples: one menu for any number of them; a single one also gets its details, playlist spots, preview and relink
    if (n == 1) {
        QString path = paths.first();
        m.addAction("Open details", [this, path] { openDetails(path); });
        auto onPlaylist = clipsOf(path);
        m.addAction(onPlaylist.empty() ? "Show on playlist (not placed)" : QString("Show on playlist (%1)").arg(onPlaylist.size()),
            [this, onPlaylist] { Q_EMIT showOnPlaylist(onPlaylist); })
            ->setEnabled(!onPlaylist.empty());
        m.addAction("Preview", [this, path] { m_app.previewFile(path); });
        if (!QFileInfo::exists(path))
            m.addAction("Locate...", [this, path] {
                QString p = QFileDialog::getOpenFileName(this, "Locate " + QFileInfo(path).fileName());
                if (p.isEmpty()) return;
                for (auto& a : m_app.project().audioSources)
                    if (a.filePath == path) m_app.relinkSample(a.id, p);
            });
    }
    if (n) {
        m.addAction("Add to playlist at playhead" + many, [this, paths] {
            Id t = m_app.firstPlaylistTrack();
            Tick at = (Tick)m_app.engine().positionTicks();
            for (auto& p : paths) m_app.addAudioClip(t, p, at);
        });
        m.addAction((n > 1 ? "Add as sampler channels" : "Add as sampler channel") + many, [this, paths] {
            for (auto& p : paths) m_app.addChannel(ChannelType::Sampler, "native.sampler", QString(), p);
        });
        m.addSeparator();
        m.addAction("Remove from project" + many, [this, paths] { removeSamples(paths); });
    }
    m.addAction("Remove all unused samples", [this] { removeUnusedSamples(); });
    m.exec(m_list->viewport()->mapToGlobal(pos));
}

std::pair<std::vector<Id>, QStringList> ProjectSamplesPanel::selection() const {
    std::vector<Id> ids;
    QStringList paths;
    for (auto* x : m_list->selectedItems()) {
        if (!(x->flags() & Qt::ItemIsEnabled)) continue;
        ids.push_back(x->data(ProjectItemList::IdRole).toULongLong());
        QString p = x->data(ProjectItemList::PathRole).toString();
        if (!p.isEmpty()) paths << p;
    }
    return {ids, paths};
}
// a sample leaves the project with every playlist clip that plays it (one undo step)
void ProjectSamplesPanel::removeSamples(const QStringList& paths) {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& p : paths)
        for (auto& a : m_app.project().audioSources)
            if (a.filePath == p) {
                for (Id c : clipsOf(p)) cmds.push_back(std::make_unique<RemoveObjectCommand<Clip>>(m_app.model(), c, ""));
                cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, ""));
            }
    if (!cmds.empty()) m_app.model().macro(paths.size() > 1 ? "Remove samples from project" : "Remove sample from project", std::move(cmds));
}
void ProjectSamplesPanel::removeUnusedSamples() {
    std::vector<std::unique_ptr<Command>> cmds;
    for (auto& a : m_app.project().audioSources)
        if (clipsOf(a.filePath).empty()) cmds.push_back(std::make_unique<RemoveObjectCommand<AudioSource>>(m_app.model(), a.id, ""));
    if (!cmds.empty()) m_app.model().macro("Remove unused samples", std::move(cmds));
}
std::vector<Id> ProjectSamplesPanel::clipsOf(const QString& path) const {
    auto& proj = m_app.project();
    std::vector<const Clip*> v;
    for (auto& c : proj.clips)
        if (c.type == ClipType::Audio)
            if (const AudioSource* a = proj.audioSource(c.sourceId); a && a->filePath == path) v.push_back(&c);
    std::sort(v.begin(), v.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    std::vector<Id> ids;
    for (auto* c : v) ids.push_back(c->id);
    return ids;
}
void ProjectSamplesPanel::openDetails(const QString& path) {
    if (auto clips = clipsOf(path); !clips.empty()) {
        Q_EMIT openAudioClip(clips.front());
        return;
    }
    for (auto& a : m_app.project().audioSources)
        if (a.filePath == path) {
            Q_EMIT openSample(a.id);
            return;
        }
    for (auto& ch : m_app.project().channels)
        if (ch.samplePath == path) {
            Q_EMIT openEditor(ch.id);
            return;
        }
}
// Samples dragged in from the browser are saved to the project without being placed (they show as 'not in the playlist')
bool ProjectSamplesPanel::takeDrop(const QMimeData* md) {
    QStringList paths;
    for (auto& u : md->urls())
        if (u.isLocalFile() && AudioFile::isAudioFile(u.toLocalFile())) paths << u.toLocalFile();
    if (auto it = dropItem(md); paths.isEmpty() && it.kind == "audio") paths << it.value;
    if (paths.isEmpty()) return false;
    for (auto& p : paths) m_app.ensureAudioSource(p);
    setTab(Samples);
    showToast(paths.size() == 1 ? QString("Saved \u201c%1\u201d to the project").arg(QFileInfo(paths.first()).completeBaseName())
                                : QString("Saved %1 samples to the project").arg(paths.size()),
        QCursor::pos());
    return true;
}
void ProjectSamplesPanel::debugShowFirst() {
    if (m_list->count()) Q_EMIT showOnPlaylist(clipsOf(m_list->item(0)->data(ProjectItemList::PathRole).toString()));
}
void ProjectSamplesPanel::debugDoubleClickFirst() {
    if (m_list->count()) Q_EMIT m_list->itemDoubleClicked(m_list->item(0));
}
bool ProjectSamplesPanel::debugDropFile(const QString& path) {
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(path)});
    setDropItem(&md, "audio|" + path);
    return takeDrop(&md);
}
void ProjectItemList::dragEnterEvent(QDragEnterEvent* e) {
    if (!m_draggingOut && e->source() != this && onDrop && hasAppDrop(e->mimeData())) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else QListWidget::dragEnterEvent(e);
}
void ProjectItemList::dragMoveEvent(QDragMoveEvent* e) {
    if (!m_draggingOut && e->source() != this && onDrop) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else QListWidget::dragMoveEvent(e);
}
void ProjectItemList::dropEvent(QDropEvent* e) {
    if (!m_draggingOut && e->source() != this && onDrop && onDrop(e->mimeData())) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
        return;
    }
    QListWidget::dropEvent(e);
}
}   // namespace mt
