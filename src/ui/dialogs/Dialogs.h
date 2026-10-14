// Settings and utility dialogs.
#pragma once
#include "ui/App.h"
#include "ui/Theme.h"
#include "audio/RenderEngine.h"
#include <QDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QListWidget>
#include <QToolButton>
#include <map>
#include <QProgressBar>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>

namespace mt {

class AudioSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AudioSettingsDialog(App& app, QWidget* parent = nullptr);

private:
    void apply();
    App& m_app;
    QComboBox *m_api, *m_out, *m_in, *m_sr, *m_buf, *m_inCh, *m_outCh;
    QLabel* m_status;
    QCheckBox* m_monitor;
};

class MidiSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit MidiSettingsDialog(App& app, QWidget* parent = nullptr);

private:
    void rebuildMappings();
    App& m_app;
    QListWidget *m_inputs, *m_outputs;
    QCheckBox* m_clock;
    QTableWidget* m_mappings;
};

class PluginManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginManagerDialog(App& app, QWidget* parent = nullptr);

private:
    void rebuild();
    void refreshYabridge();
    void runYabridge(const QStringList& args, bool syncAndScan);
    App& m_app;
    QListWidget* m_dirs;
    QTableWidget* m_table;
    QProgressBar* m_progress;
    QLabel* m_status;
    QLineEdit* m_filter;
    QLabel* m_yabStatus = nullptr;
    QPushButton* m_yabAdd = nullptr;
    QPushButton* m_yabSync = nullptr;
};

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(App& app, QWidget* parent = nullptr);

private:
    void doExport();
    App& m_app;
    QLineEdit* m_path;
    QComboBox *m_format, *m_sr, *m_bits, *m_range;
    QCheckBox *m_mono, *m_normalize, *m_dither, *m_stems;
    QDoubleSpinBox* m_tail;
    QProgressBar* m_progress;
    bool m_cancel = false;
};

class MissingFilesDialog : public QDialog {
    Q_OBJECT
public:
    explicit MissingFilesDialog(App& app, QWidget* parent = nullptr);
    static bool checkAndPrompt(App& app, QWidget* parent);

private:
    void rebuild();
    App& m_app;
    QTableWidget* m_table;
};

class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutsDialog(App& app, QWidget* parent = nullptr);

private:
    App& m_app;
    QTableWidget* m_table;
};

class AutosaveDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutosaveDialog(App& app, QWidget* parent = nullptr);
};

class ProjectInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectInfoDialog(App& app, QWidget* parent = nullptr);

private:
    App& m_app;
    QLineEdit *m_title, *m_author, *m_genre;
    QPlainTextEdit* m_comments;
};

// Theme picker: choose a preset, optionally override individual colours, applied live to every open window.
class ThemeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ThemeDialog(QWidget* parent = nullptr);

private:
    void reloadList(const QString& selectId);
    void selectPalette(const QString& id);
    void apply();   // push m_palette to the whole app
    void refreshSwatches();
    theme::Palette m_palette;
    theme::Palette m_entry;   // working copy, and what was active when the dialog opened
    QListWidget* m_presets;
    std::map<QString, QToolButton*> m_swatches;
    QPushButton *m_saveAs, *m_delete;
};

}   // namespace mt
