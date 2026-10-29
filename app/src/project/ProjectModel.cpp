#include "project/ProjectModel.h"

namespace mt {

ProjectModel::ProjectModel(QObject* parent)
    : QObject(parent) {
    connect(&m_undo, &UndoStack::changed, this, [this] {});
}

void ProjectModel::replaceProject(Project p) {
    m_project = std::move(p);
    m_undo.clear();
    m_dirty = false;
    Q_EMIT changed(ChangeAll);
    Q_EMIT dirtyChanged(false);
}

void ProjectModel::notify(uint32_t flags) {
    m_dirty = true;
    Q_EMIT changed(flags);
    Q_EMIT dirtyChanged(true);
}

void ProjectModel::editTransport(const QString& desc, std::function<void(TransportSettings&)> fn, int mergeId) {
    push(transportCommand(desc, std::move(fn), mergeId));
}
std::unique_ptr<Command> ProjectModel::transportCommand(const QString& desc, std::function<void(TransportSettings&)> fn, int mergeId) {
    struct Cmd : Command {
        ProjectModel& m;
        TransportSettings before, after;
        QString d;
        int mid;
        void execute() override {
            m.project().transport = after;
            m.notify(ChangeTransport);
        }
        void undo() override {
            m.project().transport = before;
            m.notify(ChangeTransport);
        }
        QString description() const override { return d; }
        int mergeId() const override { return mid; }
        bool mergeWith(const Command* o) override {
            auto* c = dynamic_cast<const Cmd*>(o);
            if (!c) return false;
            after = c->after;
            return true;
        }
        Cmd(ProjectModel& mm)
            : m(mm) {}
    };
    auto c = std::make_unique<Cmd>(*this);
    c->before = m_project.transport;
    c->after = c->before;
    fn(c->after);
    c->d = desc;
    c->mid = mergeId;
    return c;
}

void ProjectModel::editMeta(const QString& desc, std::function<void(Project&)> fn) {
    struct Cmd : Command {
        ProjectModel& m;
        ProjectMetadata mb, ma;
        ProjectSettings sb, sa;
        Id spb, spa, scb, sca;
        QString d;
        void execute() override {
            auto& p = m.project();
            p.meta = ma;
            p.settings = sa;
            p.selectedPatternId = spa;
            p.selectedChannelId = sca;
            m.notify(ChangeMeta);
        }
        void undo() override {
            auto& p = m.project();
            p.meta = mb;
            p.settings = sb;
            p.selectedPatternId = spb;
            p.selectedChannelId = scb;
            m.notify(ChangeMeta);
        }
        QString description() const override { return d; }
        Cmd(ProjectModel& mm)
            : m(mm) {}
    };
    auto c = std::make_unique<Cmd>(*this);
    auto& p = m_project;
    c->mb = p.meta;
    c->sb = p.settings;
    c->spb = p.selectedPatternId;
    c->scb = p.selectedChannelId;
    Project tmp = p;
    fn(tmp);
    c->ma = tmp.meta;
    c->sa = tmp.settings;
    c->spa = tmp.selectedPatternId;
    c->sca = tmp.selectedChannelId;
    c->d = desc;
    push(std::move(c));
}

void ProjectModel::macro(const QString& desc, std::vector<std::unique_ptr<Command>> cmds) {
    push(std::make_unique<MacroCommand>(desc, std::move(cmds)));
}

}   // namespace mt
