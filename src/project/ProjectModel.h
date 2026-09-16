// QObject wrapper around Project that owns the undo stack and broadcasts change notifications.
// The UI never mutates Project directly; it pushes Commands through this model.
#pragma once
#include "project/Project.h"
#include "core/UndoStack.h"
#include <QObject>
#include <functional>
#include <memory>

namespace mt {

class ProjectModel : public QObject {
    Q_OBJECT
public:
    explicit ProjectModel(QObject* parent = nullptr);
    Project& project() { return m_project; }
    const Project& project() const { return m_project; }
    UndoStack& undoStack() { return m_undo; }

    void replaceProject(Project p);            // load / new (clears undo)
    void notify(uint32_t flags);              // called by commands after mutation
    void push(std::unique_ptr<Command> cmd) { m_undo.push(std::move(cmd)); m_dirty = true; }
    bool isDirty() const { return m_dirty; }
    void setDirty(bool d) { m_dirty = d; Q_EMIT dirtyChanged(d); }
    QString filePath() const { return m_filePath; }
    void setFilePath(const QString& p) { m_filePath = p; }

    // ---- convenience helpers (each pushes an undoable command) ----
    template <class T> std::vector<T>& vec();
    template <class T> uint32_t flagsFor();

    // Edit a single object by id in-place using a mutator; the command stores before/after copies of that object only.
    template <class T> void edit(Id id, const QString& desc, std::function<void(T&)> fn, int mergeId = 0);
    template <class T> void insert(T obj, const QString& desc, int index = -1);
    template <class T> void remove(Id id, const QString& desc);
    template <class T> void reorder(Id id, int newIndex, const QString& desc);
    void editTransport(const QString& desc, std::function<void(TransportSettings&)> fn, int mergeId = 0);
    void editMeta(const QString& desc, std::function<void(Project&)> fn); // meta/settings/selection (no structural change)
    void macro(const QString& desc, std::vector<std::unique_ptr<Command>> cmds);

Q_SIGNALS:
    void changed(uint32_t flags);
    void projectReplaced();
    void dirtyChanged(bool);

private:
    Project m_project;
    UndoStack m_undo;
    QString m_filePath;
    bool m_dirty = false;
};

// ---------------- Generic command implementations ----------------
template <class T>
class EditObjectCommand : public Command {
public:
    EditObjectCommand(ProjectModel& m, Id id, QString desc, std::function<void(T&)> fn, int mergeId)
        : m_model(m), m_id(id), m_desc(std::move(desc)), m_mergeId(mergeId) {
        T* obj = Project::findIn(m.vec<T>(), id);
        if (obj) { m_before = *obj; m_after = *obj; fn(m_after); m_valid = true; }
    }
    void execute() override { apply(m_after); }
    void undo() override { apply(m_before); }
    QString description() const override { return m_desc; }
    int mergeId() const override { return m_mergeId; }
    bool mergeWith(const Command* other) override {
        auto* o = dynamic_cast<const EditObjectCommand<T>*>(other);
        if (!o || o->m_id != m_id) return false;
        m_after = o->m_after; return true;
    }
private:
    void apply(const T& v) { if (!m_valid) return; T* obj = Project::findIn(m_model.vec<T>(), m_id); if (obj) *obj = v; m_model.notify(m_model.flagsFor<T>()); }
    ProjectModel& m_model; Id m_id; QString m_desc; int m_mergeId; T m_before, m_after; bool m_valid = false;
};

template <class T>
class InsertObjectCommand : public Command {
public:
    InsertObjectCommand(ProjectModel& m, T obj, QString desc, int index) : m_model(m), m_obj(std::move(obj)), m_desc(std::move(desc)), m_index(index) {}
    void execute() override { auto& v = m_model.vec<T>(); int idx = (m_index < 0 || m_index > (int)v.size()) ? (int)v.size() : m_index; v.insert(v.begin() + idx, m_obj); m_model.notify(m_model.flagsFor<T>() | ChangeStructure); }
    void undo() override { auto& v = m_model.vec<T>(); for (size_t i = 0; i < v.size(); ++i) if (v[i].id == m_obj.id) { v.erase(v.begin() + i); break; } m_model.notify(m_model.flagsFor<T>() | ChangeStructure); }
    QString description() const override { return m_desc; }
private:
    ProjectModel& m_model; T m_obj; QString m_desc; int m_index;
};

template <class T>
class RemoveObjectCommand : public Command {
public:
    RemoveObjectCommand(ProjectModel& m, Id id, QString desc) : m_model(m), m_id(id), m_desc(std::move(desc)) {}
    void execute() override { auto& v = m_model.vec<T>(); for (size_t i = 0; i < v.size(); ++i) if (v[i].id == m_id) { m_obj = v[i]; m_index = (int)i; v.erase(v.begin() + i); break; } m_model.notify(m_model.flagsFor<T>() | ChangeStructure); }
    void undo() override { auto& v = m_model.vec<T>(); int idx = std::min(m_index, (int)v.size()); v.insert(v.begin() + idx, m_obj); m_model.notify(m_model.flagsFor<T>() | ChangeStructure); }
    QString description() const override { return m_desc; }
private:
    ProjectModel& m_model; Id m_id; QString m_desc; T m_obj; int m_index = 0;
};

template <class T>
class ReorderCommand : public Command {
public:
    ReorderCommand(ProjectModel& m, Id id, int newIndex, QString desc) : m_model(m), m_id(id), m_new(newIndex), m_desc(std::move(desc)) {}
    void execute() override { auto& v = m_model.vec<T>(); for (size_t i = 0; i < v.size(); ++i) if (v[i].id == m_id) { m_old = (int)i; break; } move(m_old, m_new); }
    void undo() override { move(m_new, m_old); }
    QString description() const override { return m_desc; }
private:
    void move(int from, int to) { auto& v = m_model.vec<T>(); if (from < 0 || from >= (int)v.size()) return; to = clamp(to, 0, (int)v.size() - 1); T tmp = v[from]; v.erase(v.begin() + from); v.insert(v.begin() + to, tmp); m_model.notify(m_model.flagsFor<T>() | ChangeStructure); }
    ProjectModel& m_model; Id m_id; int m_old = 0, m_new; QString m_desc;
};

class LambdaCommand : public Command {
public:
    LambdaCommand(QString desc, std::function<void()> exec, std::function<void()> undo) : m_desc(std::move(desc)), m_exec(std::move(exec)), m_undo(std::move(undo)) {}
    void execute() override { m_exec(); }
    void undo() override { m_undo(); }
    QString description() const override { return m_desc; }
private:
    QString m_desc; std::function<void()> m_exec, m_undo;
};

class MacroCommand : public Command {
public:
    MacroCommand(QString desc, std::vector<std::unique_ptr<Command>> cmds) : m_desc(std::move(desc)), m_cmds(std::move(cmds)) {}
    void execute() override { for (auto& c : m_cmds) c->execute(); }
    void undo() override { for (auto it = m_cmds.rbegin(); it != m_cmds.rend(); ++it) (*it)->undo(); }
    QString description() const override { return m_desc; }
private:
    QString m_desc; std::vector<std::unique_ptr<Command>> m_cmds;
};

// Parameter change on a runtime processor (instrument/effect/plugin). Applies through a setter so the
// engine parameter atomics are updated; the project's saved state is synchronised at save time.
class ProcessorParamCommand : public Command {
public:
    ProcessorParamCommand(QString desc, std::function<void(float)> setter, float before, float after, int mergeId, std::function<void()> notify)
        : m_desc(std::move(desc)), m_set(std::move(setter)), m_before(before), m_after(after), m_mergeId(mergeId), m_notify(std::move(notify)) {}
    void execute() override { m_set(m_after); if (m_notify) m_notify(); }
    void undo() override { m_set(m_before); if (m_notify) m_notify(); }
    QString description() const override { return m_desc; }
    int mergeId() const override { return m_mergeId; }
    bool mergeWith(const Command* o) override { auto* c = dynamic_cast<const ProcessorParamCommand*>(o); if (!c) return false; m_after = c->m_after; return true; }
private:
    QString m_desc; std::function<void(float)> m_set; float m_before, m_after; int m_mergeId; std::function<void()> m_notify;
};

// --- template specialisations ---
template <> inline std::vector<Pattern>& ProjectModel::vec<Pattern>() { return m_project.patterns; }
template <> inline std::vector<Channel>& ProjectModel::vec<Channel>() { return m_project.channels; }
template <> inline std::vector<PlaylistTrack>& ProjectModel::vec<PlaylistTrack>() { return m_project.playlistTracks; }
template <> inline std::vector<Clip>& ProjectModel::vec<Clip>() { return m_project.clips; }
template <> inline std::vector<MixerTrack>& ProjectModel::vec<MixerTrack>() { return m_project.mixerTracks; }
template <> inline std::vector<PluginInstanceData>& ProjectModel::vec<PluginInstanceData>() { return m_project.plugins; }
template <> inline std::vector<AutomationLane>& ProjectModel::vec<AutomationLane>() { return m_project.automationLanes; }
template <> inline std::vector<AudioSource>& ProjectModel::vec<AudioSource>() { return m_project.audioSources; }
template <> inline std::vector<Marker>& ProjectModel::vec<Marker>() { return m_project.markers; }
template <> inline uint32_t ProjectModel::flagsFor<Pattern>() { return ChangePatterns; }
template <> inline uint32_t ProjectModel::flagsFor<Channel>() { return ChangeChannels; }
template <> inline uint32_t ProjectModel::flagsFor<PlaylistTrack>() { return ChangePlaylist; }
template <> inline uint32_t ProjectModel::flagsFor<Clip>() { return ChangePlaylist; }
template <> inline uint32_t ProjectModel::flagsFor<MixerTrack>() { return ChangeMixer; }
template <> inline uint32_t ProjectModel::flagsFor<PluginInstanceData>() { return ChangePlugins; }
template <> inline uint32_t ProjectModel::flagsFor<AutomationLane>() { return ChangeAutomation; }
template <> inline uint32_t ProjectModel::flagsFor<AudioSource>() { return ChangeAudio; }
template <> inline uint32_t ProjectModel::flagsFor<Marker>() { return ChangePlaylist; }

template <class T> void ProjectModel::edit(Id id, const QString& desc, std::function<void(T&)> fn, int mergeId) { push(std::make_unique<EditObjectCommand<T>>(*this, id, desc, std::move(fn), mergeId)); }
template <class T> void ProjectModel::insert(T obj, const QString& desc, int index) { push(std::make_unique<InsertObjectCommand<T>>(*this, std::move(obj), desc, index)); }
template <class T> void ProjectModel::remove(Id id, const QString& desc) { push(std::make_unique<RemoveObjectCommand<T>>(*this, id, desc)); }
template <class T> void ProjectModel::reorder(Id id, int newIndex, const QString& desc) { push(std::make_unique<ReorderCommand<T>>(*this, id, newIndex, desc)); }

} // namespace mt
