// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include <QAction>
#include <QFormLayout>
#include <QTreeWidgetItemIterator>
#include <QComboBox>
#include <QHeaderView>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTreeWidget>
#include <QKeyEvent>
#include <Inventor/nodes/SoSphere.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>

#include <Base/Interpreter.h>
#include <Base/Converter.h>
#include <Base/ServiceProvider.h>
#include <Base/Quantity.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Inventor/Draggers/SoLinearDragger.h>
#include <Gui/Inventor/Draggers/GizmoStyleParameters.h>
#include <Gui/Inventor/Draggers/SoLinearDraggerGeometry.h>
#include <Gui/Selection/Selection.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Utilities.h>
#include <Gui/ViewProvider.h>
#include <Gui/Document.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Mod/PartDesign/App/FeatureFillet.h>
#include <Mod/Part/App/Attacher.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Part/App/Tools.h>
#include <Mod/Part/App/GizmoHelper.h>

#include "ui_TaskFilletParameters.h"
#include "TaskFilletParameters.h"


using namespace PartDesignGui;
using namespace Gui;

/* TRANSLATOR PartDesignGui::TaskFilletParameters */

namespace
{
constexpr double controlPointTolerance = 1.0e-4;

// Keep normalized values and expressions in the model; only format as percentages.
class FilletPercentSpinBox: public Gui::DoubleSpinBox
{
public:
    using Gui::DoubleSpinBox::DoubleSpinBox;
    QString textFromValue(double value) const override
    {
        return locale().toString(value * 100, 'f', 2);
    }
    double valueFromText(const QString& text) const override
    {
        QString number = text;
        number.remove(QLatin1Char('%'));
        return locale().toDouble(number.trimmed()) / 100;
    }
    QValidator::State validate(QString& text, int&) const override
    {
        QString number = text;
        number.remove(QLatin1Char('%'));
        if (number.trimmed().isEmpty()) {
            return QValidator::Intermediate;
        }
        bool valid = false;
        const double value = locale().toDouble(number.trimmed(), &valid);
        return valid && value >= 0 && value <= 100 ? QValidator::Acceptable : QValidator::Invalid;
    }
};

class FilletRadiusGizmo: public Gui::LinearGizmo
{
public:
    FilletRadiusGizmo(Gui::QuantitySpinBox* editor, std::function<void(bool)> gesture)
        : LinearGizmo(editor)
        , gesture(std::move(gesture))
    {}
    SoInteractionKit* initDragger() override
    {
        auto* result = LinearGizmo::initDragger();
        auto* dragger = getDraggerContainer()->getDragger();
        dragger->setName("filletRadiusHandle");
        auto* arrow = SO_GET_PART(dragger, "arrow", Gui::SoArrowGeometry);
        arrow->setName("filletNativeRadiusArrow");
        dragger->addStartCallback(start, this);
        dragger->addFinishCallback(finish, this);
        return result;
    }
    void uninitDragger() override
    {
        auto* dragger = getDraggerContainer()->getDragger();
        dragger->removeStartCallback(start, this);
        dragger->removeFinishCallback(finish, this);
        LinearGizmo::uninitDragger();
    }

private:
    static void start(void* data, SoDragger*)
    {
        auto* self = static_cast<FilletRadiusGizmo*>(data);
        // Variable fillets always preview while dragging, independent of the
        // generic gizmo delay preference. The base finish callback restores it.
        self->property->blockSignals(false);
        self->gesture(true);
    }
    static void finish(void* data, SoDragger*)
    {
        static_cast<FilletRadiusGizmo*>(data)->gesture(false);
    }
    std::function<void(bool)> gesture;
};

struct EdgePointFrame
{
    Base::Vector3d position;
    Base::Vector3d tangent;
    double length;
};

struct RadiusDraggerFrame
{
    DraggerPlacementProps first;
    DraggerPlacementProps second;
    double correction;
};

RadiusDraggerFrame radiusDraggerFrame(Part::TopoShape& edge, Part::TopoShape& baseShape)
{
    auto [face1, face2] = getAdjacentFacesFromEdge(edge, baseShape);
    const auto first = getDraggerPlacementFromEdgeAndFace(edge, face1);
    const auto second = getDraggerPlacementFromEdgeAndFace(edge, face2);
    // Convert radius to drag distance when the adjacent faces are not orthogonal.
    const double tangent = std::tan(first.dir.GetAngle(second.dir) / 2.0);
    return {first, second, std::abs(tangent) > Precision::Angular() ? 1.0 / tangent : 1.0};
}

std::optional<EdgePointFrame> evaluateEdgePosition(const TopoDS_Edge& edge, double position)
{
    BRepAdaptor_Curve curve(edge);
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();
    const double length = GCPnts_AbscissaPoint::Length(curve, first, last, Precision::Confusion());
    if (length <= Precision::Confusion()) {
        return std::nullopt;
    }

    // OCCT parameterizes a variable-radius law along the fillet spine's
    // underlying curve direction, independently of the selected edge's orientation.
    double parameter = first;
    if (position > controlPointTolerance && position < 1.0 - controlPointTolerance) {
        GCPnts_AbscissaPoint solver(curve, position * length, parameter, Precision::Confusion());
        if (!solver.IsDone()) {
            return std::nullopt;
        }
        parameter = solver.Parameter();
    }
    else if (position >= 1.0 - controlPointTolerance) {
        parameter = last;
    }

    gp_Pnt point;
    gp_Vec tangent;
    curve.D1(parameter, point, tangent);
    if (tangent.SquareMagnitude() <= gp::Resolution()) {
        return std::nullopt;
    }
    tangent.Normalize();
    return EdgePointFrame {
        Base::Vector3d(point.X(), point.Y(), point.Z()),
        Base::Vector3d(tangent.X(), tangent.Y(), tangent.Z()),
        length
    };
}

std::optional<double> projectPointToEdge(const TopoDS_Edge& edge, const gp_Pnt& point)
{
    double first = 0.0;
    double last = 0.0;
    auto curve = BRep_Tool::Curve(edge, first, last);
    if (curve.IsNull()) {
        return std::nullopt;
    }

    GeomAPI_ProjectPointOnCurve projector(point, curve, first, last);
    if (projector.NbPoints() == 0) {
        return std::nullopt;
    }

    BRepAdaptor_Curve adaptor(edge);
    const double parameter = projector.LowerDistanceParameter();
    const double total = GCPnts_AbscissaPoint::Length(adaptor, first, last, Precision::Confusion());
    if (total <= Precision::Confusion()) {
        return std::nullopt;
    }
    const double position
        = GCPnts_AbscissaPoint::Length(adaptor, first, parameter, Precision::Confusion()) / total;
    return std::clamp(position, 0.0, 1.0);
}
}  // namespace

namespace PartDesignGui
{

class EdgePositionGizmo: public Gui::Gizmo
{
public:
    using Callback = std::function<void(double, bool)>;

    EdgePositionGizmo(
        double position,
        double edgeLength,
        Callback callback,
        std::function<void()> activationCallback,
        bool fixed = false
    )
        : position(position)
        , edgeLength(edgeLength)
        , callback(std::move(callback))
        , activationCallback(std::move(activationCallback))
        , fixed(fixed)
    {}

    SoInteractionKit* initDragger() override
    {
        container = new Gui::SoLinearDraggerContainer;
        dragger = container->getDragger();
        setSelected(false);
        dragger->setName("filletPositionHandle");
        dragger->labelVisible = false;
        auto* geometry = new Gui::SoSphereGeometry;
        geometry->radius = geometry->radius.getValue() * (fixed ? 1.F : .8F);
        auto* sphere = SO_GET_PART(geometry, "sphere", SoSphere);
        sphere->setName(fixed ? "filletEndpointBall" : "filletControlPointBall");
        dragger->setPart("arrow", geometry);
        dragger->addStartCallback(&EdgePositionGizmo::startCallback, this);
        dragger->addMotionCallback(&EdgePositionGizmo::motionCallback, this);
        dragger->addFinishCallback(&EdgePositionGizmo::finishCallback, this);
        return container;
    }

    void uninitDragger() override
    {
        if (dragger) {
            dragger->removeStartCallback(&EdgePositionGizmo::startCallback, this);
            dragger->removeMotionCallback(&EdgePositionGizmo::motionCallback, this);
            dragger->removeFinishCallback(&EdgePositionGizmo::finishCallback, this);
        }
        dragger = nullptr;
        container = nullptr;
    }

    Gui::GizmoPlacement getDraggerPlacement() override
    {
        return {container->translation.getValue(), container->getPointerDirection()};
    }

    void setDraggerPlacement(const SbVec3f& pos, const SbVec3f& dir) override
    {
        container->translation = pos;
        container->setPointerDirection(dir);
    }

    void setGeometryScale(float scale) override
    {
        if (!(scale > 0.0F) || !std::isfinite(scale)) {
            return;
        }
        dragger->geometryScale = SbVec3f(scale, scale, scale);
        dragger->translationIncrement = std::pow(10.0, std::floor(std::log10(scale)));
    }

    void setVisibility(bool visible)
    {
        container->visible = visible;
    }

    bool isDragging() const
    {
        return dragging;
    }

    void setSelected(bool selected)
    {
        if (dragger) {
            auto* styles = Base::provideService<Gui::StyleParameters::ParameterManager>();
            dragger->activeColor = styles->resolve(
                Gui::StyleParameters::LinearGizmoActiveColor
            ).asValue<SbColor>();
            dragger->color = selected ? dragger->activeColor.getValue()
                                     : styles->resolve(Gui::StyleParameters::LinearGizmoBaseColor)
                                           .asValue<SbColor>();
        }
    }

    void setPosition(double value, double length)
    {
        position = value;
        edgeLength = length;
        if (dragger && !dragging) {
            dragger->translation = SbVec3f(0, 0, 0);
            dragger->translationIncrementCount = 0;
        }
    }

private:
    static void startCallback(void* data, SoDragger*)
    {
        auto* self = static_cast<EdgePositionGizmo*>(data);
        self->dragging = true;
        if (self->activationCallback) {
            self->activationCallback();
        }
        self->dragStartPosition = self->position;
        self->dragger->translationIncrementCount = 0;
    }

    static void motionCallback(void* data, SoDragger*)
    {
        static_cast<EdgePositionGizmo*>(data)->update(false);
    }

    static void finishCallback(void* data, SoDragger*)
    {
        auto* self = static_cast<EdgePositionGizmo*>(data);
        self->dragging = false;
        self->update(true);
    }

    void update(bool finished)
    {
        if (fixed) {
            dragger->translation = SbVec3f(0, 0, 0);
            dragger->translationIncrementCount = 0;
            return;
        }
        if (edgeLength <= Precision::Confusion()) {
            return;
        }
        const auto modifiers = QApplication::queryKeyboardModifiers();
        const double fine = modifiers.testFlag(Gui::GizmoContainer::getFineSnapModifier()) ? .1 : 1.;
        const double distance = dragger->translationIncrementCount.getValue()
            * dragger->translationIncrement.getValue() * fine;
        double target = dragStartPosition + distance / edgeLength;
        if (!modifiers.testFlag(Qt::AltModifier)) {
            const double snap = std::round(target * 4) / 4;
            if (std::abs(target - snap) < .015) {
                target = snap;
            }
        }
        callback(std::clamp(target, 0.0, 1.0), finished);
    }

    Gui::SoLinearDraggerContainer* container = nullptr;
    Gui::SoLinearDragger* dragger = nullptr;
    double position;
    double dragStartPosition = 0.0;
    double edgeLength;
    bool dragging = false;
    Callback callback;
    std::function<void()> activationCallback;
    bool fixed;
};
}  // namespace PartDesignGui

TaskFilletParameters::TaskFilletParameters(ViewProviderDressUp* DressUpView, QWidget* parent)
    : TaskDressUpParameters(DressUpView, true, true, parent)
    , ui(new Ui_TaskFilletParameters)
{
    // we need a separate container widget to add all controls to
    proxy = new QWidget(this);
    ui->setupUi(proxy);
    this->groupLayout()->addWidget(proxy);
    setupTaskPanel();

    PartDesign::Fillet* pcFillet = DressUpView->getObject<PartDesign::Fillet>();
    filletChangedConnection = pcFillet->signalChanged.connect(
        [this, pcFillet](const App::DocumentObject&, const App::Property& property) {
            const bool valuesChanged = &property == &pcFillet->VariableRadiusControlPointValues;
            const bool radiusChanged = &property == &pcFillet->Radius;
            const bool expressionsChanged = &property == &pcFillet->ExpressionEngine;
            const bool shapeChanged = &property == &pcFillet->Shape;
            if (!valuesChanged && !radiusChanged && !expressionsChanged && !shapeChanged) {
                return;
            }
            if (controlPointRefreshQueued) {
                return;
            }
            controlPointRefreshQueued = true;
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    if (controlPointRefreshQueued) {
                        refreshControlPointValuesFromModel();
                    }
                },
                Qt::QueuedConnection
            );
        }
    );
    const bool variableRadius = pcFillet->RadiusMode.getValue()
        == static_cast<int>(PartDesign::Fillet::RadiusModeValue::Variable);
    bool useAllEdges = pcFillet->UseAllEdges.getValue();
    if (variableRadius && useAllEdges) {
        pcFillet->UseAllEdges.setValue(false);
        pcFillet->recomputeFeature();
        useAllEdges = false;
    }
    ui->checkBoxUseAllEdges->setChecked(useAllEdges);
    ui->buttonRefSel->setEnabled(!useAllEdges);
    ui->listWidgetReferences->setEnabled(!useAllEdges);
    ui->treeWidgetReferences->setEnabled(!useAllEdges);
    double r = pcFillet->Radius.getValue();
    defaultRadius = r;
    ui->filletType->setCurrentIndex(pcFillet->RadiusMode.getValue());
    allowFaces = true;

    for (auto* editor : {ui->filletRadius, ui->filletEndRadius, ui->defaultRadiusEditor}) {
        editor->setUnit(Base::Unit::Length);
        editor->setValue(r);
        editor->setMinimum(0);
    }
    ui->defaultRadiusEditor->bind(pcFillet->Radius);
    ui->defaultRadiusEditor->setAutoApply(true);

    ui->treeWidgetReferences->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->treeWidgetReferences->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);


    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    QMetaObject::invokeMethod(
        ui->defaultRadiusEditor,
        [editor = ui->defaultRadiusEditor]() {
            editor->setFocus();
            editor->selectNumber();
        },
        Qt::QueuedConnection
    );
    std::vector<std::string> strings = pcFillet->Base.getSubValues();
    for (const auto& [name, edge] : radiusEdges()) {
        if (std::ranges::find(strings, name) == strings.end()) {
            strings.push_back(name);
        }
    }
    for (const auto& string : strings) {
        ui->listWidgetReferences->addItem(QString::fromStdString(string));
        auto law = pcFillet->getRadiusLaw(string, edgeLength(string));
        EdgeRadii radii {r, r, {}};
        if (law.size() >= 2) {
            radii.start = law.front().radius;
            radii.end = law.back().radius;
            auto ids = pcFillet->getRadiusControlPointIds(string);
            const std::size_t controlPointCount = law.size() - 2;
            if (ids.size() != controlPointCount) {
                ids.clear();
                for (std::size_t i = 0; i < controlPointCount; ++i) {
                    ids.push_back("cp" + std::to_string(i + 1));
                }
                pcFillet->setRadiusControlPointIds(string, ids);
            }
            for (std::size_t i = 0; i < controlPointCount; ++i) {
                radii.controlPoints.push_back({law[i + 1].position, law[i + 1].radius, ids[i]});
            }
        }
        edgeRadii.emplace(string, std::move(radii));
    }

    QMetaObject::connectSlotsByName(this);

    // clang-format off
    connect(ui->filletRadius, qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this, &TaskFilletParameters::onStartRadiusChanged);
    connect(ui->filletEndRadius, qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this, &TaskFilletParameters::onEndRadiusChanged);
    for (auto* editor : {ui->filletRadius, ui->filletEndRadius}) {
        connect(editor, &Gui::QuantitySpinBox::showFormulaDialog, this, [this](bool shown) {
            if (!shown) {
                updatePreview();
            }
        });
    }
    connect(ui->filletType, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &TaskFilletParameters::onFilletTypeChanged);
    connect(ui->addControlPointButton, &QToolButton::toggled,
        this, &TaskFilletParameters::setAddControlPointMode);

    connect(ui->defaultRadiusEditor, qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this, &TaskFilletParameters::onDefaultRadiusChanged);
    connect(ui->buttonRefSel, &QToolButton::toggled,
        this, &TaskFilletParameters::onButtonRefSel);
    connect(ui->checkBoxUseAllEdges, &QToolButton::toggled,
        this, &TaskFilletParameters::onCheckBoxUseAllEdgesToggled);

    // Create context menu
    createDeleteAction(ui->listWidgetReferences);
    ui->treeWidgetReferences->addAction(deleteAction);
    ui->treeWidgetReferences->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(deleteAction, &QAction::triggered, this, &TaskFilletParameters::onRefDeleted);

    createAddAllEdgesAction(ui->listWidgetReferences);
    ui->treeWidgetReferences->addAction(addAllEdgesAction);
    connect(addAllEdgesAction, &QAction::triggered, this, &TaskFilletParameters::onAddAllEdges);

    connect(ui->listWidgetReferences, &QListWidget::currentItemChanged,
        this, &TaskFilletParameters::setSelection);
    connect(ui->listWidgetReferences, &QListWidget::currentItemChanged,
        this, &TaskFilletParameters::onCurrentEdgeChanged);
    connect(ui->listWidgetReferences, &QListWidget::itemClicked,
        this, &TaskFilletParameters::setSelection);
    connect(ui->listWidgetReferences, &QListWidget::itemDoubleClicked,
        this, &TaskFilletParameters::doubleClicked);
    connect(ui->listWidgetReferences, &QListWidget::currentItemChanged,
        this, [this](QListWidgetItem* current) {
            if (current) {
                selectEdgeTreeItem(current->text());
            }
        });
    connect(ui->treeWidgetReferences, &QTreeWidget::currentItemChanged,
        this, [this](QTreeWidgetItem*, QTreeWidgetItem*) { syncEdgeTreeSelection(); });
    connect(ui->treeWidgetReferences, &QTreeWidget::itemSelectionChanged,
        this, &TaskFilletParameters::syncEdgeTreeSelection);
    // clang-format on

    if (strings.empty()) {
        setSelectionMode(refSel);
    }
    else {
        hideOnError();
    }

    refreshReferences();
    refreshEdgeTree();
    rebuildGizmos();
    if (GizmoContainer::isEnabled()) {
        showDraggerHints();
    }
    updateFilletTypeUi();
    ensureCurrentEdge();
}

void TaskFilletParameters::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    // executed when the user selected something in the CAD object
    // adds/deletes the selection accordingly

    if (addingControlPoint && msg.Type == Gui::SelectionChanges::AddSelection) {
        const bool added = addControlPointFromSelection(msg);
        Gui::Selection().clearSelection();
        if (added) {
            setAddControlPointMode(false);
        }
        return;
    }

    if (msg.Type == Gui::SelectionChanges::AddSelection) {
        if (selectionMode == refSel) {
            auto* fillet = getObject<PartDesign::Fillet>();
            auto* base = getBase();
            if (!fillet || !base || strcmp(msg.pDocName, fillet->getDocument()->getName()) != 0
                || strcmp(msg.pObjectName, base->getNameInDocument()) != 0) {
                return;
            }

            const std::string subName(msg.pSubName);
            referenceSelected(msg, ui->listWidgetReferences);

            const auto matches = ui->listWidgetReferences->findItems(
                QString::fromStdString(subName),
                Qt::MatchExactly
            );
            if (!matches.empty()) {
                const bool inserted
                    = edgeRadii.try_emplace(subName, EdgeRadii {defaultRadius, defaultRadius, {}}).second;
                ui->listWidgetReferences->setCurrentItem(matches.front());
                if (inserted && isVariableRadius() && edgeShape(subName)) {
                    syncRadiusLaw(subName);
                }
            }
            else {
                edgeRadii.erase(subName);
                ensureCurrentEdge();
            }
            refreshReferences();
            refreshEdgeTree();
            rebuildAllGizmos();
        }
    }
    else if (msg.Type == Gui::SelectionChanges::ClrSelection) {
        // TODO: the gizmo position should be only recalculated when the feature associated
        // with the gizmo is removed from the list
        setGizmoPositions();
    }
}

void TaskFilletParameters::onCheckBoxUseAllEdgesToggled(bool checked)
{
    if (checked && isVariableRadius()) {
        QSignalBlocker blocker(ui->checkBoxUseAllEdges);
        ui->checkBoxUseAllEdges->setChecked(false);
        return;
    }

    if (auto fillet = getObject<PartDesign::Fillet>()) {
        if (checked) {
            setAddControlPointMode(false);
            setSelectionMode(none);
        }

        ui->buttonRefSel->setEnabled(!checked);
        ui->listWidgetReferences->setEnabled(!checked);
        ui->treeWidgetReferences->setEnabled(!checked);
        ui->controlPointTable->setEnabled(!checked);
        ui->addControlPointButton->setEnabled(
            !checked && isVariableRadius() && ui->listWidgetReferences->currentItem()
        );
        fillet->UseAllEdges.setValue(checked);
        fillet->recomputeFeature();

        if (checked) {
            ui->activeEdgeLabel->setText(tr("Radius for all edges"));
            setRadiusControlsEnabled(!isVariableRadius());
            if (gizmoContainer) {
                gizmoContainer->visible = false;
            }
        }
        else {
            ensureCurrentEdge();
        }
    }
}

void TaskFilletParameters::setButtons(const selectionModes mode)
{
    const bool selectingReferences = mode == refSel && !addingControlPoint;
    ui->buttonRefSel->setChecked(selectingReferences);
    ui->buttonRefSel->setText(selectingReferences ? stopSelectionLabel() : tr("+ Add geometry"));
}

void TaskFilletParameters::onRefDeleted()
{
    auto* fillet = getObject<PartDesign::Fillet>();
    std::set<std::string> selected;
    std::set<std::string> removed;
    for (auto* item : ui->treeWidgetReferences->selectedItems()) {
        selected.insert(item->text(0).toStdString());
        removed.insert(item->text(0).toStdString());
        for (int i = 0; i < item->childCount(); ++i) {
            removed.insert(item->child(i)->text(0).toStdString());
        }
    }
    if (removed.empty()) {
        return;
    }
    // A deleted face does not own boundaries shared with a retained face.
    // Explicitly selected edges, however, must be removed from every group.
    for (int row = 0; row < ui->treeWidgetReferences->topLevelItemCount(); ++row) {
        auto* group = ui->treeWidgetReferences->topLevelItem(row);
        if (selected.contains(group->text(0).toStdString())) {
            continue;
        }
        for (int i = 0; i < group->childCount(); ++i) {
            const auto child = group->child(i)->text(0).toStdString();
            if (!selected.contains(child)) {
                removed.erase(child);
            }
        }
    }
    // Removing part of a face selection makes the remaining boundaries explicit.
    // Otherwise the face would silently re-add the removed edge on recompute.
    std::vector<std::string> refs;
    for (const auto& ref : fillet->Base.getSubValues()) {
        if (removed.contains(ref)) {
            continue;
        }
        const auto matches
            = ui->treeWidgetReferences->findItems(QString::fromStdString(ref), Qt::MatchExactly);
        auto* group = matches.empty() ? nullptr : matches.front();
        bool partial = false;
        if (group) {
            for (int i = 0; i < group->childCount(); ++i) {
                partial = partial || removed.contains(group->child(i)->text(0).toStdString());
            }
        }
        if (partial) {
            for (int i = 0; i < group->childCount(); ++i) {
                const auto child = group->child(i)->text(0).toStdString();
                if (!removed.contains(child) && std::ranges::find(refs, child) == refs.end()) {
                    refs.push_back(child);
                }
            }
        }
        else if (std::ranges::find(refs, ref) == refs.end()) {
            refs.push_back(ref);
        }
    }
    setupTransaction();
    Gui::Selection().clearSelection();
    updateFeature(fillet, refs);
    const auto remaining = radiusEdges();
    for (const auto& ref : removed) {
        if (std::ranges::none_of(remaining, [&](const auto& edge) { return edge.first == ref; })) {
            fillet->VariableRadiusData.deleteValue(ref);
            fillet->RadiusLawModes.deleteValue(ref);
            fillet->clearRadiusControlPoints(ref);
            edgeRadii.erase(ref);
        }
    }
    refreshReferences();
    refreshEdgeTree();
    rebuildAllGizmos();
}

void TaskFilletParameters::onAddAllEdges()
{
    TaskDressUpParameters::addAllEdges(ui->listWidgetReferences);
    refreshReferences();
    updatePreview();
    rebuildAllGizmos();
}

void TaskFilletParameters::onStartRadiusChanged(double value)
{
    if (!isVariableRadius()) {
        onDefaultRadiusChanged(value);
        return;
    }

    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    editPoint(item->text().toStdString(), "start", 0, value, false);
}

void TaskFilletParameters::onEndRadiusChanged(double value)
{
    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }
    editPoint(item->text().toStdString(), "end", 1, value, false);
}

void TaskFilletParameters::onDefaultRadiusChanged(double value)
{
    defaultRadius = value;
    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        fillet->Radius.setValue(value);
        if (!isVariableRadius()) {
            QSignalBlocker blocker(ui->filletRadius);
            ui->filletRadius->setValue(value);
        }
        updatePreview();
    }
}


void TaskFilletParameters::onFilletTypeChanged(int index)
{
    const bool variable = isVariableRadius();
    if (variable && ui->checkBoxUseAllEdges->isChecked()) {
        ui->checkBoxUseAllEdges->setChecked(false);
    }

    allowFaces = true;
    if (selectionMode != none) {
        Gui::Selection().rmvSelectionGate();
        setSelectionGate();
    }

    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        fillet->RadiusMode.setValue(index);
        if (variable) {
            refreshReferences();
        }
        if (variable && currentEdgeShape()) {
            syncRadiusLaw(ui->listWidgetReferences->currentItem()->text().toStdString());
        }
        else {
            fillet->recomputeFeature();
            hideOnError();
        }
    }
    if (!variable) {
        setAddControlPointMode(false);
    }
    updateFilletTypeUi();
    ensureCurrentEdge();
    refreshEdgeTree();
    rebuildAllGizmos();
}

void TaskFilletParameters::onCurrentEdgeChanged(
    QListWidgetItem* current,
    [[maybe_unused]] QListWidgetItem* previous
)
{
    QSignalBlocker startBlocker(ui->filletRadius);
    QSignalBlocker endBlocker(ui->filletEndRadius);
    ui->filletRadius->unbind();
    ui->filletEndRadius->unbind();
    if (addingControlPoint) {
        setAddControlPointMode(false);
    }
    if (ui->checkBoxUseAllEdges->isChecked()) {
        ui->activeEdgeLabel->setText(tr("Radius for all edges"));
        setRadiusControlsEnabled(!isVariableRadius());
        if (gizmoContainer) {
            gizmoContainer->visible = false;
        }
        return;
    }

    if (!current) {
        ui->activeEdgeLabel->setText(
            isVariableRadius() ? tr("Select an edge to edit its radii")
                               : tr("Select an edge to edit its radius")
        );
        setRadiusControlsEnabled(false);
        clearGizmos();
        ui->controlPointTable->setRowCount(0);
        updateFilletTypeUi();
        return;
    }

    auto it = edgeRadii
                  .try_emplace(
                      current->text().toStdString(),
                      EdgeRadii {defaultRadius, defaultRadius, {}}
                  )
                  .first;
    const auto& radii = it->second;
    if (isVariableRadius() && !currentEdgeShape()) {
        ui->activeEdgeLabel->setText(tr("Select an edge in this face group"));
        setRadiusControlsEnabled(false);
        clearGizmos();
        ui->controlPointTable->setRowCount(0);
        updateFilletTypeUi();
        return;
    }
    ui->activeEdgeLabel->setText(
        isVariableRadius() ? tr("Radii for %1").arg(current->text())
                           : tr("Radius for %1").arg(current->text())
    );

    auto* fillet = getObject<PartDesign::Fillet>();
    if (isVariableRadius() && fillet) {
        const std::string edgeName = current->text().toStdString();
        ui->filletRadius->bind(fillet->ensureRadiusControlPointValue(
            edgeName,
            "start",
            PartDesign::Fillet::ControlPointComponent::Radius,
            radii.start
        ));
        ui->filletRadius->setAutoApply(true);
        ui->filletEndRadius->bind(fillet->ensureRadiusControlPointValue(
            edgeName,
            "end",
            PartDesign::Fillet::ControlPointComponent::Radius,
            radii.end
        ));
        ui->filletEndRadius->setAutoApply(true);
    }
    const double startRadius = isVariableRadius() || !fillet ? radii.start
                                                             : fillet->Radius.getValue();
    ui->filletRadius->setValue(startRadius);
    ui->filletEndRadius->setValue(radii.end);

    setRadiusControlsEnabled(true);
    refreshPointTable();
    updateFilletTypeUi();
    setGizmoPositions();
}


TaskFilletParameters::~TaskFilletParameters()
{
    clearGizmos();
    try {
        Gui::Selection().clearSelection();
        Gui::Selection().rmvSelectionGate();
    }
    catch (const Py::Exception&) {
        Base::PyException e;  // extract the Python error text
        e.reportException();
    }
}

void TaskFilletParameters::changeEvent(QEvent* e)
{
    TaskBox::changeEvent(e);
    if (e->type() == QEvent::LanguageChange) {
        ui->retranslateUi(proxy);
        setButtons(selectionMode);
        updateFilletTypeUi();
        ensureCurrentEdge();
        refreshEdgeTree();
    }
}

void TaskFilletParameters::apply()
{
    ui->defaultRadiusEditor->apply();

    // Alert user if he created an empty feature
    if (ui->listWidgetReferences->count() == 0) {
        std::string text = tr("Empty fillet created!").toStdString();
        Base::Console().warning("%s\n", text.c_str());
    }
}

void TaskFilletParameters::clearGizmos()
{
    radiusGizmo = nullptr;
    radiusGizmo2 = nullptr;
    startPointGizmo = nullptr;
    endPointGizmo = nullptr;
    if (gizmoContainer) {
        gizmoContainer->replaceGizmos({});
    }
    for (const auto& gizmo : controlPointGizmos) {
        delete gizmo.radiusEditor;
    }
    controlPointGizmos.clear();
}

void TaskFilletParameters::rebuildAllGizmos()
{
    clearGizmos();
    rebuildGizmos();
}

void TaskFilletParameters::refreshEdgeTree()
{
    auto* fillet = getObject<PartDesign::Fillet>();
    if (!fillet) {
        return;
    }
    const QString currentEdge = ui->listWidgetReferences->currentItem()
        ? ui->listWidgetReferences->currentItem()->text()
        : QString();
    QSignalBlocker blocker(ui->treeWidgetReferences);
    if (geometryTreeDirty) {
        geometryTreeDirty = false;
        ui->treeWidgetReferences->clear();
        for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
            auto* reference = ui->listWidgetReferences->item(row);
            auto* item = new QTreeWidgetItem(ui->treeWidgetReferences, {reference->text()});
            item->setSelected(reference->isSelected());
            if (reference->text() == currentEdge) {
                ui->treeWidgetReferences->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            }
        }
        // A face is a selection group, never a radius law. Its boundary rows share
        // the same edge state as individually selected edges.
        const auto baseShape = fillet->getBaseTopoShape(true);
        const auto refs = fillet->Base.getSubValues();
        const auto resolved = fillet->Base.getSubValues(true);
        const auto edges = radiusEdges();
        for (size_t i = 0; i < refs.size(); ++i) {
            const auto face = baseShape.getSubTopoShape(resolved[i].c_str(), true);
            if (face.isNull() || face.shapeType() != TopAbs_FACE) {
                continue;
            }
            const auto groups
                = ui->treeWidgetReferences->findItems(QString::fromStdString(refs[i]), Qt::MatchExactly);
            if (groups.empty()) {
                continue;
            }
            auto* group = groups.front();
            group->setText(1, QString());
            for (const auto& child : face.getSubTopoShapes(TopAbs_EDGE)) {
                for (const auto& [name, edge] : edges) {
                    if (!child.getShape().IsSame(edge.getShape())) {
                        continue;
                    }
                    const auto matches = ui->treeWidgetReferences->findItems(
                        QString::fromStdString(name),
                        Qt::MatchExactly | Qt::MatchRecursive
                    );
                    if (!matches.empty()) {
                        auto* item = matches.front();
                        if (item->parent()) {
                            group->addChild(item->clone());
                        }
                        else {
                            ui->treeWidgetReferences->takeTopLevelItem(
                                ui->treeWidgetReferences->indexOfTopLevelItem(item)
                            );
                            group->addChild(item);
                        }
                    }
                }
            }
            group->setExpanded(true);
        }
    }
    const auto quantity = [](double radius) {
        return QString::fromStdString(Base::Quantity(radius, Base::Unit::Length).getUserString());
    };
    // Radius edits retain item identity, selection, and collapsed face groups.
    for (QTreeWidgetItemIterator it(ui->treeWidgetReferences); *it; ++it) {
        auto* item = *it;
        const auto name = item->text(0).toStdString();
        if (item->childCount()) {
            continue;
        }
        const auto found = edgeRadii.find(name);
        if (found != edgeRadii.end()) {
            const auto& radii = found->second;
            double low = std::min(radii.start, radii.end);
            double high = std::max(radii.start, radii.end);
            for (const auto& point : radii.controlPoints) {
                low = std::min(low, point.radius);
                high = std::max(high, point.radius);
            }
            item->setText(
                1,
                quantity(low) + (low == high ? QString() : QStringLiteral(" – ") + quantity(high))
            );
        }
    }
}

void TaskFilletParameters::refreshControlPointValuesFromModel()
{
    // A synchronous edit also consumes the pending model-change refresh.
    controlPointRefreshQueued = false;
    auto* fillet = getObject<PartDesign::Fillet>();
    if (!fillet) {
        return;
    }

    defaultRadius = fillet->Radius.getValue();
    {
        QSignalBlocker blocker(ui->defaultRadiusEditor);
        ui->defaultRadiusEditor->setValue(defaultRadius);
    }

    bool structureChanged = false;
    for (auto& [edgeName, radii] : edgeRadii) {
        const auto law = fillet->getRadiusLaw(edgeName, edgeLength(edgeName));
        const auto ids = fillet->getRadiusControlPointIds(edgeName);
        if (law.size() < 2 || ids.size() + 2 != law.size()) {
            continue;
        }
        const bool edgeStructureChanged
            = !std::ranges::equal(radii.controlPoints, ids, {}, &ControlPoint::id);

        radii.start = law.front().radius;
        radii.end = law.back().radius;
        std::vector<ControlPoint> refreshedPoints;
        refreshedPoints.reserve(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            refreshedPoints.push_back({law[i + 1].position, law[i + 1].radius, ids[i]});
        }
        radii.controlPoints = std::move(refreshedPoints);
        structureChanged = structureChanged || edgeStructureChanged;
    }

    for (auto& gizmo : controlPointGizmos) {
        const auto edge = edgeRadii.find(gizmo.edgeName);
        if (edge == edgeRadii.end()) {
            continue;
        }
        const auto point
            = std::ranges::find(edge->second.controlPoints, gizmo.pointId, &ControlPoint::id);
        if (point == edge->second.controlPoints.end()) {
            continue;
        }
        QSignalBlocker radiusBlocker(gizmo.radiusEditor);
        gizmo.radiusEditor->setValue(point->radius);
    }

    auto* current = ui->listWidgetReferences->currentItem();
    if (current) {
        const auto edge = edgeRadii.find(current->text().toStdString());
        if (edge != edgeRadii.end()) {
            QSignalBlocker startBlocker(ui->filletRadius);
            QSignalBlocker endBlocker(ui->filletEndRadius);
            ui->filletRadius->setValue(
                isVariableRadius() ? edge->second.start : fillet->Radius.getValue()
            );
            ui->filletEndRadius->setValue(edge->second.end);
        }
    }
    refreshEdgeTree();
    refreshPointTable();
    if (structureChanged) {
        rebuildAllGizmos();
    }
    else {
        updateFilletTypeUi();
    }
}

void TaskFilletParameters::activateEdge(const std::string& edgeName)
{
    const auto items
        = ui->listWidgetReferences->findItems(QString::fromStdString(edgeName), Qt::MatchExactly);
    if (items.empty()) {
        return;
    }
    auto* previous = ui->listWidgetReferences->currentItem();
    {
        QSignalBlocker blocker(ui->listWidgetReferences);
        ui->listWidgetReferences->setCurrentItem(items.front());
        items.front()->setSelected(true);
    }
    if (items.front() != previous) {
        onCurrentEdgeChanged(items.front(), previous);
    }
    selectEdgeTreeItem(items.front()->text());
}

void TaskFilletParameters::selectEdgeTreeItem(const QString& edgeName)
{
    QSignalBlocker blocker(ui->treeWidgetReferences);
    ui->treeWidgetReferences->clearSelection();
    for (QTreeWidgetItemIterator it(ui->treeWidgetReferences); *it; ++it) {
        auto* treeItem = *it;
        const auto matches = ui->listWidgetReferences->findItems(treeItem->text(0), Qt::MatchExactly);
        if (!matches.empty() && matches.front()->isSelected()) {
            treeItem->setSelected(true);
        }
        if (treeItem->text(0) == edgeName) {
            ui->treeWidgetReferences->setCurrentItem(treeItem);
        }
    }
}

void TaskFilletParameters::syncEdgeTreeSelection()
{
    std::vector<QString> selectedEdges;
    for (auto* item : ui->treeWidgetReferences->selectedItems()) {
        selectedEdges.push_back(item->text(0));
    }
    auto* currentTreeItem = ui->treeWidgetReferences->currentItem();
    QListWidgetItem* currentListItem = nullptr;
    auto* previousListItem = ui->listWidgetReferences->currentItem();
    {
        QSignalBlocker blocker(ui->listWidgetReferences);
        ui->listWidgetReferences->clearSelection();
        for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
            auto* item = ui->listWidgetReferences->item(row);
            item->setSelected(std::ranges::find(selectedEdges, item->text()) != selectedEdges.end());
            if (currentTreeItem && item->text() == currentTreeItem->text(0)) {
                currentListItem = item;
            }
        }
        if (currentListItem && currentListItem != previousListItem) {
            ui->listWidgetReferences->setCurrentItem(currentListItem);
        }
    }
    if (currentListItem && currentListItem != previousListItem) {
        onCurrentEdgeChanged(currentListItem, previousListItem);
    }
    updateFilletTypeUi();
    refreshPointTable();
}

void TaskFilletParameters::rebuildGizmos()
{
    auto* viewProvider = getDressUpView();
    if (!GizmoContainer::isEnabled() || !viewProvider) {
        return;
    }

    std::vector<Gui::Gizmo*> gizmos;
    const auto gesture = [this](bool started) {
        if (started) {
            beginPointEdit();
        }
        else {
            finishPointEdit();
            updatePreview();
        }
    };
    radiusGizmo = isVariableRadius() ? new FilletRadiusGizmo(ui->filletRadius, gesture)
                                     : new Gui::LinearGizmo(ui->filletRadius);
    radiusGizmo2 = isVariableRadius() ? new FilletRadiusGizmo(ui->filletEndRadius, gesture)
                                      : new Gui::LinearGizmo(ui->filletEndRadius);
    radiusGizmo->setActivationCallback([this] {
        if (isVariableRadius()) {
            beginPointEdit();
            selectPoint("start");
        }
    });
    radiusGizmo2->setActivationCallback([this] {
        beginPointEdit();
        selectPoint("end");
    });
    gizmos.push_back(radiusGizmo);
    gizmos.push_back(radiusGizmo2);
    startPointGizmo = nullptr;
    endPointGizmo = nullptr;
    if (isVariableRadius()) {
        startPointGizmo = new EdgePositionGizmo(0, 1, [](double, bool) {},
                                               [this] { selectPoint("start"); }, true);
        endPointGizmo = new EdgePositionGizmo(1, 1, [](double, bool) {},
                                             [this] { selectPoint("end"); }, true);
        gizmos.push_back(startPointGizmo);
        gizmos.push_back(endPointGizmo);
    }

    auto* fillet = getObject<PartDesign::Fillet>();
    for (int edgeRow = 0; edgeRow < ui->listWidgetReferences->count(); ++edgeRow) {
        const std::string edgeName = ui->listWidgetReferences->item(edgeRow)->text().toStdString();
        auto found = edgeRadii.find(edgeName);
        if (found == edgeRadii.end()) {
            continue;
        }

        const double edgeLength = this->edgeLength(edgeName).value_or(1.0);

        auto& points = found->second.controlPoints;
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            Gui::QuantitySpinBox* radiusEditor = nullptr;
            if (fillet) {
                radiusEditor = new Gui::QuantitySpinBox(proxy);
                radiusEditor->setUnit(Base::Unit::Length);
                radiusEditor->setMinimum(0.0);
                radiusEditor->setValue(points[pointIndex].radius);
                radiusEditor->setVisible(false);
                connect(
                    radiusEditor,
                    qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                    this,
                    [this, edgeName, id = points[pointIndex].id](double value) {
                        editRadius(edgeName, id, value);
                    }
                );
                radiusEditor->bind(fillet->ensureRadiusControlPointValue(
                    edgeName,
                    points[pointIndex].id,
                    PartDesign::Fillet::ControlPointComponent::Radius,
                    points[pointIndex].radius
                ));
                radiusEditor->setAutoApply(true);
            }

            if (!radiusEditor) {
                continue;
            }

            auto* radius = new FilletRadiusGizmo(radiusEditor, gesture);
            radius->setActivationCallback([this, edgeName, id = points[pointIndex].id]() {
                beginPointEdit();
                activateEdge(edgeName);
                selectPoint(id);
            });
            auto* slider = new EdgePositionGizmo(
                points[pointIndex].position,
                edgeLength,
                [this, edgeName, id = points[pointIndex].id](double value, bool finished) {
                    auto edge = edgeRadii.find(edgeName);
                    if (edge == edgeRadii.end() || hasPositionExpression(edgeName, id)) {
                        return;
                    }
                    auto point = std::ranges::find(edge->second.controlPoints, id, &ControlPoint::id);
                    if (point == edge->second.controlPoints.end()) {
                        return;
                    }
                    const std::size_t index = static_cast<std::size_t>(
                        std::distance(edge->second.controlPoints.begin(), point)
                    );
                    const double lower = index == 0
                        ? controlPointTolerance
                        : edge->second.controlPoints[index - 1].position + controlPointTolerance;
                    const double upper = index + 1 == edge->second.controlPoints.size()
                        ? 1.0 - controlPointTolerance
                        : edge->second.controlPoints[index + 1].position - controlPointTolerance;
                    if (lower > upper) {
                        return;
                    }
                    editPoint(
                        edgeName, id, std::clamp(value, lower, upper), point->radius,
                        getObject<PartDesign::Fillet>()->isRadiusControlPointAbsolute(edgeName, id)
                    );
                    if (finished) {
                        finishPointEdit();
                    }
                },
                [this, edgeName, id = points[pointIndex].id]() {
                    beginPointEdit();
                    activateEdge(edgeName);
                    selectPoint(id);
                }
            );
            controlPointGizmos.push_back(
                {edgeName, points[pointIndex].id, radius, slider, radiusEditor}
            );
            gizmos.push_back(radius);
            gizmos.push_back(slider);
        }
    }

    if (gizmoContainer) {
        gizmoContainer->replaceGizmos(gizmos);
    }
    else {
        gizmoContainer = GizmoContainer::create(gizmos, viewProvider);
    }
    updateFilletTypeUi();
}

void TaskFilletParameters::setGizmoPositions()
{
    if (!gizmoContainer || !radiusGizmo || !radiusGizmo2) {
        return;
    }
    if (ui->checkBoxUseAllEdges->isChecked()) {
        gizmoContainer->visible = false;
        return;
    }

    auto fillet = getObject<PartDesign::Fillet>();
    if (!fillet) {
        gizmoContainer->visible = false;
        return;
    }
    auto* current = ui->listWidgetReferences->currentItem();
    if (!current) {
        gizmoContainer->visible = false;
        return;
    }

    Part::TopoShape baseShape = fillet->getBaseTopoShape(true);
    const auto selected = currentEdgeShape();
    if (!selected) {
        gizmoContainer->visible = false;
        return;
    }
    auto edge = *selected;
    gizmoContainer->visible = true;

    auto [props1, props2, correction] = radiusDraggerFrame(edge, baseShape);

    if (isVariableRadius()) {
        // Use the same spine parameterization for endpoint and control-point gizmos.
        const auto start = evaluateEdgePosition(TopoDS::Edge(edge.getShape()), 0.0);
        const auto end = evaluateEdgePosition(TopoDS::Edge(edge.getShape()), 1.0);
        if (!start || !end) {
            gizmoContainer->visible = false;
            return;
        }

        props1.position = start->position;
        props2.position = end->position;
        if (startPointGizmo && endPointGizmo) {
            startPointGizmo->Gizmo::setDraggerPlacement(start->position, start->tangent);
            endPointGizmo->Gizmo::setDraggerPlacement(end->position, end->tangent);
        }
    }

    radiusGizmo->Gizmo::setDraggerPlacement(props1.position, props1.dir);
    if (isVariableRadius()) {
        radiusGizmo2->Gizmo::setDraggerPlacement(props2.position, props1.dir);
    }

    radiusGizmo->setMultFactor(correction);
    radiusGizmo2->setMultFactor(correction);

    for (auto& gizmo : controlPointGizmos) {
        const auto selectedEdge = edgeShape(gizmo.edgeName);
        auto radii = edgeRadii.find(gizmo.edgeName);
        if (!selectedEdge || radii == edgeRadii.end()) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }
        auto controlEdge = *selectedEdge;
        const auto point
            = std::ranges::find(radii->second.controlPoints, gizmo.pointId, &ControlPoint::id);
        if (point == radii->second.controlPoints.end()) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }

        const auto frame = evaluateEdgePosition(TopoDS::Edge(controlEdge.getShape()), point->position);
        if (!frame) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }

        const auto radiusFrame = radiusDraggerFrame(controlEdge, baseShape);
        gizmo.radius->Gizmo::setDraggerPlacement(frame->position, radiusFrame.first.dir);
        gizmo.radius->setMultFactor(radiusFrame.correction);
        gizmo.radius->setVisibility(isVariableRadius());

        if (!gizmo.position->isDragging()) {
            gizmo.position->setPosition(point->position, frame->length);
            gizmo.position->Gizmo::setDraggerPlacement(frame->position, frame->tangent);
        }
        gizmo.position->setVisibility(
            isVariableRadius() && !hasPositionExpression(gizmo.edgeName, gizmo.pointId)
        );
    }
    updatePointHighlight();
}

void TaskFilletParameters::setRadiusControlsEnabled(bool enabled)
{
    ui->startRadiusLabel->setEnabled(enabled);
    ui->filletRadius->setEnabled(enabled);
    ui->filletEndRadius->setEnabled(enabled && isVariableRadius());
    const bool canEditPoints = enabled && isVariableRadius();
    ui->controlPointTable->setEnabled(canEditPoints);
    ui->addControlPointButton->setEnabled(canEditPoints);
}

void TaskFilletParameters::updateFilletTypeUi()
{
    const bool variable = isVariableRadius();
    const bool points = variable && currentLawIsVariable();
    const bool edge = bool(currentEdgeShape());
    if (gizmoContainer) {
        gizmoContainer->setOnTop(variable);
    }
    ui->treeWidgetReferences->setColumnHidden(1, !variable);
    ui->activeEdgeLabel->setVisible(variable);
    if (auto* current = ui->listWidgetReferences->currentItem()) {
        ui->activeEdgeLabel->setText(current->text());
    }
    ui->startRadiusLabel->setVisible(variable && !points);
    ui->startRadiusLabel->setText(tr("Radius"));
    ui->filletRadius->setVisible(variable && !points);
    ui->filletEndRadius->hide();
    ui->controlPointTable->setVisible(points);
    ui->addControlPointButton->setVisible(points);
    ui->removeControlPoint->setVisible(points);
    ui->removeControlPoint->setEnabled(points && activePoint != "start" && activePoint != "end");
    ui->radiusLaw->setVisible(variable);
    ui->radiusLaw->setEnabled(edge);
    ui->radiusLawLabel->setVisible(variable);
    {
        QSignalBlocker blocker(ui->radiusLaw);
        ui->radiusLaw->setCurrentIndex(points ? 1 : 0);
    }
    ui->defaultRadiusLabel->setText(variable ? tr("Default Radius") : tr("Radius"));
    ui->defaultRadiusLabel->setVisible(!variable || points);
    ui->defaultRadiusEditor->setVisible(!variable || points);
    advancedBox->setVisible(points);
    ui->checkBoxUseAllEdges->setEnabled(!variable);
    ui->checkBoxUseAllEdges->setVisible(!variable);
    ui->addControlPointButton->setEnabled(points && edge);

    if (radiusGizmo && radiusGizmo2) {
        radiusGizmo->setVisibility(variable || !ui->defaultRadiusEditor->hasExpression());
        radiusGizmo2->setVisibility(points);
        if (startPointGizmo && endPointGizmo) {
            startPointGizmo->setVisibility(points);
            endPointGizmo->setVisibility(points);
        }
        for (auto& gizmo : controlPointGizmos) {
            gizmo.radius->setVisibility(variable);
            gizmo.position->setVisibility(
                variable && !hasPositionExpression(gizmo.edgeName, gizmo.pointId)
            );
        }
    }
    setGizmoPositions();
}

bool TaskFilletParameters::isVariableRadius() const
{
    return ui->filletType->currentIndex() == 1;
}

void TaskFilletParameters::ensureCurrentEdge()
{
    auto* current = ui->listWidgetReferences->currentItem();
    if (!current && ui->listWidgetReferences->count() > 0) {
        QSignalBlocker blocker(ui->listWidgetReferences);
        ui->listWidgetReferences->setCurrentRow(0);
        current = ui->listWidgetReferences->currentItem();
    }
    onCurrentEdgeChanged(current, nullptr);
}

void TaskFilletParameters::setAddControlPointMode(bool enabled)
{
    enabled = enabled && isVariableRadius() && !ui->checkBoxUseAllEdges->isChecked()
        && ui->listWidgetReferences->currentItem();
    addingControlPoint = enabled;

    QSignalBlocker blocker(ui->addControlPointButton);
    ui->addControlPointButton->setChecked(enabled);
    if (enabled) {
        setSelectionMode(refSel);
        Gui::Selection().clearSelection();
        ui->activeEdgeLabel->setText(tr("Click a position on any selected edge"));
    }
    else {
        if (selectionMode == refSel) {
            setSelectionMode(none);
        }
        if (auto* current = ui->listWidgetReferences->currentItem()) {
            ui->activeEdgeLabel->setText(tr("Radii for %1").arg(current->text()));
        }
    }
}

bool TaskFilletParameters::addControlPointFromSelection(const Gui::SelectionChanges& msg)
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* base = getBase();
    if (!fillet || !base || !msg.hasPickedPoint
        || strcmp(msg.pDocName, fillet->getDocument()->getName()) != 0
        || strcmp(msg.pObjectName, base->getNameInDocument()) != 0) {
        return false;
    }

    const auto items
        = ui->listWidgetReferences->findItems(QString::fromUtf8(msg.pSubName), Qt::MatchExactly);
    if (items.empty()) {
        return false;
    }
    const int edgeRow = ui->listWidgetReferences->row(items.front());
    const auto selected = edgeShape(items.front()->text().toStdString());
    if (!selected) {
        return false;
    }
    const auto& edge = *selected;

    Base::Matrix4D transform;
    if (!base->getSubObject(msg.pSubName, nullptr, &transform, true, 0)) {
        return false;
    }
    transform.inverse();
    const Base::Vector3d localPoint = transform * Base::Vector3d(msg.x, msg.y, msg.z);
    const auto position = projectPointToEdge(
        TopoDS::Edge(edge.getShape()),
        gp_Pnt(localPoint.x, localPoint.y, localPoint.z)
    );
    if (!position || *position <= controlPointTolerance || *position >= 1.0 - controlPointTolerance) {
        return false;
    }

    const std::string edgeName = ui->listWidgetReferences->item(edgeRow)->text().toStdString();
    auto& radii
        = edgeRadii.try_emplace(edgeName, EdgeRadii {defaultRadius, defaultRadius, {}}).first->second;
    auto& points = radii.controlPoints;
    if (std::ranges::any_of(points, [position](const ControlPoint& point) {
            return std::abs(point.position - *position) <= controlPointTolerance;
        })) {
        return false;
    }

    activateEdge(edgeName);
    insertPoint(*position);
    setAddControlPointMode(true);
    return true;
}

void TaskFilletParameters::syncRadiusLaw(const std::string& edgeName)
{
    auto* fillet = getObject<PartDesign::Fillet>();
    const auto found = edgeRadii.find(edgeName);
    if (!fillet || found == edgeRadii.end()) {
        return;
    }

    if (!edgeShape(edgeName)) {
        return;
    }
    auto refs = fillet->Base.getSubValues();
    if (std::ranges::find(refs, edgeName) == refs.end()) {
        refs.push_back(edgeName);
        fillet->Base.setValue(fillet->Base.getValue(), refs);
    }

    Part::FilletRadiusLaw law {{0.0, found->second.start}};
    std::vector<std::string> ids;
    for (const auto& point : found->second.controlPoints) {
        law.push_back({point.position, point.radius});
        ids.push_back(point.id);
    }
    law.push_back({1.0, found->second.end});
    fillet->setRadiusControlPointIds(found->first, ids);
    fillet->setRadiusLaw(found->first, law);
    updatePreview();
}

bool TaskFilletParameters::hasPositionExpression(const std::string& edge, const std::string& id) const
{
    const auto* fillet = getObject<PartDesign::Fillet>();
    for (const auto* component : {"|position", "|length"}) {
        const auto path = fillet->VariableRadiusControlPointValues.getItemPath(edge + '|' + id + component);
        if (fillet->getExpression(path).expression) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, Part::TopoShape>> TaskFilletParameters::radiusEdges() const
try {
    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        // UseAllEdges deliberately ignores empty or stale Base subelement references.
        // Preserve those references on old documents; only resolve them for explicit selection.
        if (!fillet->UseAllEdges.getValue()) {
            return fillet->getRadiusEdges();
        }
    }
    return {};
}
catch (const Base::Exception&) {
    return {};  // Invalid references must not prevent opening the task panel to repair them.
}
catch (const Standard_Failure&) {
    return {};
}

std::optional<Part::TopoShape> TaskFilletParameters::edgeShape(const std::string& name) const
{
    for (const auto& [reference, edge] : radiusEdges()) {
        if (reference == name) {
            return edge;
        }
    }
    return std::nullopt;
}

std::optional<Part::TopoShape> TaskFilletParameters::currentEdgeShape() const
{
    const auto* current = ui->listWidgetReferences->currentItem();
    return current ? edgeShape(current->text().toStdString()) : std::nullopt;
}

std::optional<double> TaskFilletParameters::currentEdgeLength() const
{
    const auto* current = ui->listWidgetReferences->currentItem();
    return current ? edgeLength(current->text().toStdString()) : std::nullopt;
}

std::optional<double> TaskFilletParameters::edgeLength(const std::string& name) const
{
    const auto edge = edgeShape(name);
    if (!edge) {
        return std::nullopt;
    }
    const auto frame = evaluateEdgePosition(TopoDS::Edge(edge->getShape()), 0);
    return frame ? std::optional<double>(frame->length) : std::nullopt;
}

//**************************************************************************
//**************************************************************************
// TaskDialog
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void TaskFilletParameters::refreshReferences()
{
    geometryTreeDirty = true;
    auto* fillet = getObject<PartDesign::Fillet>();
    const QString current = ui->listWidgetReferences->currentItem()
        ? ui->listWidgetReferences->currentItem()->text()
        : QString();
    auto refs = fillet->Base.getSubValues();
    for (const auto& [name, edge] : radiusEdges()) {
        if (std::ranges::find(refs, name) == refs.end()) {
            refs.push_back(name);
        }
    }
    {
        QSignalBlocker blocker(ui->listWidgetReferences);
        ui->listWidgetReferences->clear();
        for (const auto& name : refs) {
            ui->listWidgetReferences->addItem(QString::fromStdString(name));
            edgeRadii.try_emplace(name, EdgeRadii {defaultRadius, defaultRadius, {}});
            if (isVariableRadius() && edgeShape(name)
                && !fillet->VariableRadiusData.getValues().contains(name)) {
                fillet->setRadiusLaw(name, {{0, defaultRadius}, {1, defaultRadius}});
            }
            if (QString::fromStdString(name) == current) {
                ui->listWidgetReferences->setCurrentRow(ui->listWidgetReferences->count() - 1);
            }
        }
    }
    ensureCurrentEdge();
}

void TaskFilletParameters::setupTaskPanel()
{
    auto* vp = getDressUpView();
    auto* view = vp ? dynamic_cast<Gui::View3DInventor*>(vp->getDocument()->getActiveView()) : nullptr;
    if (view) {
        view->getViewer()->getGLWidget()->installEventFilter(this);
    }
    // Share the dress-up selection action and its transaction/preview handling.
    const auto trashIcon = Gui::BitmapFactory().iconFromTheme("edit-delete");
    ui->removeGeometry->setIcon(trashIcon);
    connect(ui->removeGeometry, &QToolButton::clicked, this, &TaskFilletParameters::onRefDeleted);
    const int labelWidth = std::max(
        {ui->filletTypeLabel->sizeHint().width(),
         ui->defaultRadiusLabel->sizeHint().width(),
         ui->radiusLawLabel->sizeHint().width()}
    );
    for (auto* label : {ui->filletTypeLabel, ui->defaultRadiusLabel, ui->startRadiusLabel, ui->radiusLawLabel}) {
        label->setMinimumWidth(labelWidth);
    }
    ui->activeEdgeLabel->setContentsMargins(0, 8, 0, 0);
    connect(
        ui->radiusLaw,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        &TaskFilletParameters::changeRadiusLaw
    );

    ui->removeControlPoint->setIcon(trashIcon);
    connect(ui->removeControlPoint, &QToolButton::clicked, this, [this] { pointAction("remove"); });
    connect(ui->controlPointTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
        if (auto* item = ui->controlPointTable->item(row, 0)) {
            selectPoint(item->data(Qt::UserRole + 1).toString().toStdString());
        }
    });

    advancedBox = new Gui::TaskView::TaskBox(tr("Advanced Properties"), true, this);
    advancedBox->setObjectName(QStringLiteral("filletAdvanced"));
    auto* advanced = new QWidget(advancedBox);
    auto* advancedLayout = new QFormLayout(advanced);
    positionUnits = new QComboBox(advanced);
    positionUnits->setObjectName(QStringLiteral("controlPointPositionUnits"));
    positionUnits->addItems({tr("Model units"), tr("Percentage")});
    advancedLayout->addRow(tr("Control point position"), positionUnits);
    advancedBox->groupLayout()->addWidget(advanced);
    advancedBox->hideGroupBox();
    connect(positionUnits, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        refreshPointTable();
    });
}

bool TaskFilletParameters::currentLawIsVariable() const
{
    const auto* item = ui->listWidgetReferences->currentItem();
    return item && getObject<PartDesign::Fillet>()->isVariableRadiusLaw(item->text().toStdString());
}

void TaskFilletParameters::changeRadiusLaw(int index)
{
    auto* item = ui->listWidgetReferences->currentItem();
    if (!item || !currentEdgeShape()) {
        return;
    }
    const auto name = item->text().toStdString();
    if (index == 0) {
        pointAction("constant");
        const auto& radii = edgeRadii.at(name);
        if (!radii.controlPoints.empty() || radii.start != radii.end) {
            updateFilletTypeUi();  // Expressions can prevent replacement.
            return;
        }
        getObject<PartDesign::Fillet>()->RadiusLawModes.setValue(name, "Constant");
    }
    else {
        auto* fillet = getObject<PartDesign::Fillet>();
        if (!currentLawIsVariable()) {
            fillet->setRadiusControlPointValue(
                name,
                "end",
                PartDesign::Fillet::ControlPointComponent::Radius,
                edgeRadii.at(name).end
            );
        }
        getObject<PartDesign::Fillet>()->RadiusLawModes.setValue(name, "Variable");
        syncRadiusLaw(name);
    }
    refreshPointTable();
    updateFilletTypeUi();
}

void TaskFilletParameters::refreshPointTable()
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* current = ui->listWidgetReferences->currentItem();
    if (!fillet || !current || !isVariableRadius() || !currentLawIsVariable()) {
        return;
    }
    const auto name = current->text().toStdString();
    const auto& radii = edgeRadii.at(name);
    std::vector<ControlPoint> points {{0, radii.start, "start"}};
    points.insert(points.end(), radii.controlPoints.begin(), radii.controlPoints.end());
    points.push_back({1, radii.end, "end"});
    const bool absolute = positionUnits->currentIndex() == 0;
    const double length = currentEdgeLength().value_or(0);
    QSignalBlocker tableBlocker(ui->controlPointTable);
    bool rebuild = ui->controlPointTable->rowCount() != int(points.size());
    for (int row = 0; !rebuild && row < int(points.size()); ++row) {
        rebuild = ui->controlPointTable->item(row, 0)->data(Qt::UserRole).toString()
                != QString::fromStdString(name + "|" + points[row].id)
            || bool(qobject_cast<Gui::QuantitySpinBox*>(ui->controlPointTable->cellWidget(row, 1)))
                != absolute;
    }
    if (rebuild && qobject_cast<QAbstractSpinBox*>(sender())) {
        QMetaObject::invokeMethod(this, [this] { refreshPointTable(); }, Qt::QueuedConnection);
        return;
    }
    for (int row = ui->controlPointTable->rowCount() - 1; row >= 0; --row) {
        const auto key = ui->controlPointTable->item(row, 0)->data(Qt::UserRole).toString();
        if (std::ranges::none_of(points, [&](const auto& point) {
                return key == QString::fromStdString(name + "|" + point.id);
            })) {
            ui->controlPointTable->removeRow(row);
        }
    }
    using Component = PartDesign::Fillet::ControlPointComponent;
    for (int row = 0; row < int(points.size()); ++row) {
        const auto& point = points[row];
        const auto id = point.id;
        const bool endpoint = id == "start" || id == "end";
        const auto key = QString::fromStdString(name + "|" + id);
        auto* existing = ui->controlPointTable->item(row, 0);
        // Insert only the changed row. Keeping existing widgets alive also preserves
        // focus and native accessibility objects during viewport point insertion.
        if (!existing || existing->data(Qt::UserRole).toString() != key) {
            ui->controlPointTable->insertRow(row);
        }
        else if (bool(qobject_cast<Gui::QuantitySpinBox*>(ui->controlPointTable->cellWidget(row, 1)))
                 != absolute) {
            ui->controlPointTable->removeRow(row);
            ui->controlPointTable->insertRow(row);
        }
        if (!ui->controlPointTable->item(row, 0)) {
            auto* item = new QTableWidgetItem(endpoint ? QStringLiteral("●") : QStringLiteral("○"));
            item->setTextAlignment(Qt::AlignCenter);
            auto markerFont = ui->controlPointTable->font();
            if (markerFont.pointSizeF() > 0) {
                markerFont.setPointSizeF(markerFont.pointSizeF() * .5);
            }
            else {
                markerFont.setPixelSize(std::max(1, markerFont.pixelSize() / 2));
            }
            item->setFont(markerFont);
            item->setData(Qt::UserRole, QString::fromStdString(name + "|" + id));
            item->setData(Qt::UserRole + 1, QString::fromStdString(id));
            item->setToolTip(
                id == "start"     ? tr("Start")
                    : id == "end" ? tr("End")
                                  : tr("Control point")
            );
            ui->controlPointTable->setItem(row, 0, item);
            const auto connectFormula = [this](auto* editor) {
                if constexpr (
                    std::is_base_of_v<Gui::QuantitySpinBox, std::remove_pointer_t<decltype(editor)>>
                ) {
                    connect(
                        editor,
                        &Gui::QuantitySpinBox::showFormulaDialog,
                        this,
                        [this, editor](bool shown) {
                            if (!shown) {
                                editor->apply();
                                updatePreview();
                            }
                        }
                    );
                }
                else {
                    editor->setAutoApply(true);
                }
            };
            auto* radius = new Gui::QuantitySpinBox;
            radius->setObjectName(QStringLiteral("pointRadius_%1").arg(QString::fromStdString(id)));
            radius->setUnit(Base::Unit::Length);
            radius->setSingleStep(.1);
            radius->setMinimum(Precision::Confusion());
            radius->setKeyboardTracking(false);
            radius->bind(
                fillet->ensureRadiusControlPointValue(name, id, Component::Radius, point.radius)
            );
            radius->setAutoApply(false);
            radius->setValue(point.radius);
            connectFormula(radius);
            ui->controlPointTable->setCellWidget(row, 2, radius);
            connect(
                radius,
                qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                this,
                [this, name, id](double value) {
                    editRadius(name, id, value);
                }
            );
            const auto editPosition = [this, id, name, absolute](double value) {
                const auto& points = edgeRadii.at(name).controlPoints;
                const auto point = std::ranges::find(points, id, &ControlPoint::id);
                const double length = edgeLength(name).value_or(0);
                if (point != points.end() && (!absolute || length > Precision::Confusion())) {
                    editPoint(name, id, absolute ? value / length : value, point->radius, absolute);
                }
            };
            const auto configure = [&](auto* editor, Component component, double value) {
                editor->setKeyboardTracking(false);
                editor->setReadOnly(endpoint);
                editor->setObjectName(
                    QStringLiteral("pointPosition_%1").arg(QString::fromStdString(id))
                );
                if (!endpoint) {
                    editor->bind(fillet->ensureRadiusControlPointValue(name, id, component, value));
                    editor->setAutoApply(false);
                    connectFormula(editor);
                }
                editor->setValue(value);
                ui->controlPointTable->setCellWidget(row, 1, editor);
            };
            if (absolute) {
                auto* position = new Gui::QuantitySpinBox;
                position->setUnit(Base::Unit::Length);
                position->setSingleStep(.1);
                configure(position, Component::Length, point.position * length);
                connect(
                    position,
                    qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
                    this,
                    editPosition
                );
            }
            else {
                auto* position = new FilletPercentSpinBox;
                position->setDecimals(6);
                position->setRange(0, 1);
                position->setSingleStep(.01);
                position->setSuffix(QStringLiteral(" %"));
                configure(position, Component::Position, point.position);
                connect(position, qOverload<double>(&Gui::DoubleSpinBox::valueChanged), this, editPosition);
            }
        }
        auto* radius = static_cast<Gui::QuantitySpinBox*>(ui->controlPointTable->cellWidget(row, 2));
        QSignalBlocker radiusBlocker(radius);
        radius->setValue(point.radius);
        auto* position = ui->controlPointTable->cellWidget(row, 1);
        QSignalBlocker positionBlocker(position);
        if (auto* quantity = qobject_cast<Gui::QuantitySpinBox*>(position)) {
            quantity->setRange(0, length);
            quantity->setValue(point.position * length);
        }
        else {
            static_cast<FilletPercentSpinBox*>(position)->setValue(point.position);
        }
        if (endpoint) {
            static_cast<QAbstractSpinBox*>(position)->setReadOnly(true);
            position->setEnabled(false);
        }
        else {
            const auto otherPath = fillet->VariableRadiusControlPointValues.getItemPath(
                name + "|" + id + (absolute ? "|position" : "|length")
            );
            const bool otherExpression = bool(fillet->getExpression(otherPath).expression);
            position->setEnabled(!otherExpression);
            position->setToolTip(
                otherExpression ? tr("Position has an expression in the other unit mode. Switch "
                                     "the Advanced Properties setting to edit it.")
                                : QString()
            );
        }
        ui->controlPointTable->setRowHeight(row, radius->sizeHint().height() + 6);
        if (id == activePoint) {
            ui->controlPointTable->setCurrentCell(row, 0, QItemSelectionModel::ClearAndSelect);
        }
    }
    ui->controlPointTable->setRowCount(int(points.size()));
    // Rebuild the native focus chain in visual order after inserting or reordering points.
    QWidget* previous = ui->controlPointTable;
    for (int row = 0; row < ui->controlPointTable->rowCount(); ++row) {
        for (int column : {1, 2}) {
            auto* editor = ui->controlPointTable->cellWidget(row, column);
            editor->setProperty("filletPointId", QString::fromStdString(points[row].id));
            editor->installEventFilter(this);
            QWidget::setTabOrder(previous, editor);
            previous = editor;
        }
    }
    QWidget::setTabOrder(previous, ui->addControlPointButton);
    QWidget::setTabOrder(ui->addControlPointButton, ui->removeControlPoint);
}

void TaskFilletParameters::selectPoint(const std::string& id)
{
    activePoint = id;
    QSignalBlocker blocker(ui->controlPointTable);
    for (int row = 0; row < ui->controlPointTable->rowCount(); ++row) {
        if (ui->controlPointTable->item(row, 0)->data(Qt::UserRole + 1).toString().toStdString()
            == id) {
            ui->controlPointTable->setCurrentCell(row, 0, QItemSelectionModel::ClearAndSelect);
        }
    }
    ui->removeControlPoint->setEnabled(id != "start" && id != "end");
    updatePointHighlight();
}

void TaskFilletParameters::updatePointHighlight()
{
    if (startPointGizmo) {
        startPointGizmo->setSelected(activePoint == "start");
    }
    if (endPointGizmo) {
        endPointGizmo->setSelected(activePoint == "end");
    }
    const auto* current = ui->listWidgetReferences->currentItem();
    for (const auto& gizmo : controlPointGizmos) {
        gizmo.position->setSelected(current && current->text().toStdString() == gizmo.edgeName
                                   && activePoint == gizmo.pointId);
    }
}

void TaskFilletParameters::beginPointEdit()
{
    if (!pointEditActive) {
        setupTransaction();
        pointEditActive = true;
    }
}
void TaskFilletParameters::finishPointEdit()
{
    pointEditActive = false;
}
void TaskFilletParameters::updatePreview()
{
    auto* fillet = getObject<PartDesign::Fillet>();
    fillet->recomputeFeature();
    hideOnError();
    const QString message = fillet->isError()
        ? tr("Previous valid preview. This edit could not be built:\n%1")
              .arg(QString::fromUtf8(fillet->getStatusString()))
        : QString();
    ui->filletError->setText(message);
    ui->filletError->setVisible(!message.isEmpty());
    if (fillet->isError() && !fillet->Shape.getValue().IsNull()) {
        getDressUpView()->showPreviousFeature(false);
        getDressUpView()->show();
    }
    refreshControlPointValuesFromModel();
}
void TaskFilletParameters::editRadius(const std::string& name, const std::string& id, double radius)
{
    const auto edge = edgeRadii.find(name);
    if (edge == edgeRadii.end()) {
        return;
    }
    double position = id == "end" ? 1 : 0;
    if (id != "start" && id != "end") {
        const auto& points = edge->second.controlPoints;
        const auto point = std::ranges::find(points, id, &ControlPoint::id);
        if (point == points.end()) {
            return;
        }
        position = point->position;
    }
    editPoint(name, id, position, radius,
              getObject<PartDesign::Fillet>()->isRadiusControlPointAbsolute(name, id));
}

void TaskFilletParameters::editPoint(
    const std::string& name, const std::string& id, double position, double radius, bool absolute
)
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* item = ui->listWidgetReferences->currentItem();
    if (!fillet || !edgeRadii.contains(name)) {
        return;
    }
    auto& radii = edgeRadii[name];
    if (!(radius > 0) || !std::isfinite(radius)) {
        ui->filletError->setText(tr("Radius must be a positive finite length."));
        ui->filletError->show();
        return;
    }
    if (id != "start" && id != "end") {
        if (position <= 0 || position >= 1 || !std::isfinite(position)
            || std::ranges::any_of(radii.controlPoints, [&](const auto& p) {
                   return p.id != id && std::abs(p.position - position) < controlPointTolerance;
               })) {
            ui->filletError->setText(tr("Points must have distinct positions inside the edge."));
            ui->filletError->show();
            return;
        }
    }
    const bool ownsEdit = !pointEditActive;
    beginPointEdit();
    if (item && item->text().toStdString() == name) {
        activePoint = id;
    }
    if (id == "start") {
        radii.start = radius;
        if (!fillet->isVariableRadiusLaw(name)) {
            radii.end = radius;
            fillet->setRadiusControlPointValue(
                name, "end", PartDesign::Fillet::ControlPointComponent::Radius, radius
            );
        }
    }
    else if (id == "end") {
        radii.end = radius;
    }
    else {
        auto point = std::ranges::find(radii.controlPoints, id, &ControlPoint::id);
        if (point == radii.controlPoints.end()) {
            pointEditActive = false;
            return;
        }
        point->position = position;
        point->radius = radius;
        fillet->setRadiusControlPointPosition(name, id, position, edgeLength(name).value_or(0), absolute);
        std::ranges::sort(radii.controlPoints, {}, &ControlPoint::position);
    }
    fillet->setRadiusControlPointValue(name, id, PartDesign::Fillet::ControlPointComponent::Radius, radius);
    syncRadiusLaw(name);
    if (ownsEdit) {
        finishPointEdit();
    }
}
void TaskFilletParameters::insertPoint(double position)
{
    auto* item = ui->listWidgetReferences->currentItem();
    auto* fillet = getObject<PartDesign::Fillet>();
    if (!item || !fillet || position <= 0 || position >= 1) {
        return;
    }
    const auto name = item->text().toStdString();
    auto& radii = edgeRadii[name];
    if (std::ranges::any_of(radii.controlPoints, [&](const auto& p) {
            return std::abs(p.position - position) < controlPointTolerance;
        })) {
        return;
    }
    beginPointEdit();
    activePoint = fillet->newRadiusControlPointId(name);
    radii.controlPoints.push_back({position, defaultRadius, activePoint});
    std::ranges::sort(radii.controlPoints, {}, &ControlPoint::position);
    syncRadiusLaw(name);
    rebuildAllGizmos();
    selectPoint(activePoint);
    finishPointEdit();
}
void TaskFilletParameters::pointAction(const std::string& action)
{
    auto* item = ui->listWidgetReferences->currentItem();
    auto* fillet = getObject<PartDesign::Fillet>();
    if (!item || !fillet) {
        return;
    }
    const auto name = item->text().toStdString();
    auto& radii = edgeRadii[name];
    // Operations that discard or remap a law must not silently destroy expressions.
    if (action == "constant") {
        for (const auto& [key, value] : fillet->VariableRadiusControlPointValues.getValues()) {
            if (
                key.starts_with(name + "|")
                && fillet->getExpression(fillet->VariableRadiusControlPointValues.getItemPath(key)).expression
            ) {
                ui->filletError->show();
                ui->filletError->setText(
                    tr("Unlink this edge's expressions before replacing its radius distribution.")
                );
                return;
            }
        }
    }
    beginPointEdit();
    if (action == "remove") {
        std::erase_if(radii.controlPoints, [&](const auto& p) {
            return p.id == activePoint;
        });
        activePoint = "start";
    }
    else if (action == "constant") {
        double radius = radii.start;
        if (activePoint == "end") {
            radius = radii.end;
        }
        auto p = std::ranges::find(radii.controlPoints, activePoint, &ControlPoint::id);
        if (p != radii.controlPoints.end()) {
            radius = p->radius;
        }
        radii = {radius, radius, {}};
        activePoint = "start";
        fillet->clearRadiusControlPoints(name);
    }
    syncRadiusLaw(name);
    rebuildAllGizmos();
    finishPointEdit();
}
bool TaskFilletParameters::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn) {
        const auto id = watched->property("filletPointId").toString();
        if (!id.isEmpty()) {
            selectPoint(id.toStdString());
        }
    }
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape
        && addingControlPoint) {
        setAddControlPointMode(false);
        return true;
    }
    return TaskDressUpParameters::eventFilter(watched, event);
}

TaskDlgFilletParameters::TaskDlgFilletParameters(ViewProviderFillet* DressUpView)
    : TaskDlgDressUpParameters(DressUpView)
{
    parameter = new TaskFilletParameters(DressUpView);

    Content.push_back(parameter);
    Content.push_back(static_cast<TaskFilletParameters*>(parameter)->advancedBox);
    Content.push_back(preview);
}

TaskDlgFilletParameters::~TaskDlgFilletParameters() = default;

//==== calls from the TaskView ===============================================================

bool TaskDlgFilletParameters::accept()
{
    auto obj = getObject();
    if (!obj->isError()) {
        getViewObject()->showPreviousFeature(false);
    }

    parameter->apply();

    return TaskDlgDressUpParameters::accept();
}

#include "moc_TaskFilletParameters.cpp"
