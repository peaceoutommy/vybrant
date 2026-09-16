// Small window listing every sample the project references (audio clips - even ones no longer in the playlist - and
// channel samples). Preview, drag back into the playlist, locate missing files, purge unused references.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QTableWidget>
#include <QLabel>

namespace mt {

class ProjectSamplesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProjectSamplesPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags);
    QSize sizeHint() const override { return {560, 300}; }
private:
    void rebuild(); void onContext(const QPoint& pos);
    App& m_app; QTableWidget* m_table; QLabel* m_summary;
};

// QTableWidget subclass that drags file URLs
class SampleTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
protected:
    QMimeData* mimeData(const QList<QTableWidgetItem*>& items) const override;
    QStringList mimeTypes() const override { return {"text/uri-list", "application/x-musicthingy-item"}; }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
};

} // namespace mt
