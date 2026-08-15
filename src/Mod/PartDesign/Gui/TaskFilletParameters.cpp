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
#include <QComboBox>
#include <QHeaderView>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTableWidget>

#include <algorithm>
#include <cmath>
#include <functional>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>

#include <Base/Interpreter.h>
#include <Base/Converter.h>
#include <Base/Quantity.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Inventor/Draggers/SoLinearDragger.h>
#include <Gui/Inventor/Draggers/SoLinearDraggerGeometry.h>
#include <Gui/Selection/Selection.h>
#include <Gui/BitmapFactory.h>
#include <Gui/ViewProvider.h>
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

struct EdgePointFrame
{
    Base::Vector3d position;
    Base::Vector3d tangent;
    double length;
};

std::optional<EdgePointFrame> evaluateEdgePosition(const TopoDS_Edge& edge, double position)
{
    BRepAdaptor_Curve curve(edge);
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();
    const double length
        = GCPnts_AbscissaPoint::Length(curve, first, last, Precision::Confusion());
    if (length <= Precision::Confusion()) {
        return std::nullopt;
    }

    // OCCT parameterizes a variable-radius law along the fillet spine's
    // underlying curve direction, independently of the selected edge's orientation.
    double parameter = first;
    if (position > controlPointTolerance && position < 1.0 - controlPointTolerance) {
        GCPnts_AbscissaPoint solver(
            curve,
            position * length,
            parameter,
            Precision::Confusion()
        );
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
    const double total
        = GCPnts_AbscissaPoint::Length(adaptor, first, last, Precision::Confusion());
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

    EdgePositionGizmo(double position, double edgeLength, Callback callback)
        : position(position)
        , edgeLength(edgeLength)
        , callback(std::move(callback))
    {}

    SoInteractionKit* initDragger() override
    {
        container = new Gui::SoLinearDraggerContainer;
        container->color.setValue(0.95F, 0.75F, 0.15F);
        dragger = container->getDragger();
        dragger->labelVisible = false;
        dragger->instantiateBaseGeometry();
        auto arrow = SO_GET_PART(dragger, "arrow", Gui::SoArrowGeometry);
        arrow->cylinderHeight = 1.8F;
        arrow->cylinderRadius = 0.12F;
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

    void setPosition(double value, double length)
    {
        position = value;
        edgeLength = length;
        if (dragger) {
            dragger->translation = SbVec3f(0, 0, 0);
            dragger->translationIncrementCount = 0;
        }
    }

private:
    static void startCallback(void* data, SoDragger*)
    {
        auto* self = static_cast<EdgePositionGizmo*>(data);
        self->dragStartPosition = self->position;
        self->dragger->translationIncrementCount = 0;
    }

    static void motionCallback(void* data, SoDragger*)
    {
        static_cast<EdgePositionGizmo*>(data)->update(false);
    }

    static void finishCallback(void* data, SoDragger*)
    {
        static_cast<EdgePositionGizmo*>(data)->update(true);
    }

    void update(bool finished)
    {
        if (edgeLength <= Precision::Confusion()) {
            return;
        }
        const double distance = dragger->translationIncrementCount.getValue()
            * dragger->translationIncrement.getValue();
        callback(
                    std::clamp(dragStartPosition + (distance / edgeLength), 0.0, 1.0),
                    finished
                );
    }

    Gui::SoLinearDraggerContainer* container = nullptr;
    Gui::SoLinearDragger* dragger = nullptr;
    double position;
    double dragStartPosition = 0.0;
    double edgeLength;
    Callback callback;
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

    PartDesign::Fillet* pcFillet = DressUpView->getObject<PartDesign::Fillet>();
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
    double r = pcFillet->Radius.getValue();
    defaultRadius = r;
    ui->filletType->setCurrentIndex(pcFillet->RadiusMode.getValue());
    allowFaces = !variableRadius;

    ui->filletRadius->setUnit(Base::Unit::Length);
    ui->filletRadius->setValue(r);
    ui->filletRadius->setMinimum(0);
    ui->filletRadius->selectNumber();

    ui->filletEndRadius->setUnit(Base::Unit::Length);
    ui->filletEndRadius->setValue(r);
    ui->filletEndRadius->setMinimum(0);

    ui->controlPointTable->verticalHeader()->hide();
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ui->controlPointTable->setSelectionMode(QAbstractItemView::NoSelection);
    ui->addControlPointButton->setIcon(Gui::BitmapFactory().iconFromTheme("list-add"));
    ui->addControlPointButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QMetaObject::invokeMethod(ui->filletRadius, "setFocus", Qt::QueuedConnection);
    std::vector<std::string> strings = pcFillet->Base.getSubValues();
    for (const auto& string : strings) {
        ui->listWidgetReferences->addItem(QString::fromStdString(string));
        auto law = pcFillet->getRadiusLaw(string);
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
    connect(ui->filletType, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &TaskFilletParameters::onFilletTypeChanged);
    connect(ui->addControlPointButton, &QToolButton::toggled,
        this, &TaskFilletParameters::onAddControlPointToggled);
    connect(ui->buttonRefSel, &QToolButton::toggled,
        this, &TaskFilletParameters::onButtonRefSel);
    connect(ui->checkBoxUseAllEdges, &QToolButton::toggled,
        this, &TaskFilletParameters::onCheckBoxUseAllEdgesToggled);

    // Create context menu
    createDeleteAction(ui->listWidgetReferences);
    connect(deleteAction, &QAction::triggered, this, &TaskFilletParameters::onRefDeleted);

    createAddAllEdgesAction(ui->listWidgetReferences);
    connect(addAllEdgesAction, &QAction::triggered, this, &TaskFilletParameters::onAddAllEdges);

    connect(ui->listWidgetReferences, &QListWidget::currentItemChanged,
        this, &TaskFilletParameters::setSelection);
    connect(ui->listWidgetReferences, &QListWidget::currentItemChanged,
        this, &TaskFilletParameters::onCurrentEdgeChanged);
    connect(ui->listWidgetReferences, &QListWidget::itemClicked,
        this, &TaskFilletParameters::setSelection);
    connect(ui->listWidgetReferences, &QListWidget::itemDoubleClicked,
        this, &TaskFilletParameters::doubleClicked);
    // clang-format on

    if (strings.empty()) {
        setSelectionMode(refSel);
    }
    else {
        hideOnError();
    }

    setupGizmos(DressUpView);
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
            if (!fillet || !base
                || strcmp(msg.pDocName, fillet->getDocument()->getName()) != 0
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
                edgeRadii.try_emplace(subName, EdgeRadii {defaultRadius, defaultRadius, {}});
                ui->listWidgetReferences->setCurrentItem(matches.front());
            }
            else {
                edgeRadii.erase(subName);
                ensureCurrentEdge();
            }
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
    ui->buttonRefSel->setText(
        selectingReferences ? stopSelectionLabel() : startSelectionLabel()
    );
}

void TaskFilletParameters::onRefDeleted()
{
    std::vector<std::string> deletedRefs;
    for (const auto* item : ui->listWidgetReferences->selectedItems()) {
        deletedRefs.push_back(item->text().toStdString());
    }

    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        for (const auto& ref : deletedRefs) {
            fillet->VariableRadiusData.deleteValue(ref);
            fillet->clearRadiusControlPoints(ref);
        }
    }
    TaskDressUpParameters::deleteRef(ui->listWidgetReferences);
    for (const auto& ref : deletedRefs) {
        edgeRadii.erase(ref);
    }
    ensureCurrentEdge();
}

void TaskFilletParameters::onAddAllEdges()
{
    TaskDressUpParameters::addAllEdges(ui->listWidgetReferences);

    auto fillet = getObject<PartDesign::Fillet>();
    if (!fillet) {
        return;
    }

    for (const auto& ref : fillet->Base.getSubValues()) {
        const QString text = QString::fromStdString(ref);
        if (ui->listWidgetReferences->findItems(text, Qt::MatchExactly).empty()) {
            ui->listWidgetReferences->addItem(text);
        }
        edgeRadii.try_emplace(ref, EdgeRadii {defaultRadius, defaultRadius, {}});
    }
    ensureCurrentEdge();
}

void TaskFilletParameters::onStartRadiusChanged(double value)
{
    if (!isVariableRadius()) {
        if (auto* fillet = getObject<PartDesign::Fillet>()) {
            fillet->Radius.setValue(value);
            fillet->recomputeFeature();
            hideOnError();
            for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
                auto* item = ui->listWidgetReferences->item(row);
                const auto found = edgeRadii.find(item->text().toStdString());
                if (found != edgeRadii.end()) {
                    updateRadiusTooltip(item, found->second);
                }
            }
        }
        return;
    }

    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    auto& radii = edgeRadii
                      .try_emplace(
                          item->text().toStdString(),
                          EdgeRadii {defaultRadius, defaultRadius, {}}
                      )
                      .first->second;
    radii.start = value;
    updateRadiusTooltip(item, radii);
    syncCurrentRadiusLaw();
}

void TaskFilletParameters::onEndRadiusChanged(double value)
{
    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    auto& radii = edgeRadii
                      .try_emplace(
                          item->text().toStdString(),
                          EdgeRadii {defaultRadius, defaultRadius, {}}
                      )
                      .first->second;
    radii.end = value;
    updateRadiusTooltip(item, radii);
    syncCurrentRadiusLaw();
}

void TaskFilletParameters::onFilletTypeChanged(int index)
{
    const bool variable = isVariableRadius();
    if (variable && ui->checkBoxUseAllEdges->isChecked()) {
        ui->checkBoxUseAllEdges->setChecked(false);
    }

    allowFaces = !variable;
    if (selectionMode != none) {
        Gui::Selection().rmvSelectionGate();
        setSelectionGate();
    }

    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        fillet->RadiusMode.setValue(index);
        if (variable && currentEdgeShape()) {
            syncCurrentRadiusLaw();
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
}

void TaskFilletParameters::onAddControlPointToggled(bool checked)
{
    setAddControlPointMode(checked);
}

void TaskFilletParameters::onCurrentEdgeChanged(
    QListWidgetItem* current,
    [[maybe_unused]] QListWidgetItem* previous
)
{
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
        controlPointPositionEditors.clear();
        controlPointRadiusEditors.clear();
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
        ui->activeEdgeLabel->setText(tr("Variable radius requires edge selections"));
        setRadiusControlsEnabled(false);
        clearGizmos();
        ui->controlPointTable->setRowCount(0);
        controlPointPositionEditors.clear();
        controlPointRadiusEditors.clear();
        return;
    }
    ui->activeEdgeLabel->setText(
        isVariableRadius() ? tr("Radii for %1").arg(current->text())
                           : tr("Radius for %1").arg(current->text())
    );

    const auto* fillet = getObject<PartDesign::Fillet>();
    const double startRadius
        = isVariableRadius() || !fillet ? radii.start : fillet->Radius.getValue();
    QSignalBlocker startBlocker(ui->filletRadius);
    QSignalBlocker endBlocker(ui->filletEndRadius);
    ui->filletRadius->setValue(startRadius);
    ui->filletEndRadius->setValue(radii.end);

    setRadiusControlsEnabled(true);
    updateRadiusTooltip(current, radii);
    rebuildControlPointTable();
}

TaskFilletParameters::~TaskFilletParameters()
{
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
        updateFilletTypeUi();
        ensureCurrentEdge();
    }
}

void TaskFilletParameters::apply()
{
    // Alert user if he created an empty feature
    if (ui->listWidgetReferences->count() == 0) {
        std::string text = tr("Empty fillet created!").toStdString();
        Base::Console().warning("%s\n", text.c_str());
    }
}

void TaskFilletParameters::setupGizmos([[maybe_unused]] ViewProviderDressUp* vp)
{
    if (GizmoContainer::isEnabled()) {
        rebuildGizmos();
        showDraggerHints();
    }
}

void TaskFilletParameters::clearGizmos()
{
    if (gizmoContainer) {
        gizmoContainer->replaceGizmos({});
    }
    radiusGizmo = nullptr;
    radiusGizmo2 = nullptr;
    controlPointRadiusGizmos.clear();
    controlPointPositionGizmos.clear();
}

void TaskFilletParameters::rebuildControlPointTable()
{
    clearGizmos();
    controlPointPositionEditors.clear();
    controlPointRadiusEditors.clear();

    auto* current = ui->listWidgetReferences->currentItem();
    if (!current) {
        ui->controlPointTable->setRowCount(0);
        return;
    }

    auto& points = edgeRadii[current->text().toStdString()].controlPoints;
    std::ranges::sort(points, {}, &ControlPoint::position);
    ui->controlPointTable->setRowCount(static_cast<int>(points.size()));

    for (std::size_t i = 0; i < points.size(); ++i) {
        auto* name = new QTableWidgetItem(tr("CP%1").arg(i + 1));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        ui->controlPointTable->setItem(static_cast<int>(i), 0, name);

        auto* fillet = getObject<PartDesign::Fillet>();
        const std::string edgeName = current->text().toStdString();

        auto* positionEditor = new Gui::QuantitySpinBox(ui->controlPointTable);
        positionEditor->setUnit(Base::Unit());
        positionEditor->setRange(0.0, 1.0);
        positionEditor->checkRangeInExpression(true);
        positionEditor->setDecimals(6);
        positionEditor->setSingleStep(0.01);
        positionEditor->setKeyboardTracking(false);
        positionEditor->setValue(points[i].position);
        if (fillet) {
            positionEditor->bind(fillet->ensureRadiusControlPointValue(
                edgeName,
                points[i].id,
                PartDesign::Fillet::ControlPointComponent::Position,
                points[i].position
            ));
            positionEditor->setAutoApply(true);
        }
        ui->controlPointTable->setCellWidget(static_cast<int>(i), 1, positionEditor);
        controlPointPositionEditors.push_back(positionEditor);

        auto* radiusEditor = new Gui::QuantitySpinBox(ui->controlPointTable);
        radiusEditor->setUnit(Base::Unit::Length);
        radiusEditor->setMinimum(0.0);
        radiusEditor->setKeyboardTracking(false);
        radiusEditor->setValue(points[i].radius);
        if (fillet) {
            radiusEditor->bind(fillet->ensureRadiusControlPointValue(
                edgeName,
                points[i].id,
                PartDesign::Fillet::ControlPointComponent::Radius,
                points[i].radius
            ));
            radiusEditor->setAutoApply(true);
        }
        ui->controlPointTable->setCellWidget(static_cast<int>(i), 2, radiusEditor);
        controlPointRadiusEditors.push_back(radiusEditor);

        auto* removeButton = new QToolButton(ui->controlPointTable);
        removeButton->setIcon(Gui::BitmapFactory().iconFromTheme("list-remove"));
        removeButton->setToolTip(tr("Remove control point CP%1").arg(i + 1));
        removeButton->setText(tr("Remove control point CP%1").arg(i + 1));
        removeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        removeButton->setAutoRaise(true);
        ui->controlPointTable->setCellWidget(static_cast<int>(i), 3, removeButton);

        connect(removeButton, &QToolButton::clicked, this, [this, id = points[i].id]() {
            auto* active = ui->listWidgetReferences->currentItem();
            if (!active) {
                return;
            }
            auto& activePoints = edgeRadii[active->text().toStdString()].controlPoints;
            const auto found = std::ranges::find(activePoints, id, &ControlPoint::id);
            if (found == activePoints.end()) {
                return;
            }
            activePoints.erase(found);
            syncCurrentRadiusLaw();
            rebuildControlPointTable();
        });

        connect(positionEditor, qOverload<double>(&Gui::QuantitySpinBox::valueChanged), this, [this, i, positionEditor](double value) {
            auto* active = ui->listWidgetReferences->currentItem();
            if (!active) {
                return;
            }
            auto& activePoints = edgeRadii[active->text().toStdString()].controlPoints;
            if (i >= activePoints.size()) {
                return;
            }
            const double lower = i == 0 ? controlPointTolerance
                                        : activePoints[i - 1].position + controlPointTolerance;
            const double upper = i + 1 == activePoints.size()
                ? 1.0 - controlPointTolerance
                : activePoints[i + 1].position - controlPointTolerance;
            if (lower > upper) {
                QSignalBlocker blocker(positionEditor);
                positionEditor->setValue(activePoints[i].position);
                return;
            }
            const double clamped = std::clamp(value, lower, upper);
            if (clamped != value) {
                QSignalBlocker blocker(positionEditor);
                positionEditor->setValue(clamped);
            }
            activePoints[i].position = clamped;
            if (auto* fillet = getObject<PartDesign::Fillet>()) {
                fillet->setRadiusControlPointValue(
                    active->text().toStdString(),
                    activePoints[i].id,
                    PartDesign::Fillet::ControlPointComponent::Position,
                    clamped
                );
            }
            syncCurrentRadiusLaw();
            setGizmoPositions();
        });

        connect(radiusEditor, qOverload<double>(&Gui::QuantitySpinBox::valueChanged), this, [this, i](double value) {
            auto* active = ui->listWidgetReferences->currentItem();
            if (!active) {
                return;
            }
            auto& activePoints = edgeRadii[active->text().toStdString()].controlPoints;
            if (i < activePoints.size()) {
                activePoints[i].radius = value;
                if (auto* fillet = getObject<PartDesign::Fillet>()) {
                    fillet->setRadiusControlPointValue(
                        active->text().toStdString(),
                        activePoints[i].id,
                        PartDesign::Fillet::ControlPointComponent::Radius,
                        value
                    );
                }
                syncCurrentRadiusLaw();
            }
        });
    }

    rebuildGizmos();
}

void TaskFilletParameters::rebuildGizmos()
{
    auto* viewProvider = getDressUpView();
    if (!GizmoContainer::isEnabled() || !viewProvider) {
        return;
    }

    std::vector<Gui::Gizmo*> gizmos;
    radiusGizmo = new Gui::LinearGizmo(ui->filletRadius);
    radiusGizmo2 = new Gui::LinearGizmo(ui->filletEndRadius);
    gizmos.push_back(radiusGizmo);
    gizmos.push_back(radiusGizmo2);

    auto edge = currentEdgeShape();
    double edgeLength = 1.0;
    if (edge) {
        auto frame = evaluateEdgePosition(TopoDS::Edge(edge->getShape()), 0.0);
        if (frame) {
            edgeLength = frame->length;
        }
    }

    for (std::size_t i = 0; i < controlPointRadiusEditors.size(); ++i) {
        auto* radius = new Gui::LinearGizmo(controlPointRadiusEditors[i]);
        controlPointRadiusGizmos.push_back(radius);
        gizmos.push_back(radius);

        auto* active = ui->listWidgetReferences->currentItem();
        const double position = active
            ? edgeRadii[active->text().toStdString()].controlPoints[i].position
            : 0.5;
        auto* slider = new EdgePositionGizmo(
            position,
            edgeLength,
            [this, i](double value, bool finished) {
                auto* current = ui->listWidgetReferences->currentItem();
                if (!current) {
                    return;
                }
                auto& points = edgeRadii[current->text().toStdString()].controlPoints;
                if (i >= points.size()) {
                    return;
                }
                const double lower = i == 0 ? controlPointTolerance
                                            : points[i - 1].position + controlPointTolerance;
                const double upper = i + 1 == points.size()
                    ? 1.0 - controlPointTolerance
                    : points[i + 1].position - controlPointTolerance;
                if (lower > upper) {
                    return;
                }
                if (i < controlPointPositionEditors.size()
                    && controlPointPositionEditors[i]->hasExpression()) {
                    return;
                }
                points[i].position = std::clamp(value, lower, upper);
                if (auto* fillet = getObject<PartDesign::Fillet>()) {
                    fillet->setRadiusControlPointValue(
                        current->text().toStdString(),
                        points[i].id,
                        PartDesign::Fillet::ControlPointComponent::Position,
                        points[i].position
                    );
                }
                if (i < controlPointPositionEditors.size()) {
                    QSignalBlocker blocker(controlPointPositionEditors[i]);
                    controlPointPositionEditors[i]->setValue(points[i].position);
                }
                if (finished) {
                    syncCurrentRadiusLaw();
                    setGizmoPositions();
                }
            }
        );
        controlPointPositionGizmos.push_back(slider);
        gizmos.push_back(slider);
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
    if (!gizmoContainer) {
        return;
    }
    if (ui->checkBoxUseAllEdges->isChecked()) {
        gizmoContainer->visible = false;
        return;
    }

    auto fillet = getObject<PartDesign::Fillet>();
    if (!fillet || fillet->isError()) {
        gizmoContainer->visible = false;
        return;
    }
    auto* current = ui->listWidgetReferences->currentItem();
    if (!current) {
        gizmoContainer->visible = false;
        return;
    }

    Part::TopoShape baseShape = fillet->getBaseTopoShape(true);
    const int row = ui->listWidgetReferences->row(current);
    const auto refs = fillet->Base.getSubValues(true);
    if (row < 0 || static_cast<std::size_t>(row) >= refs.size()) {
        gizmoContainer->visible = false;
        return;
    }

    Part::TopoShape edge = baseShape.getSubTopoShape(refs[row].c_str(), true);
    if (edge.isNull() || edge.shapeType() != TopAbs_EDGE) {
        gizmoContainer->visible = false;
        return;
    }
    gizmoContainer->visible = true;

    auto [face1, face2] = getAdjacentFacesFromEdge(edge, baseShape);
    DraggerPlacementProps props1 = getDraggerPlacementFromEdgeAndFace(edge, face1);
    DraggerPlacementProps props2 = getDraggerPlacementFromEdgeAndFace(edge, face2);

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
    }

    radiusGizmo->Gizmo::setDraggerPlacement(props1.position, props1.dir);
    if (isVariableRadius()) {
        radiusGizmo2->Gizmo::setDraggerPlacement(props2.position, props2.dir);
    }

    // The dragger length won't be equal to the radius if the two faces
    // are not orthogonal so this correction is needed
    const double angle = props1.dir.GetAngle(props2.dir);
    const double tangent = std::tan(angle / 2.0);
    const double correction
        = std::abs(tangent) > Precision::Angular() ? 1.0 / tangent : 1.0;

    radiusGizmo->setMultFactor(correction);
    radiusGizmo2->setMultFactor(correction);

    const auto& points = edgeRadii[current->text().toStdString()].controlPoints;
    const std::size_t count = std::min(
        points.size(),
        std::min(controlPointRadiusGizmos.size(), controlPointPositionGizmos.size())
    );
    for (std::size_t i = 0; i < count; ++i) {
        auto frame = evaluateEdgePosition(TopoDS::Edge(edge.getShape()), points[i].position);
        if (!frame) {
            controlPointRadiusGizmos[i]->setVisibility(false);
            controlPointPositionGizmos[i]->setVisibility(false);
            continue;
        }
        auto* radius = controlPointRadiusGizmos[i];
        radius->Gizmo::setDraggerPlacement(frame->position, props1.dir);
        radius->setMultFactor(correction);
        radius->setOriginLabel(tr("CP%1").arg(i + 1).toStdString());
        radius->setVisibility(isVariableRadius());

        auto* slider = controlPointPositionGizmos[i];
        slider->setPosition(points[i].position, frame->length);
        slider->Gizmo::setDraggerPlacement(frame->position, frame->tangent);
        const bool positionHasExpression = i < controlPointPositionEditors.size()
            && controlPointPositionEditors[i]->hasExpression();
        slider->setVisibility(isVariableRadius() && !positionHasExpression);
    }
}

void TaskFilletParameters::setRadiusControlsEnabled(bool enabled)
{
    ui->startRadiusLabel->setEnabled(enabled);
    ui->endRadiusLabel->setEnabled(enabled && isVariableRadius());
    ui->filletRadius->setEnabled(enabled);
    ui->filletEndRadius->setEnabled(enabled && isVariableRadius());
    ui->controlPointTable->setEnabled(enabled && isVariableRadius());
    ui->addControlPointButton->setEnabled(enabled && isVariableRadius());
}

void TaskFilletParameters::updateRadiusTooltip(QListWidgetItem* item, const EdgeRadii& radii)
{
    const Base::Quantity start(radii.start, Base::Unit::Length);
    const Base::Quantity end(radii.end, Base::Unit::Length);
    if (isVariableRadius()) {
        item->setToolTip(
            tr("Start radius: %1\nEnd radius: %2")
                .arg(QString::fromStdString(start.getUserString()))
                .arg(QString::fromStdString(end.getUserString()))
        );
    }
    else {
        const auto* fillet = getObject<PartDesign::Fillet>();
        const Base::Quantity radius(
            fillet ? fillet->Radius.getValue() : radii.start,
            Base::Unit::Length
        );
        item->setToolTip(tr("Radius: %1").arg(QString::fromStdString(radius.getUserString())));
    }
}

void TaskFilletParameters::updateFilletTypeUi()
{
    const bool variable = isVariableRadius();
    ui->startRadiusLabel->setText(variable ? tr("Start") : tr("Radius"));
    ui->positionHeaderLabel->setVisible(variable);
    ui->radiusHeaderLabel->setVisible(variable);
    ui->filletStartPosition->setVisible(variable);
    ui->endRadiusLabel->setVisible(variable);
    ui->filletEndPosition->setVisible(variable);
    ui->filletEndRadius->setVisible(variable);
    ui->controlPointTable->setVisible(variable);
    ui->addControlPointButton->setVisible(variable);
    ui->checkBoxUseAllEdges->setEnabled(!variable);
    ui->addControlPointButton->setEnabled(
        variable && !ui->checkBoxUseAllEdges->isChecked()
        && ui->listWidgetReferences->currentItem()
    );

    if (radiusGizmo && radiusGizmo2) {
        radiusGizmo->setOriginLabel(variable ? tr("Start").toStdString() : std::string());
        radiusGizmo2->setOriginLabel(variable ? tr("End").toStdString() : std::string());
        radiusGizmo2->setVisibility(variable);
        for (auto* gizmo : controlPointRadiusGizmos) {
            gizmo->setVisibility(variable);
        }
        for (auto* gizmo : controlPointPositionGizmos) {
            gizmo->setVisibility(variable);
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
        ui->listWidgetReferences->setCurrentRow(0);
        return;
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
        ui->activeEdgeLabel->setText(tr("Click a position on the active edge"));
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
    auto* current = ui->listWidgetReferences->currentItem();
    if (!fillet || !base || !current || !msg.hasPickedPoint
        || strcmp(msg.pDocName, fillet->getDocument()->getName()) != 0
        || strcmp(msg.pObjectName, base->getNameInDocument()) != 0) {
        return false;
    }

    const int row = ui->listWidgetReferences->row(current);
    const auto resolvedRefs = fillet->Base.getSubValues(true);
    const QString pickedRef = QString::fromUtf8(msg.pSubName);
    if (row < 0 || static_cast<std::size_t>(row) >= resolvedRefs.size()
        || (current->text() != pickedRef
            && QString::fromStdString(resolvedRefs[row]) != pickedRef)) {
        return false;
    }

    auto edge = currentEdgeShape();
    if (!edge) {
        return false;
    }
    Base::Matrix4D transform;
    if (!base->getSubObject(msg.pSubName, nullptr, &transform, true, 0)) {
        return false;
    }
    transform.inverse();
    const Base::Vector3d localPoint
        = transform * Base::Vector3d(msg.x, msg.y, msg.z);
    auto position = projectPointToEdge(
        TopoDS::Edge(edge->getShape()),
        gp_Pnt(localPoint.x, localPoint.y, localPoint.z)
    );
    if (!position || *position <= controlPointTolerance
        || *position >= 1.0 - controlPointTolerance) {
        return false;
    }

    auto& radii = edgeRadii[current->text().toStdString()];
    auto& points = radii.controlPoints;
    if (std::ranges::any_of(points, [position](const ControlPoint& point) {
            return std::abs(point.position - *position) <= controlPointTolerance;
        })) {
        return false;
    }

    double leftPosition = 0.0;
    double leftRadius = radii.start;
    double rightPosition = 1.0;
    double rightRadius = radii.end;
    for (const auto& point : points) {
        if (point.position < *position) {
            leftPosition = point.position;
            leftRadius = point.radius;
        }
        else {
            rightPosition = point.position;
            rightRadius = point.radius;
            break;
        }
    }
    const double fraction = (*position - leftPosition) / (rightPosition - leftPosition);
    const double radius = std::lerp(leftRadius, rightRadius, fraction);
    points.push_back(ControlPoint {
        *position,
        radius,
        fillet->newRadiusControlPointId(current->text().toStdString())
    });
    std::ranges::sort(points, {}, &ControlPoint::position);
    syncCurrentRadiusLaw();
    rebuildControlPointTable();
    return true;
}

void TaskFilletParameters::syncCurrentRadiusLaw()
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* current = ui->listWidgetReferences->currentItem();
    if (!fillet || !current) {
        return;
    }

    const auto found = edgeRadii.find(current->text().toStdString());
    if (found == edgeRadii.end()) {
        return;
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
    fillet->recomputeFeature();
    hideOnError();
}

std::optional<Part::TopoShape> TaskFilletParameters::currentEdgeShape() const
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* current = ui->listWidgetReferences->currentItem();
    if (!fillet || !current) {
        return std::nullopt;
    }

    Part::TopoShape baseShape = fillet->getBaseTopoShape(true);
    const int row = ui->listWidgetReferences->row(current);
    const auto refs = fillet->Base.getSubValues(true);
    if (row < 0 || static_cast<std::size_t>(row) >= refs.size()) {
        return std::nullopt;
    }
    Part::TopoShape edge = baseShape.getSubTopoShape(refs[row].c_str(), true);
    if (edge.isNull() || edge.shapeType() != TopAbs_EDGE) {
        return std::nullopt;
    }
    return edge;
}

//**************************************************************************
//**************************************************************************
// TaskDialog
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TaskDlgFilletParameters::TaskDlgFilletParameters(ViewProviderFillet* DressUpView)
    : TaskDlgDressUpParameters(DressUpView)
{
    parameter = new TaskFilletParameters(DressUpView);

    Content.push_back(parameter);
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
