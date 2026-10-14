#include "ui/dialogs/Dialogs.h"
#include "ui/widgets/Controls.h"
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QInputDialog>
#include <QGridLayout>
#include <QColorDialog>
#include "ui/Theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QPushButton>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QSettings>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QGroupBox>

namespace mt {

// ---------------------------------------------------------------- audio settings
AudioSettingsDialog::AudioSettingsDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Audio Settings");
    resize(520, 360);
    auto* form = new QFormLayout(this);
    m_api = new QComboBox(this);
    m_out = new QComboBox(this);
    m_in = new QComboBox(this);
    m_sr = new QComboBox(this);
    m_buf = new QComboBox(this);
    m_inCh = new QComboBox(this);
    m_outCh = new QComboBox(this);
    m_api->addItem("All host APIs");
    for (auto& a : app.devices().hostApis()) m_api->addItem(a);
    auto fill = [this] {
        m_out->clear();
        m_in->clear();
        m_in->addItem("(none)", -1);
        QString api = m_api->currentIndex() > 0 ? m_api->currentText() : QString();
        const auto& cfg = m_app.devices().config();
        for (auto& d : m_app.devices().devices()) {
            if (!api.isEmpty() && d.hostApi != api) continue;
            QString label = QString("%1  [%2]").arg(d.name, d.hostApi);
            if (d.maxOutputs > 0) {
                m_out->addItem(label, d.index);
                if (d.index == cfg.outputDevice) m_out->setCurrentIndex(m_out->count() - 1);
            }
            if (d.maxInputs > 0) {
                m_in->addItem(label, d.index);
                if (d.index == cfg.inputDevice) m_in->setCurrentIndex(m_in->count() - 1);
            }
        }
    };
    connect(m_api, &QComboBox::currentIndexChanged, this, fill);
    const auto& cfg = app.devices().config();
    {
        QString api = cfg.hostApi;
        for (int i = 0; i < m_api->count(); ++i)
            if (m_api->itemText(i) == api) m_api->setCurrentIndex(i);
    }
    fill();
    for (double sr : AudioDeviceManager::standardSampleRates()) {
        m_sr->addItem(QString::number(sr), sr);
        if (sr == cfg.sampleRate) m_sr->setCurrentIndex(m_sr->count() - 1);
    }
    for (int b : AudioDeviceManager::standardBufferSizes()) {
        m_buf->addItem(QString("%1 samples").arg(b), b);
        if (b == cfg.bufferSize) m_buf->setCurrentIndex(m_buf->count() - 1);
    }
    for (int c : {1, 2, 4, 8}) {
        m_inCh->addItem(QString::number(c), c);
        if (c == std::max(cfg.inputChannels, 1)) m_inCh->setCurrentIndex(m_inCh->count() - 1);
        m_outCh->addItem(QString::number(c), c);
        if (c == cfg.outputChannels) m_outCh->setCurrentIndex(m_outCh->count() - 1);
    }
    m_monitor = new QCheckBox("Monitor hardware inputs through the mixer", this);
    m_monitor->setChecked(QSettings().value("audio/monitor", true).toBool());
    form->addRow("Host API", m_api);
    form->addRow("Output device", m_out);
    form->addRow("Input device", m_in);
    form->addRow("Sample rate", m_sr);
    form->addRow("Buffer size", m_buf);
    form->addRow("Input channels", m_inCh);
    form->addRow("Output channels", m_outCh);
    form->addRow(m_monitor);
    auto* jack = new QCheckBox("Sync transport with JACK (play, stop and position follow other JACK apps)", this);
    jack->setChecked(m_app.jackSync());
    form->addRow(jack);
    connect(jack, &QCheckBox::toggled, this, [this, jack](bool b) {
        QString err;
        if (!m_app.setJackSync(b, &err)) {
            QSignalBlocker sb(jack);
            jack->setChecked(false);
            m_status->setText(err);
        }
    });
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    form->addRow(m_status);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    form->addRow(bb);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &AudioSettingsDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_status->setText(app.engine().deviceRunning() ? QString("Running: %1 Hz, %2 frames, output latency %3 ms")
                                                         .arg(app.engine().sampleRate())
                                                         .arg(cfg.bufferSize)
                                                         .arg(app.devices().reportedOutputLatency() * 1000, 0, 'f', 1)
                                                   : "Audio device not running");
}
void AudioSettingsDialog::apply() {
    AudioConfig cfg = m_app.devices().config();
    cfg.hostApi = m_api->currentIndex() > 0 ? m_api->currentText() : QString();
    cfg.outputDevice = m_out->currentData().toInt();
    cfg.inputDevice = m_in->currentData().toInt();
    cfg.sampleRate = m_sr->currentData().toDouble();
    cfg.bufferSize = m_buf->currentData().toInt();
    cfg.inputChannels = m_inCh->currentData().toInt();
    cfg.outputChannels = m_outCh->currentData().toInt();
    QSettings().setValue("audio/monitor", m_monitor->isChecked());
    m_app.engine().setMonitorInputs(m_monitor->isChecked());
    QString err;
    bool ok = m_app.applyAudioConfig(cfg, &err);
    m_status->setText(ok ? QString("Running: %1 Hz, %2 frames, output latency %3 ms")
                               .arg(m_app.engine().sampleRate())
                               .arg(cfg.bufferSize)
                               .arg(m_app.devices().reportedOutputLatency() * 1000, 0, 'f', 1)
                         : "Failed: " + err);
}

// ---------------------------------------------------------------- MIDI settings
MidiSettingsDialog::MidiSettingsDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("MIDI Settings");
    resize(640, 480);
    auto* lay = new QVBoxLayout(this);
    auto* hl = new QHBoxLayout();
    auto mkList = [&](const QString& title, bool input) {
        auto* v = new QVBoxLayout();
        v->addWidget(new QLabel(title, this));
        auto* l = new QListWidget(this);
        QStringList enabled = input ? app.midi().enabledInputs() : app.midi().enabledOutputs();
        for (auto& p : app.midi().ports()) {
            if (input ? !p.input : !p.output) continue;
            auto* it = new QListWidgetItem(p.name, l);
            it->setData(Qt::UserRole, p.id());
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(enabled.contains(p.id()) ? Qt::Checked : Qt::Unchecked);
        }
        v->addWidget(l);
        hl->addLayout(v);
        return l;
    };
    m_inputs = mkList("MIDI inputs (check to connect)", true);
    m_outputs = mkList("MIDI outputs", false);
    lay->addLayout(hl);
    auto collect = [](QListWidget* l) {
        QStringList ids;
        for (int i = 0; i < l->count(); ++i)
            if (l->item(i)->checkState() == Qt::Checked) ids << l->item(i)->data(Qt::UserRole).toString();
        return ids;
    };
    connect(m_inputs, &QListWidget::itemChanged, this, [this, collect] { m_app.midi().setEnabledInputs(collect(m_inputs)); });
    connect(m_outputs, &QListWidget::itemChanged, this, [this, collect] { m_app.midi().setEnabledOutputs(collect(m_outputs)); });
    m_clock = new QCheckBox("Send MIDI clock / start / stop to outputs", this);
    m_clock->setChecked(QSettings().value("midi/clockOut", false).toBool());
    lay->addWidget(m_clock);
    connect(m_clock, &QCheckBox::toggled, this, [this](bool b) {
        QSettings().setValue("midi/clockOut", b);
        m_app.engine().setMidiClockOut(b);
    });
    auto* clockIn = new QCheckBox("Follow incoming MIDI clock (tempo, start / stop, song position)", this);
    clockIn->setChecked(QSettings().value("midi/clockIn", false).toBool());
    lay->addWidget(clockIn);
    connect(clockIn, &QCheckBox::toggled, this, [this](bool b) {
        QSettings().setValue("midi/clockIn", b);
        m_app.midi().setClockFollow(b);
        if (!b) m_app.engine().setBpm(m_app.project().transport.bpm);   // back to the project's own tempo
    });
    lay->addWidget(new QLabel("Controller links (right-click any knob/fader > Link to MIDI controller):", this));
    m_mappings = new QTableWidget(this);
    m_mappings->setColumnCount(3);
    m_mappings->setHorizontalHeaderLabels({"CC", "Channel", "Target"});
    m_mappings->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_mappings);
    auto* rm = new QPushButton("Remove selected link", this);
    lay->addWidget(rm);
    connect(rm, &QPushButton::clicked, this, [this] {
        int r = m_mappings->currentRow();
        if (r >= 0) {
            m_app.removeMidiMapping(r);
            rebuildMappings();
        }
    });
    auto* hint = theme::dimLabel(
        "Hardware ports appear here when connected (ALSA sequencer). Live input plays the channel selected in the Channel Rack.", this);
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rebuildMappings();
}
void MidiSettingsDialog::rebuildMappings() {
    m_mappings->setRowCount(0);
    for (auto& m : m_app.project().midiMappings) {
        int r = m_mappings->rowCount();
        m_mappings->insertRow(r);
        m_mappings->setItem(r, 0, new QTableWidgetItem(QString::number(m.cc)));
        m_mappings->setItem(r, 1, new QTableWidgetItem(m.midiChannel < 0 ? "any" : QString::number(m.midiChannel + 1)));
        m_mappings->setItem(r, 2, new QTableWidgetItem(m.name.isEmpty() ? m_app.targetName(m.target) : m.name));
    }
}

// ---------------------------------------------------------------- plugin manager
PluginManagerDialog::PluginManagerDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Plugin Manager");
    resize(820, 560);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
        "Plugin folders (VST2 .so, VST3 bundles and CLAP files are scanned recursively, out of process; LV2 comes from the standard LV2 paths):",
        this));
    auto* dl = new QHBoxLayout();
    m_dirs = new QListWidget(this);
    m_dirs->addItems(app.pluginDb().directories());
    m_dirs->setMaximumHeight(110);
    dl->addWidget(m_dirs);
    auto* dbtns = new QVBoxLayout();
    auto* add = new QPushButton("Add...", this);
    auto* rem = new QPushButton("Remove", this);
    dbtns->addWidget(add);
    dbtns->addWidget(rem);
    dbtns->addStretch(1);
    dl->addLayout(dbtns);
    lay->addLayout(dl);
    auto* bridge = new QCheckBox(
        "Run new plugins in their own process (crash protection: a crashing plugin goes silent, the app keeps running)", this);
    bridge->setChecked(QSettings().value("plugins/bridge", false).toBool());
    bridge->setToolTip("Each plugin window has an 'Own process' switch for plugins already in the project");
    lay->addWidget(bridge);
    connect(bridge, &QCheckBox::toggled, this, [](bool b) { QSettings().setValue("plugins/bridge", b); });
    connect(add, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Plugin folder");
        if (!d.isEmpty()) {
            m_dirs->addItem(d);
            QStringList l;
            for (int i = 0; i < m_dirs->count(); ++i) l << m_dirs->item(i)->text();
            m_app.pluginDb().setDirectories(l);
        }
    });
    connect(rem, &QPushButton::clicked, this, [this] {
        delete m_dirs->currentItem();
        QStringList l;
        for (int i = 0; i < m_dirs->count(); ++i) l << m_dirs->item(i)->text();
        m_app.pluginDb().setDirectories(l);
    });
    {   // Windows plugins via yabridge
        auto* box = new QGroupBox("Windows plugins (yabridge)", this);
        auto* bl = new QVBoxLayout(box);
        m_yabStatus = new QLabel(box);
        m_yabStatus->setWordWrap(true);
        m_yabStatus->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_yabStatus->setOpenExternalLinks(true);
        bl->addWidget(m_yabStatus);
        auto* yl = new QHBoxLayout();
        m_yabAdd = new QPushButton("Add Windows plugin folder...", box);
        m_yabSync = new QPushButton("Sync yabridge + scan", box);
        yl->addWidget(m_yabAdd);
        yl->addWidget(m_yabSync);
        yl->addStretch(1);
        bl->addLayout(yl);
        lay->addWidget(box);
        connect(m_yabAdd, &QPushButton::clicked, this, [this] {
            QString d = QFileDialog::getExistingDirectory(this,
                "Folder with Windows VST2 (.dll) / VST3 plugins (inside your Wine prefix, e.g. ~/.wine/drive_c/Program Files/Common Files/VST3)");
            if (d.isEmpty()) return;
            runYabridge({"add", d}, true);
        });
        connect(m_yabSync, &QPushButton::clicked, this, [this] { runYabridge({"sync"}, true); });
        refreshYabridge();
    }
    auto* sl = new QHBoxLayout();
    auto* scan = new QPushButton("Scan for new plugins", this);
    auto* rescan = new QPushButton("Rescan all", this);
    auto* cancel = new QPushButton("Cancel", this);
    auto* clean = new QPushButton("Remove missing", this);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter...");
    sl->addWidget(scan);
    sl->addWidget(rescan);
    sl->addWidget(cancel);
    sl->addWidget(clean);
    sl->addWidget(m_filter, 1);
    lay->addLayout(sl);
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    lay->addWidget(m_progress);
    m_status = new QLabel(this);
    lay->addWidget(m_status);
    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({"★", "Hide", "Name", "Vendor", "Format", "Type", "Path / status"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(m_table, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(scan, &QPushButton::clicked, this, [this] {
        m_progress->setVisible(true);
        m_app.scanner().scan(false);
    });
    connect(rescan, &QPushButton::clicked, this, [this] {
        m_progress->setVisible(true);
        m_app.scanner().scan(true);
    });
    connect(cancel, &QPushButton::clicked, this, [this] { m_app.scanner().cancel(); });
    connect(clean, &QPushButton::clicked, this, [this] { m_app.pluginDb().removeMissing(); });
    connect(&app.scanner(), &PluginScanner::progress, this, [this](int d, int t, const QString& cur) {
        m_progress->setVisible(true);
        m_progress->setRange(0, std::max(t, 1));
        m_progress->setValue(d);
        m_status->setText("Scanning " + cur);
    });
    connect(&app.scanner(), &PluginScanner::finished, this, [this] {
        m_progress->setVisible(false);
        m_status->setText(QString("Scan complete: %1 plugin entries").arg(m_app.pluginDb().plugins().size()));
        rebuild();
        refreshYabridge();
    });
    connect(&app.pluginDb(), &PluginDatabase::changed, this, &PluginManagerDialog::rebuild);
    connect(m_filter, &QLineEdit::textChanged, this, &PluginManagerDialog::rebuild);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int r, int c) {
        QString key = m_table->item(r, 2)->data(Qt::UserRole).toString();
        const PluginInfo* info = nullptr;
        for (auto& p : m_app.pluginDb().plugins())
            if (p.key() == key) info = &p;
        if (!info) return;
        if (c == 0) m_app.pluginDb().setFavorite(key, !info->favorite);
        else if (c == 1) m_app.pluginDb().setHidden(key, !info->hidden);
    });
    rebuild();
}
void PluginManagerDialog::refreshYabridge() {
    auto st = yabridge::status();
    m_yabAdd->setEnabled(st.installed());
    m_yabSync->setEnabled(st.installed());
    if (!st.installed()) {
        m_yabStatus->setText(QString(
            "<b>Not installed.</b> Windows .dll / .vst3 plugins run through Wine + yabridge. On Arch / CachyOS: <code>sudo pacman -S wine yabridge yabridgectl</code> (multilib enabled), other distros: <a href=\"https://github.com/robbert-vdh/yabridge\">github.com/robbert-vdh/yabridge</a>. Then reopen this dialog.%1")
                .arg(st.wine.isEmpty() ? "" : " Wine found at " + st.wine + "."));
        return;
    }
    int bridged = 0;
    for (auto& p : m_app.pluginDb().plugins())
        if (yabridge::isBridged(p.path)) ++bridged;
    m_yabStatus->setText(QString(
        "<b>%1</b> (%2)%3<br>Windows plugin folders: %4 &nbsp;·&nbsp; bridged plugins in the database: %5. Copy a plugin's .dll / .vst3 into a folder inside your Wine prefix, add that folder here (once), then <i>Sync</i>: the wrappers land in ~/.vst/yabridge and ~/.vst3/yabridge and are scanned like native plugins.")
            .arg(st.version.isEmpty() ? "yabridge" : st.version, st.yabridgectl,
                st.wine.isEmpty() ? " - <b>Wine not found</b>, install it first" : "",
                st.winePluginDirs.isEmpty() ? "none yet" : st.winePluginDirs.join(", "))
            .arg(bridged));
}
void PluginManagerDialog::runYabridge(const QStringList& args, bool syncAndScan) {
    auto st = yabridge::status();
    if (!st.installed()) return;
    m_status->setText("yabridgectl " + args.join(' ') + " ...");
    m_yabAdd->setEnabled(false);
    m_yabSync->setEnabled(false);
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p, args, syncAndScan](int code, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAllStandardOutput() + p->readAllStandardError()).trimmed();
        p->deleteLater();
        m_status->setText(out.right(300));
        if (code != 0) {
            QMessageBox::warning(this, "yabridgectl failed", out.isEmpty() ? "yabridgectl returned an error" : out);
            refreshYabridge();
            return;
        }
        if (syncAndScan && args.first() == "add") {
            runYabridge({"sync"}, true);
            return;
        }
        // make sure the wrapper folders are scanned, then look for the new plugins
        QStringList dirs = m_app.pluginDb().directories();
        bool changed = false;
        for (auto& d : yabridge::wrapperDirs()) {
            bool covered = false;
            for (auto& x : dirs)
                if (d.startsWith(x)) covered = true;
            if (!covered && QDir(d).exists()) {
                dirs << d;
                changed = true;
            }
        }
        if (changed) {
            m_app.pluginDb().setDirectories(dirs);
            m_dirs->clear();
            m_dirs->addItems(dirs);
        }
        refreshYabridge();
        m_progress->setVisible(true);
        m_app.scanner().scan(false);
    });
    p->start(st.yabridgectl, args);
}
void PluginManagerDialog::rebuild() {
    m_table->setRowCount(0);
    QString f = m_filter->text();
    for (auto& p : m_app.pluginDb().plugins()) {
        if (!f.isEmpty() && !p.name.contains(f, Qt::CaseInsensitive) && !p.vendor.contains(f, Qt::CaseInsensitive) &&
            !p.path.contains(f, Qt::CaseInsensitive))
            continue;
        int r = m_table->rowCount();
        m_table->insertRow(r);
        auto set = [&](int c, const QString& t) {
            auto* it = new QTableWidgetItem(t);
            if (p.failed) it->setForeground(theme::red());
            if (p.hidden) it->setForeground(theme::textDim());
            m_table->setItem(r, c, it);
        };
        set(0, p.favorite ? "★" : "☆");
        set(1, p.hidden ? "✓" : "");
        set(2, p.name);
        m_table->item(r, 2)->setData(Qt::UserRole, p.key());
        set(3, p.vendor);
        set(4, p.format.toUpper());
        set(5, p.category);
        set(6, p.failed ? p.failReason + "  (" + p.path + ")" : p.path);
    }
    m_table->resizeColumnsToContents();
}

// ---------------------------------------------------------------- export
ExportDialog::ExportDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Export audio");
    resize(480, 380);
    auto* form = new QFormLayout(this);
    auto* pl = new QHBoxLayout();
    m_path = new QLineEdit(this);
    QString def = QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/" + app.projectTitle() + ".wav";
    m_path->setText(def);
    auto* browse = new QPushButton("...", this);
    pl->addWidget(m_path);
    pl->addWidget(browse);
    form->addRow("File", pl);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString p = QFileDialog::getSaveFileName(
            this, "Export", m_path->text(), "WAV (*.wav);;AIFF (*.aiff);;FLAC (*.flac);;MP3 (*.mp3);;OGG Vorbis (*.ogg)");
        if (!p.isEmpty()) {
            m_path->setText(p);
            QString ext = QFileInfo(p).suffix().toLower();
            for (int i = 0; i < m_format->count(); ++i)
                if (m_format->itemData(i).toString() == ext) m_format->setCurrentIndex(i);
        }
    });
    m_format = new QComboBox(this);
    m_format->addItem("WAV", "wav");
    m_format->addItem("AIFF", "aiff");
    m_format->addItem("FLAC", "flac");
    m_format->addItem("MP3 (LAME, 320 kbps)", "mp3");
    m_format->addItem("OGG Vorbis", "ogg");
    form->addRow("Format", m_format);
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        QString p = m_path->text();
        int dot = p.lastIndexOf('.');
        if (dot > 0) p = p.left(dot);
        m_path->setText(p + "." + m_format->currentData().toString());
    });
    m_sr = new QComboBox(this);
    for (double sr : AudioDeviceManager::standardSampleRates()) m_sr->addItem(QString::number(sr), sr);
    m_sr->setCurrentIndex(0);
    form->addRow("Sample rate", m_sr);
    m_bits = new QComboBox(this);
    m_bits->addItem("16-bit", 16);
    m_bits->addItem("24-bit", 24);
    m_bits->addItem("32-bit float", 32);
    m_bits->setCurrentIndex(1);
    form->addRow("Bit depth", m_bits);
    m_range = new QComboBox(this);
    m_range->addItems({"Whole song (playlist)", "Selected pattern", "Loop region"});
    form->addRow("Range", m_range);
    m_mono = new QCheckBox("Mono", this);
    m_normalize = new QCheckBox("Normalize to -0.3 dBFS", this);
    m_dither = new QCheckBox("Dither (TPDF, for 16/24-bit)", this);
    m_dither->setChecked(true);
    form->addRow(m_mono);
    form->addRow(m_normalize);
    form->addRow(m_dither);
    m_stems = new QCheckBox("Split mixer tracks (one file per used track)", this);
    m_stems->setToolTip("Stems: every mixer track something plays into, as heard there (after its effects and fader), plus the master");
    form->addRow(m_stems);
    m_tail = new QDoubleSpinBox(this);
    m_tail->setRange(0, 60);
    m_tail->setValue(3);
    m_tail->setSuffix(" s");
    m_tail->setToolTip("Extra time rendered after the end so reverbs and delays can ring out (stops early when silent)");
    form->addRow("Tail", m_tail);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    form->addRow(m_progress);
    auto* bb = new QDialogButtonBox(this);
    auto* go = bb->addButton("Export", QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(go, &QPushButton::clicked, this, &ExportDialog::doExport);
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
        m_cancel = true;
        reject();
    });
}
void ExportDialog::doExport() {
    RenderOptions o;
    o.path = m_path->text();
    o.format = m_format->currentData().toString();
    o.sampleRate = m_sr->currentData().toDouble();
    o.bitDepth = m_bits->currentData().toInt();
    o.mono = m_mono->isChecked();
    o.normalize = m_normalize->isChecked();
    o.dither = m_dither->isChecked();
    o.tailSeconds = m_tail->value();
    o.stems = m_stems->isChecked();
    int range = m_range->currentIndex();
    o.songMode = range != 1;
    if (range == 2) {
        o.start = m_app.project().transport.loopStart;
        o.end = m_app.project().transport.loopEnd;
    }
    m_cancel = false;
    QString err;
    setEnabled(false);
    bool ok = m_app.exportAudio(
        o,
        [this](double p) {
            m_progress->setValue((int)(p * 1000));
            QCoreApplication::processEvents();
            return !m_cancel;
        },
        &err);
    setEnabled(true);
    if (ok) {
        QMessageBox::information(
            this, "Export", (o.stems ? "Exported stems to " + QFileInfo(o.path).absolutePath() : "Exported to " + o.path));
        accept();
    } else QMessageBox::warning(this, "Export failed", err);
}

// ---------------------------------------------------------------- missing files
MissingFilesDialog::MissingFilesDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Missing samples");
    resize(700, 400);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel("These audio files referenced by the project could not be found:", this));
    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"Name", "Last known path"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(m_table, 1);
    auto* hl = new QHBoxLayout();
    auto* locate = new QPushButton("Locate selected...", this);
    auto* search = new QPushButton("Search in folder...", this);
    auto* skip = new QPushButton("Skip", this);
    hl->addWidget(locate);
    hl->addWidget(search);
    hl->addStretch(1);
    hl->addWidget(skip);
    lay->addLayout(hl);
    connect(locate, &QPushButton::clicked, this, [this] {
        int r = m_table->currentRow();
        if (r < 0) return;
        Id id = (Id)m_table->item(r, 0)->data(Qt::UserRole).toULongLong();
        QString p = QFileDialog::getOpenFileName(this, "Locate " + m_table->item(r, 0)->text());
        if (!p.isEmpty()) {
            m_app.relinkSample(id, p);
            rebuild();
        }
    });
    connect(search, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Search folder (recursive)");
        if (!d.isEmpty()) {
            int n = m_app.searchMissingIn(d);
            m_app.statusMessage(QString("Relinked %1 file(s)").arg(n), 3000);
            rebuild();
        }
    });
    connect(skip, &QPushButton::clicked, this, &QDialog::accept);
    rebuild();
}
void MissingFilesDialog::rebuild() {
    m_table->setRowCount(0);
    for (auto* a : m_app.missingSamples()) {
        int r = m_table->rowCount();
        m_table->insertRow(r);
        auto* it = new QTableWidgetItem(a->name);
        it->setData(Qt::UserRole, (qulonglong)a->id);
        m_table->setItem(r, 0, it);
        m_table->setItem(r, 1, new QTableWidgetItem(a->filePath));
    }
    if (m_table->rowCount() == 0) accept();
}
bool MissingFilesDialog::checkAndPrompt(App& app, QWidget* parent) {
    if (app.missingSamples().empty()) return false;
    MissingFilesDialog d(app, parent);
    d.exec();
    return true;
}

// ---------------------------------------------------------------- shortcuts
ShortcutsDialog::ShortcutsDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Keyboard shortcuts");
    resize(620, 520);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel("Click a shortcut cell to change it. Empty = no shortcut.", this));
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"Category", "Command", "Shortcut", "Default"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(m_table, 1);
    auto& reg = app.commands();
    for (auto it = reg.entries().begin(); it != reg.entries().end(); ++it) {
        const auto& e = it.value();
        int r = m_table->rowCount();
        m_table->insertRow(r);
        m_table->setItem(r, 0, new QTableWidgetItem(e.category));
        auto* name = new QTableWidgetItem(e.description);
        name->setData(Qt::UserRole, e.id);
        m_table->setItem(r, 1, name);
        auto* edit = new QKeySequenceEdit(reg.effectiveShortcut(e), this);
        m_table->setCellWidget(r, 2, edit);
        connect(edit, &QKeySequenceEdit::editingFinished, this,
            [this, edit, id = e.id] { m_app.commands().setUserShortcut(id, edit->keySequence()); });
        m_table->setItem(r, 3, new QTableWidgetItem(e.defaultShortcut.toString(QKeySequence::NativeText)));
    }
    m_table->resizeColumnsToContents();
    auto* hl = new QHBoxLayout();
    auto* reset = new QPushButton("Reset selected to default", this);
    hl->addWidget(reset);
    hl->addStretch(1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    hl->addWidget(bb);
    lay->addLayout(hl);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(reset, &QPushButton::clicked, this, [this] {
        int r = m_table->currentRow();
        if (r < 0) return;
        QString id = m_table->item(r, 1)->data(Qt::UserRole).toString();
        m_app.commands().resetShortcut(id);
        if (auto* e = qobject_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 2)))
            e->setKeySequence(m_app.commands().action(id)->shortcut());
    });
}

// ---------------------------------------------------------------- autosave
AutosaveDialog::AutosaveDialog(App& app, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Autosave / backups");
    auto* form = new QFormLayout(this);
    auto a = app.autosaveSettings();
    auto* enabled = new QCheckBox("Enable autosave backups", this);
    enabled->setChecked(a.enabled);
    form->addRow(enabled);
    auto* interval = new QSpinBox(this);
    interval->setRange(1, 120);
    interval->setValue(a.intervalMinutes);
    interval->setSuffix(" min");
    form->addRow("Save a backup every", interval);
    auto* copies = new QSpinBox(this);
    copies->setRange(1, 200);
    copies->setValue(a.maxCopies);
    form->addRow("Keep the newest", copies);
    auto* info = theme::dimLabel(
        "Backups are written to a <b>Backup</b> folder next to the project file (name_date_time.mtp) whenever the project has unsaved changes; the oldest copies are recycled once the limit is reached. Ctrl+S always saves over the project itself. Unsaved (untitled) projects get a crash-recovery copy instead.",
        this);
    info->setWordWrap(true);
    form->addRow(info);
    auto* now = new QPushButton("Write a backup now", this);
    form->addRow(now);
    connect(now, &QPushButton::clicked, this, [&app, this] {
        QString err;
        if (app.writeBackupNow(&err)) QMessageBox::information(this, "Backup", "Backup written to " + app.backupDir());
        else QMessageBox::warning(this, "Backup", err);
    });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, [&app, this, enabled, interval, copies] {
        App::AutosaveSettings s;
        s.enabled = enabled->isChecked();
        s.intervalMinutes = interval->value();
        s.maxCopies = copies->value();
        app.setAutosaveSettings(s);
        accept();
    });
}

// ---------------------------------------------------------------- project info
ProjectInfoDialog::ProjectInfoDialog(App& app, QWidget* parent)
    : QDialog(parent)
    , m_app(app) {
    setWindowTitle("Project info");
    resize(420, 320);
    auto* form = new QFormLayout(this);
    const auto& m = app.project().meta;
    m_title = new QLineEdit(m.title, this);
    m_author = new QLineEdit(m.author, this);
    m_genre = new QLineEdit(m.genre, this);
    m_comments = new QPlainTextEdit(m.comments, this);
    form->addRow("Title", m_title);
    form->addRow("Author", m_author);
    form->addRow("Genre", m_genre);
    form->addRow("Comments", m_comments);
    form->addRow("Created", new QLabel(m.created, this));
    form->addRow("Modified", new QLabel(m.modified, this));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        QString t = m_title->text(), a = m_author->text(), g = m_genre->text(), c = m_comments->toPlainText();
        m_app.model().editMeta("Project info", [=](Project& p) {
            p.meta.title = t;
            p.meta.author = a;
            p.meta.genre = g;
            p.meta.comments = c;
        });
        accept();
    });
}

// ---------------------------------------------------------------- theme
ThemeDialog::ThemeDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Theme");
    resize(560, 460);
    m_palette = theme::current();
    m_entry = m_palette;
    auto* lay = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    lay->addLayout(row, 1);

    m_presets = new QListWidget(this);
    m_presets->setFixedWidth(160);
    row->addWidget(m_presets);

    auto* right = new QVBoxLayout();
    row->addLayout(right, 1);
    right->addWidget(new QLabel("Click a colour to change it. Changes apply to every window as you make them.", this));
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    right->addLayout(grid);
    int r = 0;
    for (auto& role : theme::paletteRoleNames()) {
        auto* sw = new QToolButton(this);
        sw->setFixedSize(26, 20);
        sw->setAutoRaise(false);
        sw->setToolTip("Pick a colour for " + theme::paletteRoleLabel(role));
        connect(sw, &QToolButton::clicked, this, [this, role] {
            QColor c = QColorDialog::getColor(theme::paletteRole(m_palette, role), this, "Colour: " + theme::paletteRoleLabel(role));
            if (!c.isValid()) return;
            theme::setPaletteRole(m_palette, role, c);
            apply();
            refreshSwatches();
        });
        m_swatches[role] = sw;
        grid->addWidget(sw, r / 2, (r % 2) * 2);
        grid->addWidget(new QLabel(theme::paletteRoleLabel(role), this), r / 2, (r % 2) * 2 + 1);
        ++r;
    }
    right->addStretch(1);

    auto* btns = new QHBoxLayout();
    lay->addLayout(btns);
    auto* reset = new QPushButton("Revert", this);
    reset->setToolTip("Discard the edits made since this theme was selected");
    btns->addWidget(reset);
    m_saveAs = new QPushButton("Save as new theme...", this);
    m_saveAs->setToolTip("Keep the current colours as your own theme, leaving the preset untouched");
    btns->addWidget(m_saveAs);
    m_delete = new QPushButton("Delete", this);
    m_delete->setToolTip("Delete the selected custom theme");
    btns->addWidget(m_delete);
    btns->addStretch(1);
    auto* cancel = new QPushButton("Cancel", this);
    auto* ok = new QPushButton("Save", this);
    ok->setDefault(true);
    btns->addWidget(cancel);
    btns->addWidget(ok);

    connect(m_presets, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it, QListWidgetItem*) {
        if (it) selectPalette(it->data(Qt::UserRole).toString());
    });
    connect(reset, &QPushButton::clicked, this, [this] { selectPalette(m_palette.id); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] {
        auto name = askName(this, "New theme", m_palette.name + " (custom)");
        if (!name) return;
        theme::Palette p = m_palette;
        p.name = *name;
        // The settings key is derived from the name; saving the same name again just overwrites that theme.
        QString key = *name;
        key.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
        p.id = "user:" + key;
        theme::saveUserTheme(p);
        theme::saveTheme(p);
        m_palette = p;
        reloadList(p.id);
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        if (!theme::isUserTheme(m_palette.id)) return;
        if (QMessageBox::question(this, "Delete theme", "Delete \"" + m_palette.name + "\"?") != QMessageBox::Yes) return;
        theme::deleteUserTheme(m_palette.id);
        reloadList("midnight");
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        theme::applyTheme(m_entry);
        reject();
    });   // put back what was there
    connect(ok, &QPushButton::clicked, this, [this] {
        if (theme::isUserTheme(m_palette.id)) theme::saveUserTheme(m_palette);
        theme::saveTheme(m_palette);
        accept();
    });
    reloadList(m_palette.id);
}
// Built-in presets first, then whatever the user has saved. A custom theme starts life as a copy of whichever palette
// was selected when "Save as new theme..." was pressed, so the default theme is the natural starting point.
void ThemeDialog::reloadList(const QString& selectId) {
    QSignalBlocker block(m_presets);
    m_presets->clear();
    auto add = [this](const theme::Palette& p) {
        auto* it = new QListWidgetItem(p.name, m_presets);
        it->setData(Qt::UserRole, p.id);
        QPixmap pm(14, 14);
        pm.fill(p.accent);
        it->setIcon(QIcon(pm));
        return it;
    };
    for (auto& p : theme::presets()) add(p);
    auto users = theme::userThemes();
    if (!users.empty()) {
        auto* sepItem = new QListWidgetItem("- my themes -", m_presets);
        sepItem->setFlags(Qt::NoItemFlags);
    }
    for (auto& p : users) add(p);
    for (int i = 0; i < m_presets->count(); ++i)
        if (m_presets->item(i)->data(Qt::UserRole).toString() == selectId) m_presets->setCurrentRow(i);
    selectPalette(selectId);
}
void ThemeDialog::selectPalette(const QString& id) {
    if (const theme::Palette* p = theme::preset(id)) m_palette = *p;
    else if (theme::isUserTheme(id)) {
        for (auto& u : theme::userThemes())
            if (u.id == id) {
                m_palette = u;
                break;
            }
    }
    m_delete->setEnabled(theme::isUserTheme(m_palette.id));
    apply();
    refreshSwatches();
}
void ThemeDialog::apply() { theme::applyTheme(m_palette); }
void ThemeDialog::refreshSwatches() {
    for (auto& [role, sw] : m_swatches) {
        QColor c = theme::paletteRole(m_palette, role);
        sw->setStyleSheet(
            QString("QToolButton { background:%1; border:1px solid %2; border-radius:3px; }").arg(c.name(), theme::border().name()));
    }
}

}   // namespace mt
