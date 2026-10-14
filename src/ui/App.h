// Application object: owns every subsystem and exposes the command-style operations the UI panels use.
// UI -> App -> (undoable command on the ProjectModel) -> engine rebuild / direct atomic update.
#pragma once
#include "project/ProjectModel.h"
#include "project/ProjectSerializer.h"
#include "audio/AudioEngine.h"
#include "audio/AudioDeviceManager.h"
#include "audio/RecordingEngine.h"
#include "audio/AudioFile.h"
#include "midi/MidiEngine.h"
#include "plugins/PluginHost.h"
#include "ui/CommandRegistry.h"
#include <QObject>
#include <QTimer>
#include <QStringList>
#include <map>
#include <set>

class QMenu;

namespace mt {

class App : public QObject {
    Q_OBJECT
public:
    explicit App(QObject* parent = nullptr);
    ~App();
    static App* instance();
    void initialize();   // start audio/midi, load plugin db, restore session

    ProjectModel& model() { return m_model; }
    Project& project() { return m_model.project(); }
    const Project& project() const { return m_model.project(); }
    AudioEngine& engine() { return *m_engine; }
    RecordingEngine& recorder() { return *m_recorder; }
    AudioDeviceManager& devices() { return m_devices; }
    SampleCache& cache() { return m_cache; }
    MidiEngine& midi() { return *m_midi; }
    PluginDatabase& pluginDb() { return m_pluginDb; }
    PluginScanner& scanner() { return *m_scanner; }
    CommandRegistry& commands() { return m_commands; }

    // ---- project lifecycle
    void newProject(bool demo = false);
    void retryAudioDevice(int attempt);
    bool openProject(const QString& path);
    bool saveProject();
    bool saveProjectAs(const QString& path);
    bool saveTemplate(const QString& name);
    bool newFromTemplate(const QString& path);
    QStringList recentFiles() const;
    QStringList templates() const;
    QString templatesDir() const;
    QString autosavePath() const;
    bool hasRecoveryFile() const;
    bool recoverAutosave();
    void collectSamples();
    std::vector<AudioSource*> missingSamples();
    void relinkSample(Id sourceId, const QString& newPath);
    void replaceSampleFile(Id sourceId, Id channelId, const QString& newPath);   // the sample editor's Apply (one undo step)
    int searchMissingIn(const QString& dir);
    QString projectTitle() const;
    // Autosave / backups (FL-style): every N minutes a numbered copy goes to <project folder>/Backup, keeping the newest M
    struct AutosaveSettings {
        int intervalMinutes = 5;
        int maxCopies = 10;
        bool enabled = true;
    };
    AutosaveSettings autosaveSettings() const;
    void setAutosaveSettings(const AutosaveSettings& s);
    QString backupDir() const;
    QStringList backups() const;
    bool writeBackupNow(QString* error = nullptr);

    // ---- transport
    void play();
    void stop();
    void pause();
    void togglePlay();
    void record(bool on);
    void seek(Tick t);
    void seekToStart();
    void setBpm(double bpm, bool commit = true);
    void setTimeSig(int n, int d);
    void setSongMode(bool s);
    void setLoop(bool on);
    void setLoopRange(Tick s, Tick e);
    void setMetronome(bool m);
    void setCountIn(bool c);
    void setPunch(bool on, Tick in, Tick out);
    void setLoopRecord(bool on);
    Tick snapTicks() const;
    Tick snapTick(Tick t, bool force = false) const;
    void selectPattern(Id id);
    void selectChannel(Id id);

    // ---- channels
    Id addChannel(ChannelType type, const QString& instrumentKey, const QString& name = QString(), const QString& samplePath = QString(),
        Id pluginInstanceId = 0);
    Id addPluginChannel(const PluginInfo& info);
    void deleteChannel(Id id);
    Id duplicateChannel(Id id);
    void renameChannel(Id id, const QString& name);
    void setChannelColor(Id id, const QColor& c);
    void moveChannel(Id id, int newIndex);
    void setChannelVolume(Id id, float v, bool commit);
    void setChannelPan(Id id, float v, bool commit);
    void setChannelMute(Id id, bool m);
    void setChannelSolo(Id id, bool s);
    void setChannelEnabled(Id id, bool e);
    void setChannelCutItself(Id id, bool on);
    void setChannelMixerTrack(Id id, Id trackId);
    void setChannelSample(Id id, const QString& path);
    void changeChannelInstrument(Id id, const QString& key, Id pluginId = 0);

    // ---- patterns
    Id addPattern(const QString& name = QString());
    void deletePattern(Id id);
    Id clonePattern(Id id);
    std::vector<Id> splitPatternByChannel(
        Id id); /* FL split by channel: one new pattern per channel with content, named after the channel */
    void renamePattern(Id id, const QString& name);
    void setPatternColor(Id id, const QColor& c);
    void setStep(Id patternId, Id channelId, int step, bool enabled);
    void setStepProperty(Id patternId, Id channelId, int step, int which, float value, bool commit);

    // ---- playlist
    // edit one project object (Pattern, Clip, MixerTrack...) as an undoable command, merged with the previous one when mergeId matches
    template <class T> void edit(Id id, const QString& desc, std::function<void(T&)> fn, int mergeId = 0) {
        m_model.edit<T>(id, desc, std::move(fn), mergeId);
    }
    Id addPlaylistTrack(const QString& name = QString());
    Id firstPlaylistTrack();   // where 'add to playlist' puts things; creates a track when there is none
    void deletePlaylistTrack(Id id);
    Id addPatternClip(Id trackId, Id patternId, Tick start, Tick length = 0, bool loopSource = false);
    Id addAudioClip(Id trackId, const QString& file, Tick start);
    Id addAutomationClip(Id trackId, Id laneId, Tick start, Tick length);
    void deleteClips(const std::vector<Id>& ids);
    void splitClip(Id id, Tick at);
    void useTake(Id clipId, Tick from, Tick to);   // comping: this take plays in [from, to), its recording's other takes are muted
    Id duplicateClip(Id id, Tick newStart, Id newTrack);
    void makeClipUnique(Id clipId);
    Id ensureAudioSource(const QString& path);
    Id addMarker(Tick pos, const QString& name);

    // ---- mixer
    Id addMixerTrack(const QString& name = QString());
    void deleteMixerTrack(Id id);
    void setMixerVolume(Id id, float v, bool commit);
    void setMixerPan(Id id, float v, bool commit);
    void setSendLevel(Id from, Id to, float level, bool commit);
    Id addEffect(Id trackId, const QString& key, int slot = -1);
    Id addPluginEffect(Id trackId, const PluginInfo& info, int slot = -1);
    void removeEffect(Id trackId, Id slotId);
    void moveEffect(Id trackId, Id slotId, int newIndex);
    void setEffectBypass(Id trackId, Id slotId, bool b);
    void setEffectMix(Id trackId, Id slotId, float mix);
    bool routeTrack(Id from, Id to);
    void addSend(Id from, Id to, bool sidechain = false);
    void removeSend(Id from, Id to);

    // ---- processors (instruments / effects / plugins)
    IProcessor* processor(Id ownerId) { return m_engine->registry().processor(ownerId); }
    void setProcessorParam(Id ownerId, uint32_t paramId, float normalized, bool commit);
    void savePreset(Id ownerId, const QString& name);
    bool loadPreset(Id ownerId, const QString& file);
    QStringList presetsFor(const QString& key) const;
    QString presetDir(const QString& key) const;

    // ---- automation & MIDI mapping
    Id createAutomationLane(const ParamTarget& target, const QString& name, bool placeClip = true, Id patternId = 0);
    void deleteLane(Id id);
    QString targetName(const ParamTarget& t) const;
    void startMidiLearn(const ParamTarget& target, const QString& name);
    void cancelMidiLearn();
    bool midiLearning() const { return m_learnActive; }
    void removeMidiMapping(int index);
    float targetValue(const ParamTarget& t) const;   // current normalized value

    // ---- live input & preview
    void noteOn(int pitch, float vel = 0.8f);
    void noteOff(int pitch);
    void previewFile(const QString& path);
    void playPreview(std::shared_ptr<PreviewBuffer> b);   // an already filled buffer (the sample editor's selection)
    void stopPreview();
    QString previewPath() const { return m_previewPath; }   // file currently previewed
    std::shared_ptr<const PreviewBuffer> previewBuffer() const { return m_previewBuffer; }
    void panic();

    // ---- audio settings
    bool applyAudioConfig(const AudioConfig& cfg, QString* error);
    bool exportAudio(const struct RenderOptions& opt, std::function<bool(double)> progress, QString* error);

Q_SIGNALS:
    void requestOpenEditor(Id ownerId, const QString& title);
    void statusMessage(const QString& msg, int timeoutMs);
    void projectLoaded();
    void midiLearnChanged(bool active, const QString& name);
    void previewChanged(const QString& path);   // preview started / stopped / loading state changed

private:
    void onModelChanged(uint32_t flags);
    void onRecordedMidi(const MidiEvent& e, double tick);
    void onRecordSegment(double tick, int64_t frame);
    void onTransportStateChanged();
    void beginRecordPass();
    void endRecordPass();
    void onPluginCrashed(Id slotId);
    void onCcReceived(int cc, int channel, int value);
    void autosave();
    void addRecent(const QString& path);
    void recordAutomationPoint(const ParamTarget& t, float normalized);
    // Faders and knobs: the change is written live while dragging and as one merged undo step on release (commit); with the
    // record mode set to Automation every move also records the target's new value. One rule for every continuous control.
    template <class T> void liveControl(Id id, const char* desc, int mergeId, bool commit, std::function<void(T&)> set, const ParamTarget& t);
    void syncTransportToEngine();
    void syncMixerTrackNames();
    void updateSamplerStretch();

    ProjectModel m_model;
    AudioDeviceManager m_devices;
    SampleCache m_cache;
    std::unique_ptr<AudioEngine> m_engine;
    std::unique_ptr<RecordingEngine> m_recorder;
    std::unique_ptr<MidiEngine> m_midi;
    PluginDatabase m_pluginDb;
    std::unique_ptr<PluginScanner> m_scanner;
    std::unique_ptr<PluginHost> m_pluginHost;
    CommandRegistry m_commands;
    QTimer m_autosaveTimer;
    QTimer m_stretchTimer;
    std::map<int, std::pair<double, MidiEvent>> m_pendingNotes;
    bool m_learnActive = false;
    ParamTarget m_learnTarget;
    QString m_learnName;
    std::map<uint32_t, float> m_paramDragStart;
    // the recording pass in progress (Record armed while the transport plays)
    bool m_recPass = false, m_recSong = false;
    Id m_recPatternId = 0;
    Tick m_recClipStart = 0;   // song mode: where notes that land outside the pattern's clips get a new clip
    std::set<Id> m_recNoteIds;
    std::vector<std::pair<double, int64_t>> m_recSegments;   // (tick, recorded frame) from AudioEngine::recordSegment
    QString m_previewPath;
    std::shared_ptr<PreviewBuffer> m_previewBuffer;
    PreviewLoader m_previewLoader;
};

// Rename / Split by channel (one pattern) and Color / Clone (any number): the pattern actions every pattern menu offers
void addPatternActions(QMenu* menu, App& app, QWidget* parent, const std::vector<Id>& ids);
// One checkable entry per mixer track (the current one checked); picking one calls pick(trackId)
void fillMixerTrackMenu(QMenu* menu, const Project& p, Id current, std::function<void(Id)> pick);

}   // namespace mt
