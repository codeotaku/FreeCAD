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
#include <QTreeWidget>
#include <QTimer>
#include <QKeyEvent>
#include <Inventor/nodes/SoSphere.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/SoRenderManager.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

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
#include "FilletPointEditor.h"
#include <Gui/Dialogs/DlgExpressionInput.h>


using namespace PartDesignGui;
using namespace Gui;

/* TRANSLATOR PartDesignGui::TaskFilletParameters */

namespace
{
constexpr double controlPointTolerance = 1.0e-4;

void filletHandle(Gui::SoLinearDragger* dragger, bool ball)
{
    auto* arrow = SO_GET_PART(dragger, "arrow", Gui::SoArrowGeometry);
    arrow->cylinderHeight = 0;
    arrow->coneHeight = 0;
    arrow->cylinderRadius = 0;
    arrow->coneBottomRadius = 0;
    // The dragger uses this vector to orient the handle, independently of its mesh.
    arrow->tipPosition = SbVec3f(0, 1, 0);
    dragger->baseGeomVisible = false;
    SoSeparator* separator = nullptr;
    for (int i = 0; i < arrow->getChildren()->getLength(); ++i) {
        auto* child = (*arrow->getChildren())[i];
        if (child->isOfType(SoSeparator::getClassTypeId())) {
            separator = static_cast<SoSeparator*>(child);
            break;
        }
    }
    if (!separator) {
        return;
    }
    if (ball) {
        // SoArrowGeometry normally uses BASE_COLOR, which flattens spheres into discs.
        auto* lighting = new SoLightModel;
        lighting->model = SoLightModel::PHONG;
        separator->addChild(lighting);
        auto* gold = new SoMaterial;
        gold->diffuseColor = SbColor(.9F, .62F, .16F);
        gold->ambientColor = SbColor(.3F, .19F, .04F);
        gold->specularColor = SbColor(1.F, .91F, .62F);
        gold->shininess = .65F;
        separator->addChild(gold);
        auto* sphere = new SoSphere;
        sphere->radius = .6F;
        separator->addChild(sphere);
        dragger->baseGeomVisible = false;
    }
    else {
        auto* style = new SoDrawStyle;
        style->lineWidth = 1.5F;
        separator->addChild(style);
        auto* coords = new SoCoordinate3;
        for (int i = 0; i <= 32; ++i) {
            const double angle = 2.0 * std::numbers::pi * i / 32;
            coords->point.set1Value(i, .65F * std::cos(angle), .65F * std::sin(angle), 0);
        }
        separator->addChild(coords);
        auto* line = new SoLineSet;
        line->numVertices = 33;
        separator->addChild(line);
    }
}

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
        getDraggerContainer()->color = SbColor(.9F, .68F, .23F);
        filletHandle(dragger, false);
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
        container->color.setValue(0.95F, 0.75F, 0.15F);
        dragger = container->getDragger();
        dragger->setName("filletPositionHandle");
        dragger->labelVisible = false;
        dragger->instantiateBaseGeometry();
        filletHandle(dragger, true);
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
    setupPointEditor();

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
            if (shapeChanged) {
                const auto refs = pcFillet->Base.getSubValues();
                if (refs.size() != static_cast<std::size_t>(ui->listWidgetReferences->count())) {
                    return;
                }
                for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
                    const auto index = static_cast<std::size_t>(row);
                    if (refs[index] != ui->listWidgetReferences->item(row)->text().toStdString()) {
                        return;
                    }
                }
            }
            controlPointValueRefreshRequested = controlPointValueRefreshRequested || valuesChanged
                || radiusChanged || expressionsChanged;
            controlPointGeometryRefreshRequested = controlPointGeometryRefreshRequested
                || shapeChanged;
            if (controlPointRefreshQueued) {
                return;
            }
            controlPointRefreshQueued = true;
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    controlPointRefreshQueued = false;
                    const bool refreshValues = controlPointValueRefreshRequested;
                    const bool refreshGeometry = controlPointGeometryRefreshRequested;
                    controlPointValueRefreshRequested = false;
                    controlPointGeometryRefreshRequested = false;
                    if (refreshValues) {
                        refreshControlPointValuesFromModel();
                    }
                    else if (refreshGeometry) {
                        refreshEdgeTree();
                    }
                    updateFilletTypeUi();
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
    allowFaces = !variableRadius;

    ui->filletRadius->setUnit(Base::Unit::Length);
    ui->filletRadius->setValue(r);
    ui->filletRadius->setMinimum(0);
    ui->filletRadius->selectNumber();

    ui->filletEndRadius->setUnit(Base::Unit::Length);
    ui->filletEndRadius->setValue(r);
    ui->filletEndRadius->setMinimum(0);

    ui->defaultRadiusEditor->setUnit(Base::Unit::Length);
    ui->defaultRadiusEditor->setValue(r);
    ui->defaultRadiusEditor->setMinimum(0);
    ui->defaultRadiusEditor->bind(pcFillet->Radius);
    ui->defaultRadiusEditor->setAutoApply(true);

    ui->treeWidgetReferences->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->treeWidgetReferences->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->treeWidgetReferences->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->treeWidgetReferences->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    ui->controlPointTable->verticalHeader()->hide();
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->controlPointTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    ui->controlPointTable->setSelectionMode(QAbstractItemView::NoSelection);
    ui->addControlPointButton->setIcon(Gui::BitmapFactory().iconFromTheme("list-add"));
    ui->addControlPointButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QMetaObject::invokeMethod(
        ui->defaultRadiusEditor,
        [editor = ui->defaultRadiusEditor]() {
            editor->setFocus();
            editor->selectNumber();
        },
        Qt::QueuedConnection
    );
    std::vector<std::string> strings = pcFillet->Base.getSubValues();
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
    connect(ui->filletRadius, &Gui::QuantitySpinBox::showFormulaDialog,
        this, [this](bool shown) {
            if (!shown) {
                refreshControlPointValuesFromModel();
                setGizmoPositions();
            }
        });
    connect(ui->filletEndRadius, &Gui::QuantitySpinBox::showFormulaDialog,
        this, [this](bool shown) {
            if (!shown) {
                refreshControlPointValuesFromModel();
                setGizmoPositions();
            }
        });
    connect(ui->filletType, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &TaskFilletParameters::onFilletTypeChanged);
    connect(ui->addControlPointButton, &QToolButton::toggled,
        this, &TaskFilletParameters::onAddControlPointToggled);

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

    refreshEdgeTree();
    setupGizmos(DressUpView);
    updateFilletTypeUi();
    ensureCurrentEdge();
    if (variableRadius && !strings.empty() && pcFillet->getRadiusProfiles().empty()) {
        updatePreview();
    }
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
                if (inserted && isVariableRadius()) {
                    syncRadiusLaw(subName);
                }
            }
            else {
                edgeRadii.erase(subName);
                ensureCurrentEdge();
            }
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
    ui->buttonRefSel->setText(selectingReferences ? stopSelectionLabel() : startSelectionLabel());
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
    refreshEdgeTree();
    rebuildAllGizmos();
}

void TaskFilletParameters::onAddAllEdges()
{
    TaskDressUpParameters::addAllEdges(ui->listWidgetReferences);

    auto fillet = getObject<PartDesign::Fillet>();
    if (!fillet) {
        return;
    }

    std::vector<std::string> newEdges;
    for (const auto& ref : fillet->Base.getSubValues()) {
        const QString text = QString::fromStdString(ref);
        if (ui->listWidgetReferences->findItems(text, Qt::MatchExactly).empty()) {
            ui->listWidgetReferences->addItem(text);
        }
        if (edgeRadii.try_emplace(ref, EdgeRadii {defaultRadius, defaultRadius, {}}).second) {
            newEdges.push_back(ref);
        }
    }
    ensureCurrentEdge();
    if (isVariableRadius()) {
        for (const auto& edgeName : newEdges) {
            syncRadiusLaw(edgeName);
        }
    }
    refreshEdgeTree();
    rebuildAllGizmos();
}

void TaskFilletParameters::onStartRadiusChanged(double value)
{
    if (!isVariableRadius()) {
        defaultRadius = value;
        {
            QSignalBlocker blocker(ui->defaultRadiusEditor);
            ui->defaultRadiusEditor->setValue(value);
        }
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
            refreshEdgeTree();
        }
        return;
    }

    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    const std::string edgeName = item->text().toStdString();
    auto& radii
        = edgeRadii.try_emplace(edgeName, EdgeRadii {defaultRadius, defaultRadius, {}}).first->second;
    radii.start = value;
    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        fillet->setRadiusControlPointValue(
            edgeName,
            "start",
            PartDesign::Fillet::ControlPointComponent::Radius,
            value
        );
    }
    updateRadiusTooltip(item, radii);
    syncCurrentRadiusLaw();
}

void TaskFilletParameters::onEndRadiusChanged(double value)
{
    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    const std::string edgeName = item->text().toStdString();
    auto& radii
        = edgeRadii.try_emplace(edgeName, EdgeRadii {defaultRadius, defaultRadius, {}}).first->second;
    radii.end = value;
    if (auto* fillet = getObject<PartDesign::Fillet>()) {
        fillet->setRadiusControlPointValue(
            edgeName,
            "end",
            PartDesign::Fillet::ControlPointComponent::Radius,
            value
        );
    }
    updateRadiusTooltip(item, radii);
    syncCurrentRadiusLaw();
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
        fillet->recomputeFeature();
        hideOnError();
        refreshEdgeTree();
        setGizmoPositions();
    }
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
    refreshEdgeTree();
    rebuildAllGizmos();
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
        controlPointPositionEditors.clear();
        controlPointLengthEditors.clear();
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
        controlPointLengthEditors.clear();
        controlPointRadiusEditors.clear();
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
    QSignalBlocker startBlocker(ui->filletRadius);
    QSignalBlocker endBlocker(ui->filletEndRadius);
    ui->filletRadius->setValue(startRadius);
    ui->filletEndRadius->setValue(radii.end);

    setRadiusControlsEnabled(true);
    updateRadiusTooltip(current, radii);
    rebuildControlPointTable();
    setGizmoPositions();
}


TaskFilletParameters::~TaskFilletParameters()
{
    delete inlineEditor.data();
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
    controlPointGizmos.clear();
    for (auto* editor : auxiliaryControlPointEditors) {
        delete editor;
    }
    auxiliaryControlPointEditors.clear();
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
    const auto selectedIds = selectedPointIds();
    const QString currentEdge = ui->listWidgetReferences->currentItem()
        ? ui->listWidgetReferences->currentItem()->text()
        : QString();
    QSignalBlocker blocker(ui->treeWidgetReferences);
    ui->treeWidgetReferences->clear();
    for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
        const QString edgeText = ui->listWidgetReferences->item(row)->text();
        const std::string name = edgeText.toStdString();
        const double length = edgeLength(name).value_or(0);
        auto* item = new QTreeWidgetItem(ui->treeWidgetReferences, {edgeText});
        item->setExpanded(edgeText == currentEdge);
        const auto radii = edgeRadii.find(name);
        if (radii == edgeRadii.end()) {
            continue;
        }
        const auto quantity = [](double r) {
            return QString::fromStdString(Base::Quantity(r, Base::Unit::Length).getUserString());
        };
        item->setText(1, quantity(isVariableRadius() ? length : defaultRadius));
        if (isVariableRadius()) {
            const auto append = [&](const std::string& id, double t, double r) {
                const QString label = id == "start" ? tr("Start · 0%")
                    : id == "end"                   ? tr("End · 100%")
                    : fillet->isRadiusControlPointAbsolute(name, id)
                    ? quantity(t * length)
                    : QString::number(t * 100, 'g', 6) + QStringLiteral("%");
                auto* child = new QTreeWidgetItem(item, {label, quantity(r)});
                child->setData(0, Qt::UserRole, QString::fromStdString(id));
                if (edgeText == currentEdge) {
                    child->setSelected(std::ranges::find(selectedIds, id) != selectedIds.end());
                    if (id == activePoint) {
                        ui->treeWidgetReferences->setCurrentItem(child);
                    }
                }
            };
            append("start", 0, radii->second.start);
            for (const auto& p : radii->second.controlPoints) {
                append(p.id, p.position, p.radius);
            }
            append("end", 1, radii->second.end);
        }
        else {
            if (edgeText == currentEdge) {
                ui->treeWidgetReferences->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            }
            item->setSelected(ui->listWidgetReferences->item(row)->isSelected());
        }
    }
    refreshPointEditor();
}


void TaskFilletParameters::refreshControlPointValuesFromModel()
{
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
        bool edgeStructureChanged = radii.controlPoints.size() != ids.size();
        if (!edgeStructureChanged) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (radii.controlPoints[i].id != ids[i]) {
                    edgeStructureChanged = true;
                    break;
                }
            }
        }

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

    if (structureChanged) {
        rebuildControlPointTable();
        rebuildAllGizmos();
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
        QSignalBlocker positionBlocker(gizmo.positionEditor);
        QSignalBlocker radiusBlocker(gizmo.radiusEditor);
        gizmo.positionEditor->setValue(point->position);
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
    for (int row = 0; row < ui->treeWidgetReferences->topLevelItemCount(); ++row) {
        auto* treeItem = ui->treeWidgetReferences->topLevelItem(row);
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
        auto* edgeItem = item->parent() ? item->parent() : item;
        if (std::ranges::find(selectedEdges, edgeItem->text(0)) == selectedEdges.end()) {
            selectedEdges.push_back(edgeItem->text(0));
        }
    }

    auto* currentTreeItem = ui->treeWidgetReferences->currentItem();
    if (currentTreeItem && currentTreeItem->parent()) {
        activePoint = currentTreeItem->data(0, Qt::UserRole).toString().toStdString();
        currentTreeItem = currentTreeItem->parent();
    }
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
    refreshPointEditor();
}

void TaskFilletParameters::rebuildControlPointTable()
{
    // Point fields now live in the shared selected-point editor, not a second table.
    controlPointPositionEditors.clear();
    controlPointLengthEditors.clear();
    controlPointRadiusEditors.clear();
    ui->controlPointTable->setRowCount(0);
    refreshPointEditor();
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
    Part::TopoShape baseShape;
    std::vector<std::string> refs;
    if (fillet) {
        try {
            baseShape = fillet->getBaseTopoShape(true);
            refs = fillet->Base.getSubValues(true);
        }
        catch (const Base::Exception&) {
            baseShape = Part::TopoShape();
            refs.clear();
        }
    }

    for (int edgeRow = 0; edgeRow < ui->listWidgetReferences->count(); ++edgeRow) {
        const std::string edgeName = ui->listWidgetReferences->item(edgeRow)->text().toStdString();
        auto found = edgeRadii.find(edgeName);
        if (found == edgeRadii.end()) {
            continue;
        }

        double edgeLength = 1.0;
        if (!baseShape.isNull() && static_cast<std::size_t>(edgeRow) < refs.size()) {
            const Part::TopoShape edge = baseShape.getSubTopoShape(refs[edgeRow].c_str(), true);
            if (!edge.isNull() && edge.shapeType() == TopAbs_EDGE) {
                const auto frame = evaluateEdgePosition(TopoDS::Edge(edge.getShape()), 0.0);
                if (frame) {
                    edgeLength = frame->length;
                }
            }
        }

        auto& points = found->second.controlPoints;
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            Gui::QuantitySpinBox* positionEditor = nullptr;
            Gui::QuantitySpinBox* radiusEditor = nullptr;
            if (fillet) {
                positionEditor = new Gui::QuantitySpinBox(proxy);
                positionEditor->setUnit(Base::Unit());
                positionEditor->setRange(0.0, 1.0);
                positionEditor->checkRangeInExpression(true);
                positionEditor->setValue(points[pointIndex].position);
                positionEditor->setVisible(false);
                positionEditor->bind(fillet->ensureRadiusControlPointValue(
                    edgeName,
                    points[pointIndex].id,
                    PartDesign::Fillet::ControlPointComponent::Position,
                    points[pointIndex].position
                ));
                positionEditor->setAutoApply(true);
                auxiliaryControlPointEditors.push_back(positionEditor);

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
                        auto edge = edgeRadii.find(edgeName);
                        if (edge == edgeRadii.end()) {
                            return;
                        }
                        const auto point
                            = std::ranges::find(edge->second.controlPoints, id, &ControlPoint::id);
                        if (point == edge->second.controlPoints.end()) {
                            return;
                        }
                        point->radius = value;
                        if (auto* fillet = getObject<PartDesign::Fillet>()) {
                            fillet->setRadiusControlPointValue(
                                edgeName,
                                id,
                                PartDesign::Fillet::ControlPointComponent::Radius,
                                value
                            );
                        }
                        syncRadiusLaw(edgeName);
                    }
                );
                radiusEditor->bind(fillet->ensureRadiusControlPointValue(
                    edgeName,
                    points[pointIndex].id,
                    PartDesign::Fillet::ControlPointComponent::Radius,
                    points[pointIndex].radius
                ));
                radiusEditor->setAutoApply(true);
                auxiliaryControlPointEditors.push_back(radiusEditor);
            }

            if (!positionEditor || !radiusEditor) {
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
                [this, edgeName, id = points[pointIndex].id, positionEditor](double value, bool finished) {
                    auto edge = edgeRadii.find(edgeName);
                    auto* feature = getObject<PartDesign::Fillet>();
                    const bool lengthBound = feature
                        && feature
                               ->getExpression(feature->VariableRadiusControlPointValues.getItemPath(
                                   edgeName + "|" + id + "|length"
                               ))
                               .expression;
                    if (edge == edgeRadii.end() || positionEditor->hasExpression() || lengthBound) {
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
                    point->position = std::clamp(value, lower, upper);
                    if (auto* fillet = getObject<PartDesign::Fillet>()) {
                        fillet->setRadiusControlPointPosition(
                            edgeName,
                            id,
                            point->position,
                            this->edgeLength(edgeName).value_or(0),
                            fillet->isRadiusControlPointAbsolute(edgeName, id)
                        );
                    }
                    QSignalBlocker blocker(positionEditor);
                    positionEditor->setValue(point->position);
                    if (finished) {
                        syncRadiusLaw(edgeName);
                        setGizmoPositions();
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
                {edgeName, points[pointIndex].id, radius, slider, positionEditor, radiusEditor}
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
    if (!gizmoContainer) {
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
        if (startPointGizmo && endPointGizmo) {
            startPointGizmo->Gizmo::setDraggerPlacement(start->position, start->tangent);
            endPointGizmo->Gizmo::setDraggerPlacement(end->position, end->tangent);
        }
    }

    radiusGizmo->Gizmo::setDraggerPlacement(props1.position, props1.dir);
    if (isVariableRadius()) {
        radiusGizmo2->Gizmo::setDraggerPlacement(props2.position, props1.dir);
    }

    // The dragger length won't be equal to the radius if the two faces
    // are not orthogonal so this correction is needed
    const double angle = props1.dir.GetAngle(props2.dir);
    const double tangent = std::tan(angle / 2.0);
    const double correction = std::abs(tangent) > Precision::Angular() ? 1.0 / tangent : 1.0;

    radiusGizmo->setMultFactor(correction);
    radiusGizmo2->setMultFactor(correction);

    for (auto& gizmo : controlPointGizmos) {
        const auto listItems = ui->listWidgetReferences->findItems(
            QString::fromStdString(gizmo.edgeName),
            Qt::MatchExactly
        );
        if (listItems.empty()) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }
        const int edgeRow = ui->listWidgetReferences->row(listItems.front());
        if (edgeRow < 0 || static_cast<std::size_t>(edgeRow) >= refs.size()) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }

        Part::TopoShape controlEdge = baseShape.getSubTopoShape(refs[edgeRow].c_str(), true);
        auto radii = edgeRadii.find(gizmo.edgeName);
        if (controlEdge.isNull() || controlEdge.shapeType() != TopAbs_EDGE
            || radii == edgeRadii.end()) {
            gizmo.radius->setVisibility(false);
            gizmo.position->setVisibility(false);
            continue;
        }
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

        auto [controlFace1, controlFace2] = getAdjacentFacesFromEdge(controlEdge, baseShape);
        const DraggerPlacementProps controlProps1
            = getDraggerPlacementFromEdgeAndFace(controlEdge, controlFace1);
        const DraggerPlacementProps controlProps2
            = getDraggerPlacementFromEdgeAndFace(controlEdge, controlFace2);
        const double controlAngle = controlProps1.dir.GetAngle(controlProps2.dir);
        const double controlTangent = std::tan(controlAngle / 2.0);
        const double controlCorrection = std::abs(controlTangent) > Precision::Angular()
            ? 1.0 / controlTangent
            : 1.0;
        gizmo.radius->Gizmo::setDraggerPlacement(frame->position, controlProps1.dir);
        gizmo.radius->setMultFactor(controlCorrection);
        gizmo.radius->setOriginLabel({});
        gizmo.radius->setVisibility(isVariableRadius());

        if (!gizmo.position->isDragging()) {
            gizmo.position->setPosition(point->position, frame->length);
            gizmo.position->Gizmo::setDraggerPlacement(frame->position, frame->tangent);
        }
        gizmo.position->setVisibility(isVariableRadius() && !gizmo.positionEditor->hasExpression());
    }
}

void TaskFilletParameters::setRadiusControlsEnabled(bool enabled)
{
    ui->startRadiusLabel->setEnabled(enabled);
    ui->endRadiusLabel->setEnabled(enabled && isVariableRadius());
    ui->filletRadius->setEnabled(enabled);
    ui->filletEndRadius->setEnabled(enabled && isVariableRadius());
    const bool canEditPoints = enabled && isVariableRadius();
    ui->controlPointTable->setEnabled(canEditPoints);
    ui->addControlPointButton->setEnabled(canEditPoints);
}

void TaskFilletParameters::updateRadiusTooltip(QListWidgetItem* item, const EdgeRadii& radii)
{
    const Base::Quantity start(radii.start, Base::Unit::Length);
    const Base::Quantity end(radii.end, Base::Unit::Length);
    if (isVariableRadius()) {
        item->setToolTip(tr("Start radius: %1\nEnd radius: %2")
                             .arg(QString::fromStdString(start.getUserString()))
                             .arg(QString::fromStdString(end.getUserString())));
    }
    else {
        const auto* fillet = getObject<PartDesign::Fillet>();
        const Base::Quantity radius(fillet ? fillet->Radius.getValue() : radii.start, Base::Unit::Length);
        item->setToolTip(tr("Radius: %1").arg(QString::fromStdString(radius.getUserString())));
    }
}

void TaskFilletParameters::updateFilletTypeUi()
{
    const bool variable = isVariableRadius();
    ui->treeWidgetReferences->header()->setVisible(false);
    ui->treeWidgetReferences->setRootIsDecorated(variable);
    ui->activeEdgeLabel->hide();
    ui->positionHeaderLabel->hide();
    ui->radiusHeaderLabel->hide();
    ui->startRadiusLabel->hide();
    ui->filletStartPosition->hide();
    ui->filletRadius->hide();
    ui->endRadiusLabel->hide();
    ui->filletEndPosition->hide();
    ui->filletEndRadius->hide();
    ui->controlPointTable->hide();
    ui->addControlPointButton->hide();
    pointEditor->setVisible(variable);
    ui->checkBoxUseAllEdges->setEnabled(!variable);
    ui->checkBoxUseAllEdges->setVisible(!variable);
    const bool canEditPoints = variable && !ui->checkBoxUseAllEdges->isChecked()
        && ui->listWidgetReferences->currentItem();
    ui->addControlPointButton->setEnabled(canEditPoints);

    if (radiusGizmo && radiusGizmo2) {
        radiusGizmo->setOriginLabel({});
        radiusGizmo->setVisibility(variable || !ui->defaultRadiusEditor->hasExpression());
        radiusGizmo2->setOriginLabel({});
        radiusGizmo2->setVisibility(variable);
        for (auto& gizmo : controlPointGizmos) {
            gizmo.radius->setVisibility(variable);
            gizmo.position->setVisibility(variable && !gizmo.positionEditor->hasExpression());
        }
    }
    setGizmoPositions();
    refreshPointEditor();
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
    pointEditor->setAdding(enabled);
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

    const auto resolvedRefs = fillet->Base.getSubValues(true);
    const QString pickedRef = QString::fromUtf8(msg.pSubName);
    int edgeRow = -1;
    for (int row = 0; row < ui->listWidgetReferences->count(); ++row) {
        const auto index = static_cast<std::size_t>(row);
        if (index >= resolvedRefs.size()) {
            break;
        }
        if (ui->listWidgetReferences->item(row)->text() == pickedRef
            || QString::fromStdString(resolvedRefs[index]) == pickedRef) {
            edgeRow = row;
            break;
        }
    }
    if (edgeRow < 0) {
        return false;
    }

    Part::TopoShape edge;
    try {
        const Part::TopoShape baseShape = fillet->getBaseTopoShape(true);
        edge = baseShape.getSubTopoShape(resolvedRefs[static_cast<std::size_t>(edgeRow)].c_str(), true);
    }
    catch (const Base::Exception&) {
        return false;
    }
    if (edge.isNull() || edge.shapeType() != TopAbs_EDGE) {
        return false;
    }

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
    refreshEdgeTree();
}

void TaskFilletParameters::syncCurrentRadiusLaw()
{
    auto* current = ui->listWidgetReferences->currentItem();
    if (current) {
        syncRadiusLaw(current->text().toStdString());
    }
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

std::optional<double> TaskFilletParameters::currentEdgeLength() const
{
    const auto* current = ui->listWidgetReferences->currentItem();
    return current ? edgeLength(current->text().toStdString()) : std::nullopt;
}

std::optional<double> TaskFilletParameters::edgeLength(const std::string& edgeName) const
{
    auto* fillet = getObject<PartDesign::Fillet>();
    const auto items
        = ui->listWidgetReferences->findItems(QString::fromStdString(edgeName), Qt::MatchExactly);
    if (!fillet || items.empty()) {
        return std::nullopt;
    }

    const int row = ui->listWidgetReferences->row(items.front());
    const auto refs = fillet->Base.getSubValues(true);
    if (row < 0 || static_cast<std::size_t>(row) >= refs.size()) {
        return std::nullopt;
    }
    const Part::TopoShape baseShape = fillet->getBaseTopoShape(true);
    const Part::TopoShape edge = baseShape.getSubTopoShape(refs[row].c_str(), true);
    if (edge.isNull() || edge.shapeType() != TopAbs_EDGE) {
        return std::nullopt;
    }
    const auto frame = evaluateEdgePosition(TopoDS::Edge(edge.getShape()), 0.0);
    return frame ? std::optional<double>(frame->length) : std::nullopt;
}

//**************************************************************************
//**************************************************************************
// TaskDialog
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void TaskFilletParameters::setupPointEditor()
{
    pointEditor = new FilletPointEditor(proxy);
    ui->verticalLayout->addWidget(pointEditor);
    ui->verticalLayout->removeItem(ui->filletTypeLayout);
    ui->verticalLayout->insertLayout(0, ui->filletTypeLayout);
    ui->treeWidgetReferences->setColumnHidden(2, true);
    ui->treeWidgetReferences->setColumnHidden(3, true);
    ui->treeWidgetReferences->setMaximumHeight(155);
    ui->treeWidgetReferences->setMinimumHeight(120);
    const auto connectEditor = [this](FilletPointEditor* editor) {
        editor->edited = [this](const std::string& id, double t, double r, bool absolute) {
            editPoint(id, t, r, absolute);
        };
        editor->selected = [this](const std::string& id) {
            selectPoint(id);
        };
        editor->inserted = [this](double t) {
            insertPoint(t);
        };
        editor->action = [this](const std::string& action) {
            pointAction(action);
        };
        editor->gesture = [this](bool started) {
            if (started) {
                beginPointEdit();
            }
            else {
                finishPointEdit();
            }
        };
    };
    connectEditor(pointEditor);
    auto* vp = getDressUpView();
    auto* view = vp ? dynamic_cast<Gui::View3DInventor*>(vp->getDocument()->getActiveView())
                    : nullptr;
    if (view) {
        inlineEditor = new FilletPointEditor(view->getViewer()->getGLWidget(), true);
        inlineEditor->setMaximumWidth(300);
        inlineEditor->hide();
        connectEditor(inlineEditor);
        view->getViewer()->getGLWidget()->installEventFilter(this);
    }
    auto* timer = new QTimer(this);
    timer->setInterval(80);
    connect(timer, &QTimer::timeout, this, &TaskFilletParameters::updateInlinePlacement);
    timer->start();
}

void TaskFilletParameters::refreshPointEditor()
{
    if (!pointEditor || refreshingEditor) {
        return;
    }
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* item = ui->listWidgetReferences->currentItem();
    if (!fillet || !item || !isVariableRadius()) {
        pointEditor->setEnabled(false);
        if (inlineEditor) {
            inlineEditor->hide();
        }
        return;
    }
    refreshingEditor = true;
    const std::string name = item->text().toStdString();
    const auto found = edgeRadii.find(name);
    if (found == edgeRadii.end()) {
        refreshingEditor = false;
        return;
    }
    std::vector<FilletEditorPoint> points;
    const auto append = [&](const std::string& id, double t, double r) {
        const auto path = [&](PartDesign::Fillet::ControlPointComponent component) {
            return fillet->ensureRadiusControlPointValue(
                name,
                id,
                component,
                component == PartDesign::Fillet::ControlPointComponent::Radius ? r : t
            );
        };
        const bool endpoint = id == "start" || id == "end";
        const bool radiusBound = bool(
            fillet->getExpression(path(PartDesign::Fillet::ControlPointComponent::Radius)).expression
        );
        bool positionBound = false;
        bool lengthBound = false;
        if (!endpoint) {
            lengthBound = bool(fillet
                                   ->getExpression(fillet->VariableRadiusControlPointValues
                                                       .getItemPath(name + "|" + id + "|length"))
                                   .expression);
            positionBound
                = bool(fillet
                           ->getExpression(path(PartDesign::Fillet::ControlPointComponent::Position))
                           .expression)
                || lengthBound;
        }
        points.push_back(
            {id, t, r, fillet->isRadiusControlPointAbsolute(name, id) || lengthBound, positionBound, radiusBound}
        );
    };
    append("start", 0, found->second.start);
    for (const auto& p : found->second.controlPoints) {
        append(p.id, p.position, p.radius);
    }
    append("end", 1, found->second.end);
    if (std::ranges::find(points, activePoint, &FilletEditorPoint::id) == points.end()) {
        activePoint = "start";
    }
    const auto& active = *std::ranges::find(points, activePoint, &FilletEditorPoint::id);
    std::vector<QPointF> curve;
    const auto& profiles = fillet->getRadiusProfiles();
    const auto& refs = fillet->Base.getSubValues();
    const auto edgeIndex = std::ranges::find(refs, name) - refs.begin();
    if (size_t(edgeIndex) < profiles.size()) {
        for (const auto& sample : profiles[edgeIndex]) {
            curve.emplace_back(sample.position, sample.radius);
        }
    }
    const auto path = fillet->ensureRadiusControlPointValue(
        name,
        activePoint,
        PartDesign::Fillet::ControlPointComponent::Radius,
        active.radius
    );
    for (auto* editor : {pointEditor, inlineEditor.data()}) {
        if (!editor) {
            continue;
        }
        editor->setPoints(points, activePoint, currentEdgeLength().value_or(0));
        editor->background = pointEditor->panelBackground();
        editor->setRadiusCurve(curve);
        editor->bindRadius(path);
        editor->setHistoryEnabled(!editUndo.empty(), !editRedo.empty());
    }
    refreshingEditor = false;
    updateInlinePlacement();
}

void TaskFilletParameters::updateInlinePlacement()
try {
    if (!inlineEditor) {
        return;
    }
    auto* vp = getDressUpView();
    auto* view = vp ? dynamic_cast<Gui::View3DInventor*>(vp->getDocument()->getActiveView())
                    : nullptr;
    auto edge = currentEdgeShape();
    if (!view || !edge || !isVariableRadius() || addingControlPoint) {
        inlineEditor->hide();
        return;
    }
    auto* current = ui->listWidgetReferences->currentItem();
    const auto it = edgeRadii.find(current->text().toStdString());
    if (it == edgeRadii.end()) {
        return;
    }
    double position = activePoint == "end" ? 1 : 0;
    auto point = std::ranges::find(it->second.controlPoints, activePoint, &ControlPoint::id);
    if (point != it->second.controlPoints.end()) {
        position = point->position;
    }
    const auto frame = evaluateEdgePosition(TopoDS::Edge(edge->getShape()), position);
    if (!frame) {
        return;
    }
    auto* viewer = view->getViewer();
    auto* camera = viewer->getSoRenderManager()->getCamera();
    if (!camera) {
        return;
    }
    auto* parent = inlineEditor->parentWidget();
    if (parent != viewer->getGLWidget()) {
        inlineEditor->hide();
        return;
    }
    SbVec3f screen;
    Base::Vector3d world = frame->position;
    // Match the editing-root transform used by the Coin gizmos.
    world = vp->getDocument()->getEditingTransform() * world;
    camera->getViewVolume(float(parent->width()) / std::max(1, parent->height()))
        .projectToScreen(SbVec3f(world.x, world.y, world.z), screen);
    inlineEditor->adjustSize();
    const QPoint desired(
        int(screen[0] * parent->width()) + 20,
        int((1 - screen[1]) * parent->height()) + 35
    );
    const QPoint target = desired + inlineEditor->userOffset;
    inlineEditor->move(
        std::clamp(target.x(), 0, std::max(0, parent->width() - inlineEditor->width())),
        std::clamp(target.y(), 0, std::max(0, parent->height() - inlineEditor->height()))
    );
    inlineEditor->show();
    inlineEditor->raise();
    inlineEditor->setAnchor(QPointF(screen[0] * parent->width(), (1 - screen[1]) * parent->height()));
}
catch (const Base::Exception&) {
    if (inlineEditor) {
        inlineEditor->hide();
    }
}
catch (const Standard_Failure&) {
    if (inlineEditor) {
        inlineEditor->hide();
    }
}

void TaskFilletParameters::selectPoint(const std::string& id)
{
    activePoint = id;
    for (int i = 0; i < ui->treeWidgetReferences->topLevelItemCount(); ++i) {
        auto* edge = ui->treeWidgetReferences->topLevelItem(i);
        if (!ui->listWidgetReferences->currentItem()
            || edge->text(0) != ui->listWidgetReferences->currentItem()->text()) {
            continue;
        }
        for (int j = 0; j < edge->childCount(); ++j) {
            auto* child = edge->child(j);
            if (child->data(0, Qt::UserRole).toString().toStdString() == id) {
                ui->treeWidgetReferences->setCurrentItem(child, 0, QItemSelectionModel::ClearAndSelect);
            }
        }
    }
    refreshPointEditor();
}

TaskFilletParameters::EditState TaskFilletParameters::captureEdit() const
{
    auto* fillet = getObject<PartDesign::Fillet>();
    return {
        fillet->VariableRadiusData.getValues(),
        fillet->VariableRadiusControlPointIds.getValues(),
        fillet->VariableRadiusControlPointValues.getValues(),
        std::shared_ptr<App::Property>(fillet->ExpressionEngine.Copy())
    };
}
void TaskFilletParameters::beginPointEdit()
{
    if (!pendingEdit) {
        setupTransaction();
        pendingEdit = captureEdit();
    }
}
void TaskFilletParameters::finishPointEdit()
{
    if (pendingEdit) {
        editUndo.push_back(std::move(*pendingEdit));
        pendingEdit.reset();
        editRedo.clear();
    }
    refreshPointEditor();
}
void TaskFilletParameters::restoreEdit(const EditState& state)
{
    auto* fillet = getObject<PartDesign::Fillet>();
    fillet->VariableRadiusData.setValues(state.laws);
    fillet->VariableRadiusControlPointIds.setValues(state.ids);
    fillet->VariableRadiusControlPointValues.setValues(state.values);
    fillet->ExpressionEngine.Paste(*state.expressions);
    refreshControlPointValuesFromModel();
    rebuildAllGizmos();
    updatePreview();
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
    pointEditor->showError(message);
    if (inlineEditor) {
        inlineEditor->showError(message);
    }
    if (fillet->isError() && !fillet->Shape.getValue().IsNull()) {
        getDressUpView()->showPreviousFeature(false);
        getDressUpView()->show();
    }
    refreshPointEditor();
    setGizmoPositions();
}
void TaskFilletParameters::editPoint(const std::string& id, double position, double radius, bool absolute)
{
    auto* fillet = getObject<PartDesign::Fillet>();
    auto* item = ui->listWidgetReferences->currentItem();
    if (!fillet || !item) {
        return;
    }
    const std::string name = item->text().toStdString();
    auto& radii = edgeRadii[name];
    if (!(radius > 0) || !std::isfinite(radius)) {
        pointEditor->showError(tr("Radius must be a positive finite length."));
        if (inlineEditor) {
            inlineEditor->showError(tr("Radius must be a positive finite length."));
        }
        return;
    }
    if (id != "start" && id != "end") {
        if (position <= 0 || position >= 1 || !std::isfinite(position)
            || std::ranges::any_of(radii.controlPoints, [&](const auto& p) {
                   return p.id != id && std::abs(p.position - position) < controlPointTolerance;
               })) {
            pointEditor->showError(tr("Points must have distinct positions inside the edge."));
            return;
        }
    }
    const bool ownsEdit = !pendingEdit;
    beginPointEdit();
    activePoint = id;
    if (id == "start") {
        radii.start = radius;
    }
    else if (id == "end") {
        radii.end = radius;
    }
    else {
        auto point = std::ranges::find(radii.controlPoints, id, &ControlPoint::id);
        if (point == radii.controlPoints.end()) {
            pendingEdit.reset();
            return;
        }
        point->position = position;
        point->radius = radius;
        fillet->setRadiusControlPointPosition(name, id, position, currentEdgeLength().value_or(0), absolute);
        std::ranges::sort(radii.controlPoints, {}, &ControlPoint::position);
    }
    fillet->setRadiusControlPointValue(name, id, PartDesign::Fillet::ControlPointComponent::Radius, radius);
    syncRadiusLaw(name);
    refreshControlPointValuesFromModel();
    setGizmoPositions();
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
std::vector<std::string> TaskFilletParameters::selectedPointIds() const
{
    std::vector<std::string> result;
    for (auto* item : ui->treeWidgetReferences->selectedItems()) {
        const auto* current = ui->listWidgetReferences->currentItem();
        if (item->parent() && current && item->parent()->text(0) == current->text()) {
            result.push_back(item->data(0, Qt::UserRole).toString().toStdString());
        }
    }
    return result;
}
void TaskFilletParameters::pointAction(const std::string& action)
{
    if (action == "cancel-gesture") {
        if (pendingEdit) {
            auto state = std::move(*pendingEdit);
            pendingEdit.reset();
            restoreEdit(state);
        }
        return;
    }
    if (action == "undo" || action == "redo") {
        auto& from = action == "undo" ? editUndo : editRedo;
        auto& to = action == "undo" ? editRedo : editUndo;
        if (from.empty()) {
            return;
        }
        to.push_back(captureEdit());
        auto state = std::move(from.back());
        from.pop_back();
        pendingEdit.reset();
        restoreEdit(state);
        return;
    }
    if (action == "add") {
        setAddControlPointMode(!addingControlPoint);
        return;
    }
    if (action == "expression") {
        refreshControlPointValuesFromModel();
        updatePreview();
        return;
    }
    auto* item = ui->listWidgetReferences->currentItem();
    auto* fillet = getObject<PartDesign::Fillet>();
    if (!item || !fillet) {
        return;
    }
    const auto name = item->text().toStdString();
    auto& radii = edgeRadii[name];
    if (action == "position-expression") {
        const auto point = std::ranges::find(radii.controlPoints, activePoint, &ControlPoint::id);
        if (point == radii.controlPoints.end()) {
            return;
        }
        using Component = PartDesign::Fillet::ControlPointComponent;
        const auto lengthPath = fillet->VariableRadiusControlPointValues.getItemPath(
            name + "|" + activePoint + "|length"
        );
        const bool absolute = fillet->isRadiusControlPointAbsolute(name, activePoint)
            || bool(fillet->getExpression(lengthPath).expression);
        const auto path = fillet->ensureRadiusControlPointValue(
            name,
            activePoint,
            absolute ? Component::Length : Component::Position,
            point->position * (absolute ? currentEdgeLength().value_or(0) : 1)
        );
        auto* dialog = new Gui::Dialog::DlgExpressionInput(
            path,
            fillet->getExpression(path).expression,
            absolute ? Base::Unit::Length : Base::Unit(),
            pointEditor
        );
        connect(dialog, &QDialog::finished, this, [this, dialog, path, fillet](int result) {
            if (result == QDialog::Accepted || dialog->discardedFormula()) {
                beginPointEdit();
                fillet->setExpression(
                    path,
                    dialog->discardedFormula() ? nullptr : dialog->getExpression()
                );
                updatePreview();
                refreshControlPointValuesFromModel();
                finishPointEdit();
            }
            dialog->deleteLater();
        });
        dialog->show();
        return;
    }
    if (action == "next") {
        std::vector<std::string> ids {"start"};
        for (const auto& p : radii.controlPoints) {
            ids.push_back(p.id);
        }
        ids.push_back("end");
        auto it = std::ranges::find(ids, activePoint);
        selectPoint(it == ids.end() || ++it == ids.end() ? ids.front() : *it);
        return;
    }
    const auto selected = selectedPointIds();
    // Operations that discard or remap a law must not silently destroy expressions.
    if (action == "swap" || action == "constant") {
        for (const auto& [key, value] : fillet->VariableRadiusControlPointValues.getValues()) {
            if (
                key.starts_with(name + "|")
                && fillet->getExpression(fillet->VariableRadiusControlPointValues.getItemPath(key)).expression
            ) {
                pointEditor->showError(
                    tr("Unlink this edge's expressions before replacing its radius distribution.")
                );
                return;
            }
        }
    }
    beginPointEdit();
    if (action == "remove") {
        std::erase_if(radii.controlPoints, [&](const auto& p) {
            return std::ranges::find(selected, p.id) != selected.end();
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
    else if (action == "swap") {
        std::swap(radii.start, radii.end);
        for (auto& p : radii.controlPoints) {
            p.position = 1 - p.position;
            fillet->setRadiusControlPointPosition(
                name,
                p.id,
                p.position,
                currentEdgeLength().value_or(0),
                fillet->isRadiusControlPointAbsolute(name, p.id)
            );
        }
        std::ranges::sort(radii.controlPoints, {}, &ControlPoint::position);
        fillet->setRadiusControlPointValue(
            name,
            "start",
            PartDesign::Fillet::ControlPointComponent::Radius,
            radii.start
        );
        fillet->setRadiusControlPointValue(
            name,
            "end",
            PartDesign::Fillet::ControlPointComponent::Radius,
            radii.end
        );
    }
    syncRadiusLaw(name);
    rebuildAllGizmos();
    finishPointEdit();
}
bool TaskFilletParameters::eventFilter(QObject* watched, QEvent* event)
{
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
    Content.push_back(preview);
}

TaskDlgFilletParameters::~TaskDlgFilletParameters() = default;

//==== calls from the TaskView ===============================================================

bool TaskDlgFilletParameters::accept()
{
    if (!static_cast<TaskFilletParameters*>(parameter)->commitPointInput()) {
        return false;
    }
    auto obj = getObject();
    if (!obj->isError()) {
        getViewObject()->showPreviousFeature(false);
    }

    parameter->apply();

    return TaskDlgDressUpParameters::accept();
}

bool TaskFilletParameters::commitPointInput()
{
    return pointEditor->commitPendingInput() && (!inlineEditor || inlineEditor->commitPendingInput());
}

#include "moc_TaskFilletParameters.cpp"
