// SPDX-License-Identifier: LGPL-2.1-or-later
#include "FilletPointEditor.h"

#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QAction>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <Base/Quantity.h>
#include <App/ObjectIdentifier.h>
#include <Gui/QuantitySpinBox.h>

using namespace PartDesignGui;

namespace
{
QString lengthText(double value)
{
    return QString::fromStdString(Base::Quantity(value, Base::Unit::Length).getUserString());
}
QToolButton* button(QWidget* owner, QHBoxLayout* layout, const QString& text)
{
    auto* result = new QToolButton(owner);
    result->setText(text);
    layout->addWidget(result);
    return result;
}
}  // namespace

// A native spin box with two explicit input dimensions. Unlike a quantity bound
// to a single unit, typing '%' or a length changes the point's position mode.
class PartDesignGui::FilletPositionSpinBox: public QAbstractSpinBox
{
public:
    explicit FilletPositionSpinBox(QWidget* parent)
        : QAbstractSpinBox(parent)
    {
        setKeyboardTracking(false);
    }
    QLineEdit* entry() const
    {
        return lineEdit();
    }
    Gui::QuantitySpinBox* appearanceReference = nullptr;
    QSize sizeHint() const override
    {
        auto size = QAbstractSpinBox::sizeHint();
        if (appearanceReference) {
            size.setHeight(appearanceReference->sizeHint().height());
        }
        return size;
    }
    void stepBy(int steps) override
    {
        QString input = text().trimmed();
        const bool percent = input.endsWith(QLatin1Char('%'));
        if (percent) {
            input.chop(1);
        }
        try {
            const auto q = Base::Quantity::parse(input.toStdString());
            const double value = q.getValue() + steps * (percent ? 1. : .1);
            lineEdit()->setText(
                percent ? QString::number(value, 'f', 2) + QStringLiteral("%") : lengthText(value)
            );
            lineEdit()->setModified(true);
            editingFinished();
        }
        catch (const Base::Exception&) {
        }
    }

protected:
    StepEnabled stepEnabled() const override
    {
        return isReadOnly() ? StepNone : StepUpEnabled | StepDownEnabled;
    }
};

// A tightly bounded, click-through sibling, so the leader never covers the viewport's input.
class PartDesignGui::FilletCalloutLeader: public QWidget
{
public:
    explicit FilletCalloutLeader(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("filletCalloutLeader"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }
    void connectTo(const QPointF& point, const QRect& card)
    {
        QPointF end(
            std::clamp(point.x(), double(card.left() + 10), double(card.right() - 10)),
            std::clamp(point.y(), double(card.top() + 10), double(card.bottom() - 10))
        );
        if (point.x() < card.left()) {
            end.setX(card.left());
        }
        else if (point.x() > card.right()) {
            end.setX(card.right());
        }
        else if (point.y() < card.top()) {
            end.setY(card.top());
        }
        else {
            end.setY(card.bottom());
        }
        const QRect area = QRectF(point, end).normalized().adjusted(-4, -4, 4, 4).toAlignedRect();
        setGeometry(area);
        start = point - area.topLeft();
        finish = end - area.topLeft();
        setVisible(!card.contains(point.toPoint()));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(125, 171, 202), 1.2, Qt::DashLine));
        painter.drawLine(start, finish);
        painter.setBrush(QColor(125, 171, 202));
        painter.drawEllipse(start, 2, 2);
    }

private:
    QPointF start, finish;
};

class PartDesignGui::FilletProfile: public QWidget
{
public:
    explicit FilletProfile(FilletPointEditor* owner)
        : QWidget(owner)
        , owner(owner)
    {
        setMinimumHeight(155);
        setFocusPolicy(Qt::StrongFocus);
        setObjectName(QStringLiteral("filletRadiusProfile"));
        setToolTip(tr("Drag points to change position and radius. Shift: fine adjustment. Alt: bypass snapping."));
    }
    std::vector<FilletEditorPoint> points;
    std::string active;
    bool adding = false;
    bool dragging = false;
    std::vector<QPointF> curve;
    void setData(const std::vector<FilletEditorPoint>& value, const std::string& id)
    {
        if (!dragging) {
            points = value;
        }
        active = id;
        update();
    }

protected:
    QRectF plot() const
    {
        return QRectF(42, 25, std::max(1, width() - 60), height() - 55);
    }
    double ceiling() const
    {
        if (dragging) {
            return dragCeiling;
        }
        double value = 1;
        for (const auto& p : points) {
            value = std::max(value, p.radius);
        }
        for (const auto& sample : curve) {
            value = std::max(value, sample.y());
        }
        return value * 1.2;
    }
    QPointF location(const FilletEditorPoint& p) const
    {
        const auto r = plot();
        return {r.left() + p.position * r.width(), r.bottom() - p.radius / ceiling() * r.height()};
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto r = plot();
        const auto colors = owner->fieldPalette();
        painter.fillRect(r, colors.base());
        painter.setPen(colors.mid().color());
        painter.drawRect(r);
        painter.setPen(colors.text().color());
        painter.drawText(QPointF(0, 15), tr("Radius"));
        for (double fraction : {0., .5, 1.}) {
            const double x = r.left() + fraction * r.width();
            painter.drawText(
                QRectF(x - 20, r.bottom() + 4, 40, 18),
                Qt::AlignCenter,
                QString::number(fraction * 100) + QStringLiteral("%")
            );
        }
        painter.drawText(
            QRectF(0, r.top() - 8, 38, 18),
            Qt::AlignRight,
            QString::number(ceiling(), 'g', 3)
        );
        painter.drawText(QRectF(0, r.bottom() - 9, 38, 18), Qt::AlignRight, QStringLiteral("0"));
        QPainterPath path;
        for (size_t i = 0; i < curve.size(); ++i) {
            const QPointF sample(
                r.left() + curve[i].x() * r.width(),
                r.bottom() - curve[i].y() / ceiling() * r.height()
            );
            if (!i) {
                path.moveTo(sample);
            }
            else {
                path.lineTo(sample);
            }
        }
        painter.setPen(QPen(colors.highlight().color(), 1.5));
        painter.save();
        painter.setClipRect(r);
        painter.drawPath(path);
        painter.restore();
        for (const auto& p : points) {
            QRadialGradient gold(location(p) - QPointF(1.5, 2), 7);
            gold.setColorAt(0, QColor(255, 244, 192));
            gold.setColorAt(.4, QColor(237, 180, 53));
            gold.setColorAt(1, QColor(138, 88, 15));
            painter.setBrush(gold);
            painter.setPen(QPen(p.id == active ? colors.highlight().color() : QColor(174, 125, 29), 2));
            painter.drawEllipse(location(p), p.id == active ? 6 : 4, p.id == active ? 6 : 4);
        }
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        int nearest = -1;
        double distance = 16;
        for (size_t i = 0; i < points.size(); ++i) {
            const double d = QLineF(location(points[i]), event->position()).length();
            if (d < distance) {
                distance = d;
                nearest = static_cast<int>(i);
            }
        }
        if (nearest < 0) {
            if (adding && plot().contains(event->position()) && owner->inserted) {
                owner->inserted((event->position().x() - plot().left()) / plot().width());
            }
            return;
        }
        const auto p = points[nearest];
        if (owner->selected) {
            owner->selected(p.id);
        }
        if (p.radiusBound && (p.positionBound || nearest == 0 || nearest + 1 == int(points.size()))) {
            owner->showError(
                tr("This point is expression-driven. Edit or unlink its expression first.")
            );
            return;
        }
        index = nearest;
        initial = p;
        origin = event->position();
        dragCeiling = ceiling();
        dragging = true;
        if (owner->gesture) {
            owner->gesture(true);
        }
        grabMouse();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!dragging) {
            return;
        }
        auto& p = points[index];
        const double fine = event->modifiers().testFlag(Qt::ShiftModifier) ? .1 : 1.;
        const auto delta = (event->position() - origin) * fine;
        double t = initial.position + delta.x() / plot().width();
        if (!event->modifiers().testFlag(Qt::AltModifier)) {
            const double snap = std::round(t * 4) / 4;
            if (std::abs(t - snap) < .015) {
                t = snap;
            }
        }
        if (!p.positionBound && index > 0 && index + 1 < static_cast<int>(points.size())) {
            p.position
                = std::clamp(t, points[index - 1].position + 1e-4, points[index + 1].position - 1e-4);
        }
        if (!p.radiusBound) {
            p.radius = std::max(1e-4, initial.radius - delta.y() / plot().height() * dragCeiling);
        }
        if (owner->edited) {
            owner->edited(p.id, p.position, p.radius, p.absolute);
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!dragging) {
            return;
        }
        const auto p = points[index];
        dragging = false;
        releaseMouse();
        if (owner->edited) {
            owner->edited(p.id, p.position, p.radius, p.absolute);
        }
        if (owner->gesture) {
            owner->gesture(false);
        }
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape && dragging) {
            dragging = false;
            releaseMouse();
            if (owner->action) {
                owner->action("cancel-gesture");
            }
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    FilletPointEditor* owner;
    int index = 0;
    double dragCeiling = 1;
    QPointF origin;
    FilletEditorPoint initial;
};

FilletPointEditor::FilletPointEditor(QWidget* parent, bool overlay)
    : QWidget(parent)
    , overlay(overlay)
{
    setObjectName(overlay ? QStringLiteral("filletInlineEditor") : QStringLiteral("filletPointEditor"));
    setAutoFillBackground(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(overlay ? 12 : 6, overlay ? 12 : 6, overlay ? 12 : 6, overlay ? 12 : 6);
    auto* heading = new QHBoxLayout;
    caption = new QLabel(this);
    heading->addWidget(caption, 1);
    if (overlay) {
        leader = new FilletCalloutLeader(parent);
        grip = button(this, heading, QStringLiteral("⠿"));
        grip->setToolTip(tr("Drag to reposition these dimensions"));
        grip->installEventFilter(this);
        grip->setCursor(Qt::SizeAllCursor);
    }
    else {
        undo = button(this, heading, tr("Undo"));
        redo = button(this, heading, tr("Redo"));
        connect(undo, &QToolButton::clicked, this, [this] {
            if (action) {
                action("undo");
            }
        });
        connect(redo, &QToolButton::clicked, this, [this] {
            if (action) {
                action("redo");
            }
        });
    }
    layout->addLayout(heading);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    position = new FilletPositionSpinBox(this);
    position->setObjectName(QStringLiteral("filletPointPosition"));
    position->setToolTip(tr("Enter a proportion (40%) or a distance along the edge (9.6 mm)."));
    positionFormula = position->entry()->addAction(
        QIcon(QStringLiteral(":/icons/bound-expression-unset.svg")),
        QLineEdit::TrailingPosition
    );
    positionFormula->setObjectName(QStringLiteral("filletPositionExpression"));
    positionFormula->setText(tr("Position expression"));
    positionFormula->setToolTip(
        tr("Edit a position expression. Proportion: 0 to 1. Distance: length units.")
    );
    connect(positionFormula, &QAction::triggered, this, [this] {
        if (action) {
            action("position-expression");
        }
    });
    form->addRow(tr("Position"), position);
    positionHint = new QLabel(this);
    positionHint->setObjectName(QStringLiteral("filletPositionEquivalent"));
    positionHint->setWordWrap(true);
    auto hintFont = positionHint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * .8);
    positionHint->setFont(hintFont);
    form->addRow(QString(), positionHint);
    auto* radiusRow = new QHBoxLayout;
    radius = new Gui::QuantitySpinBox(this);
    radius->setObjectName(QStringLiteral("filletPointRadius"));
    radius->setUnit(Base::Unit::Length);
    radius->setMinimum(0);
    radius->setKeyboardTracking(false);
    radius->setAutoApply(false);
    position->appearanceReference = radius;
    const int fieldWidth = fontMetrics().horizontalAdvance(QStringLiteral("0000.00 mm")) + 70;
    position->setFixedWidth(fieldWidth);
    radius->setFixedWidth(fieldWidth);
    radiusRow->addWidget(radius);
    scrubber = button(this, radiusRow, QStringLiteral("↔"));
    scrubber->setToolTip(tr("Drag horizontally to adjust radius. Shift: fine adjustment."));
    scrubber->setCursor(Qt::SizeHorCursor);
    scrubber->installEventFilter(this);
    form->addRow(tr("Radius"), radiusRow);
    layout->addLayout(form);
    position->installEventFilter(this);
    radius->installEventFilter(this);
    connect(position, &QAbstractSpinBox::editingFinished, this, [this] { commitPosition(); });
    connect(radius, &QAbstractSpinBox::editingFinished, this, [this] { commitRadius(); });
    connect(radius, &Gui::QuantitySpinBox::showFormulaDialog, this, [this](bool shown) {
        if (shown && gesture) {
            gesture(true);
        }
        if (!shown && !updating) {
            radius->apply();
            if (action) {
                action("expression");
            }
            if (gesture) {
                gesture(false);
            }
        }
    });
    error = new QLabel(this);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    revert = new QToolButton(this);
    revert->setText(tr("Revert last edit"));
    revert->hide();
    layout->addWidget(revert);
    connect(revert, &QToolButton::clicked, this, [this] {
        if (action) {
            action("undo");
        }
    });
    if (!overlay) {
        auto* actions = new QHBoxLayout;
        add = button(this, actions, tr("Add point"));
        add->setCheckable(true);
        remove = button(this, actions, tr("Remove"));
        actions->addSpacing(12);
        auto* other = button(this, actions, tr("Select other"));
        actions->addStretch();
        layout->addLayout(actions);
        connect(add, &QToolButton::clicked, this, [this] {
            if (action) {
                action("add");
            }
        });
        connect(remove, &QToolButton::clicked, this, [this] {
            if (action) {
                action("remove");
            }
        });
        connect(other, &QToolButton::clicked, this, [this] {
            if (action) {
                action("next");
            }
        });
        auto* distribution = new QHBoxLayout;
        auto* swap = button(this, distribution, tr("Swap ends"));
        auto* constant = button(this, distribution, tr("Make constant"));
        distribution->addStretch();
        layout->addLayout(distribution);
        connect(swap, &QToolButton::clicked, this, [this] {
            if (action) {
                action("swap");
            }
        });
        connect(constant, &QToolButton::clicked, this, [this] {
            if (action) {
                action("constant");
            }
        });
        auto* toggle = new QToolButton(this);
        toggle->setText(tr("Radius profile"));
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setAutoRaise(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::DownArrow);
        connect(toggle, &QToolButton::toggled, this, [toggle](bool visible) {
            toggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        });
        layout->addWidget(toggle);
        profile = new FilletProfile(this);
        layout->addWidget(profile);
        auto* help
            = new QLabel(tr("Drag to edit · Shift for precision · Alt to bypass snapping"), this);
        help->setWordWrap(true);
        help->setFont(hintFont);
        layout->addWidget(help);
        connect(toggle, &QToolButton::toggled, profile, &QWidget::setVisible);
        connect(toggle, &QToolButton::toggled, help, &QWidget::setVisible);
    }
    setTabOrder(position, radius);
}

FilletPointEditor::~FilletPointEditor()
{
    delete leader.data();
}

void FilletPointEditor::setRadiusCurve(const std::vector<QPointF>& samples)
{
    if (profile) {
        profile->curve = samples;
        profile->update();
    }
}

void FilletPointEditor::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    if (!overlay) {
        painter.fillRect(rect(), panelBackground());
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(background.isValid() ? background : panelBackground());
    painter.setPen(QPen(QColor(125, 171, 202), 1.5));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
}

QPalette FilletPointEditor::fieldPalette() const
{
    // These custom widgets have no Q_OBJECT. Use a polished native input's palette,
    // not QWidget's unstyled Window color (which can still be white in a dark theme).
    auto colors = position->palette();
    colors.setCurrentColorGroup(QPalette::Active);
    return colors;
}

QColor FilletPointEditor::panelBackground() const
{
    for (auto* widget = parentWidget(); widget; widget = widget->parentWidget()) {
        if (widget->property("class").toString() == QStringLiteral("content")) {
            widget->ensurePolished();
            return widget->palette().color(QPalette::Window);
        }
    }
    return fieldPalette().color(QPalette::Window);
}

void FilletPointEditor::setAnchor(const QPointF& position)
{
    anchor = position;
    if (leader && isVisible()) {
        leader->connectTo(anchor, geometry());
        leader->stackUnder(this);
    }
}

void FilletPointEditor::hideEvent(QHideEvent* event)
{
    if (leader) {
        leader->hide();
    }
    QWidget::hideEvent(event);
}

const FilletEditorPoint* FilletPointEditor::point() const
{
    auto p = std::ranges::find(points, activeId, &FilletEditorPoint::id);
    return p == points.end() ? nullptr : &*p;
}

void FilletPointEditor::setPoints(
    const std::vector<FilletEditorPoint>& value,
    const std::string& active,
    double edgeLength
)
{
    updating = true;
    points = value;
    activeId = active;
    length = edgeLength;
    const auto* p = point();
    setEnabled(p != nullptr);
    if (p) {
        caption->setText(
            overlay ? tr("Point %1 · inline dimensions").arg(p - points.data() + 1)
                    : tr("Point %1 of %2").arg(p - points.data() + 1).arg(points.size())
        );
        if (!position->hasFocus()) {
            position->entry()->setText(
                p->absolute ? lengthText(p->position * length)
                            : QString::number(p->position * 100, 'f', 2) + QStringLiteral("%")
            );
        }
        position->setEnabled(p->id != "start" && p->id != "end");
        position->setReadOnly(p->positionBound);
        positionFormula->setEnabled(p->id != "start" && p->id != "end");
        positionHint->setText(
            p->positionBound  ? tr("Expression-driven position")
                : p->absolute ? tr("Proportion · %1%").arg(p->position * 100, 0, 'g', 5)
                              : tr("Distance · %1").arg(lengthText(p->position * length))
        );
        if (!radius->hasFocus()) {
            QSignalBlocker blocker(radius);
            radius->setValue(p->radius);
        }
        scrubber->setEnabled(!p->radiusBound);
        if (remove) {
            remove->setEnabled(p->id != "start" && p->id != "end");
        }
    }
    if (profile) {
        profile->setData(points, activeId);
    }
    updating = false;
}

void FilletPointEditor::bindRadius(const App::ObjectIdentifier& path)
{
    const std::string name = path.toString();
    if (radiusBinding != name) {
        QSignalBlocker blocker(radius);
        radius->unbind();
        radius->bind(path);
        radiusBinding = name;
    }
}

void FilletPointEditor::commitPosition()
{
    const auto* p = point();
    if (updating || !p || !position->entry()->isModified() || p->positionBound) {
        return;
    }
    try {
        QString text = position->text().trimmed();
        const bool absolute = !text.endsWith(QLatin1Char('%'));
        if (!absolute) {
            text.chop(1);
        }
        auto quantity = Base::Quantity::parse(text.toStdString());
        if ((!quantity.isDimensionless() && quantity.getUnit() != Base::Unit::Length)
            || (!absolute && !quantity.isDimensionless()) || length <= 0) {
            throw Base::ValueError("Expected a percentage or length.");
        }
        double t = quantity.getValue() / (absolute ? length : 100.);
        if (!std::isfinite(t) || t <= 0 || t >= 1) {
            throw Base::ValueError("Position must lie inside the edge.");
        }
        position->entry()->setModified(false);
        if (edited) {
            edited(p->id, t, p->radius, absolute);
        }
    }
    catch (const Base::Exception& e) {
        showError(QString::fromUtf8(e.what()));
    }
}

void FilletPointEditor::commitRadius()
{
    const auto* p = point();
    if (updating || !p || p->radiusBound || !radius->hasValidInput()) {
        return;
    }
    if (radius->rawValue() != p->radius && edited) {
        edited(p->id, p->position, radius->rawValue(), p->absolute);
    }
}

bool FilletPointEditor::commitPendingInput()
{
    if (!isVisible() || !isEnabled()) {
        return true;
    }
    commitPosition();
    commitRadius();
    return !position->entry()->isModified() && radius->hasValidInput() && radius->rawValue() > 0
        && error->text().isEmpty();
}

void FilletPointEditor::showError(const QString& message)
{
    error->setText(message);
    error->setVisible(!message.isEmpty());
    revert->setVisible(!message.isEmpty());
}
void FilletPointEditor::setHistoryEnabled(bool canUndo, bool canRedo)
{
    if (undo) {
        undo->setEnabled(canUndo);
    }
    if (redo) {
        redo->setEnabled(canRedo);
    }
}
void FilletPointEditor::setAdding(bool adding)
{
    if (add) {
        QSignalBlocker blocker(add);
        add->setChecked(adding);
    }
    if (profile) {
        profile->adding = adding;
    }
}
void FilletPointEditor::scrub(double amount, bool finished)
{
    if (!point()) {
        return;
    }
    const double value = std::max(1e-4, radiusStart + amount);
    {
        QSignalBlocker blocker(radius);
        radius->setValue(value);
    }
    Q_UNUSED(finished);
    if (edited) {
        edited(point()->id, point()->position, value, point()->absolute);
    }
}
bool FilletPointEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == grip || watched == scrubber) {
        auto* target = qobject_cast<QWidget*>(watched);
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) {
                return false;
            }
            dragStart = mouse->globalPosition().toPoint();
            offsetStart = userOffset;
            radiusStart = radius->rawValue();
            target->grabMouse();
            if (watched == scrubber && gesture) {
                gesture(true);
            }
            return true;
        }
        if (event->type() == QEvent::MouseMove && target->mouseGrabber() == target) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint delta = mouse->globalPosition().toPoint() - dragStart;
            if (watched == grip) {
                userOffset = offsetStart + delta;
                move(pos() + delta);
                setAnchor(anchor);
                dragStart = mouse->globalPosition().toPoint();
                offsetStart = userOffset;
            }
            else {
                scrub(delta.x() * (mouse->modifiers().testFlag(Qt::ShiftModifier) ? .001 : .01), false);
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && target->mouseGrabber() == target) {
            target->releaseMouse();
            if (watched == scrubber) {
                if (point() && edited) {
                    edited(point()->id, point()->position, radius->rawValue(), point()->absolute);
                }
                if (gesture) {
                    gesture(false);
                }
            }
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && (watched == position || watched == radius)) {
            updating = true;
            position->entry()->setModified(false);
            position->clearFocus();
            radius->clearFocus();
            updating = false;
            setPoints(points, activeId, length);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void FilletPointEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo)) {
        if (action) {
            action(event->matches(QKeySequence::Undo) ? "undo" : "redo");
        }
        event->accept();
    }
    else {
        QWidget::keyPressEvent(event);
    }
}
