// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <QWidget>
#include <QPoint>
#include <QPointer>

class QLabel;
class QLineEdit;
class QToolButton;
class QAction;

namespace App
{
class ObjectIdentifier;
}
namespace Gui
{
class QuantitySpinBox;
}

namespace PartDesignGui
{
struct FilletEditorPoint
{
    std::string id;
    double position = 0;
    double radius = 1;
    bool absolute = false;
    bool positionBound = false;
    bool radiusBound = false;
};

class FilletProfile;
class FilletCalloutLeader;
class FilletPositionSpinBox;

// Both the task panel and on-model editor consume the same feature point model.
class FilletPointEditor: public QWidget
{
public:
    explicit FilletPointEditor(QWidget* parent, bool overlay = false);
    ~FilletPointEditor() override;
    void setPoints(const std::vector<FilletEditorPoint>& points, const std::string& active, double length);
    void bindRadius(const App::ObjectIdentifier& path);
    void setRadiusCurve(const std::vector<QPointF>& samples);
    void showError(const QString& message);
    void setHistoryEnabled(bool undo, bool redo);
    void setAdding(bool adding);
    bool commitPendingInput();
    void setAnchor(const QPointF& position);
    QPalette fieldPalette() const;
    QColor panelBackground() const;
    QColor background;

    std::function<void(const std::string&, double, double, bool)> edited;
    std::function<void(const std::string&)> selected;
    std::function<void(double)> inserted;
    std::function<void(const std::string&)> action;
    std::function<void(bool)> gesture;
    Gui::QuantitySpinBox* radiusEditor() const
    {
        return radius;
    }
    QPoint userOffset;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void commitPosition();
    void commitRadius();
    void scrub(double amount, bool finished);
    const FilletEditorPoint* point() const;
    std::vector<FilletEditorPoint> points;
    std::string activeId;
    std::string radiusBinding;
    double length = 0;
    bool updating = false;
    bool overlay;
    QLabel* caption;
    FilletPositionSpinBox* position;
    QAction* positionFormula;
    Gui::QuantitySpinBox* radius;
    QLabel* positionHint;
    QLabel* error;
    QToolButton* revert;
    QToolButton* undo = nullptr;
    QToolButton* redo = nullptr;
    QToolButton* add = nullptr;
    QToolButton* remove = nullptr;
    QToolButton* grip = nullptr;
    QToolButton* scrubber;
    FilletProfile* profile = nullptr;
    QPointer<FilletCalloutLeader> leader;
    QPointF anchor;
    QPoint dragStart;
    QPoint offsetStart;
    double radiusStart = 0;
};
}  // namespace PartDesignGui
