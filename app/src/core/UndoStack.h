// Command-based undo/redo. Every meaningful user action is a Command with execute()/undo().
#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

namespace mt {

class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual QString description() const = 0;
    // Optional merging of successive commands (e.g. continuous knob drags)
    virtual bool mergeWith(const Command*) { return false; }
    virtual int mergeId() const { return 0; }
};

class UndoStack : public QObject {
    Q_OBJECT
public:
    explicit UndoStack(QObject* parent = nullptr)
        : QObject(parent) {}

    void push(std::unique_ptr<Command> cmd) {
        cmd->execute();
        if (!m_undo.empty() && cmd->mergeId() != 0 && m_undo.back()->mergeId() == cmd->mergeId() && m_undo.back()->mergeWith(cmd.get())) {
            Q_EMIT changed();
            return;
        }
        m_undo.push_back(std::move(cmd));
        m_redo.clear();
        if (m_undo.size() > m_limit) m_undo.erase(m_undo.begin());
        Q_EMIT changed();
    }
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    QString undoText() const { return canUndo() ? m_undo.back()->description() : QString(); }
    void undo() {
        if (!canUndo()) return;
        auto c = std::move(m_undo.back());
        m_undo.pop_back();
        c->undo();
        m_redo.push_back(std::move(c));
        Q_EMIT changed();
    }
    void redo() {
        if (!canRedo()) return;
        auto c = std::move(m_redo.back());
        m_redo.pop_back();
        c->execute();
        m_undo.push_back(std::move(c));
        Q_EMIT changed();
    }
    void clear() {
        m_undo.clear();
        m_redo.clear();
        Q_EMIT changed();
    }
    size_t count() const { return m_undo.size(); }
Q_SIGNALS:
    void changed();

private:
    std::vector<std::unique_ptr<Command>> m_undo, m_redo;
    size_t m_limit = 500;
};

}   // namespace mt
