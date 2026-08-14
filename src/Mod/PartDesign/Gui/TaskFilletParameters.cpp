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
#include <QListWidget>
#include <QMessageBox>
#include <QSignalBlocker>

#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>

#include <Base/Interpreter.h>
#include <Base/Converter.h>
#include <Base/Quantity.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Selection/Selection.h>
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

TaskFilletParameters::TaskFilletParameters(ViewProviderDressUp* DressUpView, QWidget* parent)
    : TaskDressUpParameters(DressUpView, true, true, parent)
    , ui(new Ui_TaskFilletParameters)
{
    // we need a separate container widget to add all controls to
    proxy = new QWidget(this);
    ui->setupUi(proxy);
    this->groupLayout()->addWidget(proxy);

    PartDesign::Fillet* pcFillet = DressUpView->getObject<PartDesign::Fillet>();
    bool useAllEdges = pcFillet->UseAllEdges.getValue();
    ui->checkBoxUseAllEdges->setChecked(useAllEdges);
    ui->buttonRefSel->setEnabled(!useAllEdges);
    ui->listWidgetReferences->setEnabled(!useAllEdges);
    double r = pcFillet->Radius.getValue();
    defaultRadius = r;

    ui->filletRadius->setUnit(Base::Unit::Length);
    ui->filletRadius->setValue(r);
    ui->filletRadius->setMinimum(0);
    ui->filletRadius->selectNumber();

    // UI-only prototype: keep both endpoint radii local until the feature model gains
    // persisted per-edge radius data.
    ui->filletEndRadius->setUnit(Base::Unit::Length);
    ui->filletEndRadius->setValue(r);
    ui->filletEndRadius->setMinimum(0);
    QMetaObject::invokeMethod(ui->filletRadius, "setFocus", Qt::QueuedConnection);
    std::vector<std::string> strings = pcFillet->Base.getSubValues();
    for (const auto& string : strings) {
        ui->listWidgetReferences->addItem(QString::fromStdString(string));
        edgeRadii.emplace(string, EdgeRadii {r, r});
    }

    QMetaObject::connectSlotsByName(this);

    // clang-format off
    connect(ui->filletRadius, qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this, &TaskFilletParameters::onStartRadiusChanged);
    connect(ui->filletEndRadius, qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this, &TaskFilletParameters::onEndRadiusChanged);
    connect(ui->filletType, qOverload<int>(&QComboBox::currentIndexChanged),
        this, &TaskFilletParameters::onFilletTypeChanged);
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
                edgeRadii.try_emplace(subName, EdgeRadii {defaultRadius, defaultRadius});
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
    if (auto fillet = getObject<PartDesign::Fillet>()) {
        if (checked) {
            setSelectionMode(none);
        }

        ui->buttonRefSel->setEnabled(!checked);
        ui->listWidgetReferences->setEnabled(!checked);
        fillet->UseAllEdges.setValue(checked);
        fillet->recomputeFeature();

        if (checked) {
            ui->activeEdgeLabel->setText(tr("Per-edge radii are unavailable when using all edges"));
            setRadiusControlsEnabled(false);
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
    ui->buttonRefSel->setChecked(mode == refSel);
    ui->buttonRefSel->setText(mode == refSel ? stopSelectionLabel() : startSelectionLabel());
}

void TaskFilletParameters::onRefDeleted()
{
    std::vector<std::string> deletedRefs;
    for (const auto* item : ui->listWidgetReferences->selectedItems()) {
        deletedRefs.push_back(item->text().toStdString());
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
        edgeRadii.try_emplace(ref, EdgeRadii {defaultRadius, defaultRadius});
    }
    ensureCurrentEdge();
}

void TaskFilletParameters::onStartRadiusChanged(double value)
{
    auto* item = ui->listWidgetReferences->currentItem();
    if (!item) {
        return;
    }

    auto& radii = edgeRadii
                      .try_emplace(
                          item->text().toStdString(),
                          EdgeRadii {defaultRadius, defaultRadius}
                      )
                      .first->second;
    radii.start = value;
    updateRadiusTooltip(item, radii);
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
                          EdgeRadii {defaultRadius, defaultRadius}
                      )
                      .first->second;
    radii.end = value;
    updateRadiusTooltip(item, radii);
}

void TaskFilletParameters::onFilletTypeChanged([[maybe_unused]] int index)
{
    updateFilletTypeUi();
    ensureCurrentEdge();
}

void TaskFilletParameters::onCurrentEdgeChanged(
    QListWidgetItem* current,
    [[maybe_unused]] QListWidgetItem* previous
)
{
    if (ui->checkBoxUseAllEdges->isChecked()) {
        ui->activeEdgeLabel->setText(tr("Per-edge radii are unavailable when using all edges"));
        setRadiusControlsEnabled(false);
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
        if (gizmoContainer) {
            gizmoContainer->visible = false;
        }
        return;
    }

    auto it = edgeRadii
                  .try_emplace(
                      current->text().toStdString(),
                      EdgeRadii {defaultRadius, defaultRadius}
                  )
                  .first;
    const auto& radii = it->second;
    ui->activeEdgeLabel->setText(
        isVariableRadius() ? tr("Radii for %1").arg(current->text())
                           : tr("Radius for %1").arg(current->text())
    );

    QSignalBlocker startBlocker(ui->filletRadius);
    QSignalBlocker endBlocker(ui->filletEndRadius);
    ui->filletRadius->setValue(radii.start);
    ui->filletEndRadius->setValue(radii.end);

    setRadiusControlsEnabled(true);
    updateRadiusTooltip(current, radii);
    if (radiusGizmo && radiusGizmo2) {
        radiusGizmo->setDragLength(radii.start);
        radiusGizmo2->setDragLength(radii.end);
    }
    setGizmoPositions();
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

void TaskFilletParameters::setupGizmos(ViewProviderDressUp* vp)
{
    if (!GizmoContainer::isEnabled()) {
        return;
    }

    radiusGizmo = new Gui::LinearGizmo(ui->filletRadius);
    radiusGizmo2 = new Gui::LinearGizmo(ui->filletEndRadius);

    gizmoContainer = GizmoContainer::create({radiusGizmo, radiusGizmo2}, vp);

    setGizmoPositions();
    showDraggerHints();
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
        // Attach one radius handle to each endpoint of the active edge.
        TopoDS_Vertex startVertex;
        TopoDS_Vertex endVertex;
        TopExp::Vertices(TopoDS::Edge(edge.getShape()), startVertex, endVertex, true);
        if (startVertex.IsNull() || endVertex.IsNull()) {
            gizmoContainer->visible = false;
            return;
        }

        const gp_Pnt start = BRep_Tool::Pnt(startVertex);
        const gp_Pnt end = BRep_Tool::Pnt(endVertex);
        props1.position = Base::Vector3d(start.X(), start.Y(), start.Z());
        props2.position = Base::Vector3d(end.X(), end.Y(), end.Z());
    }

    radiusGizmo->Gizmo::setDraggerPlacement(props1.position, props1.dir);
    if (isVariableRadius()) {
        radiusGizmo2->Gizmo::setDraggerPlacement(props2.position, props2.dir);
    }

    // The dragger length won't be equal to the radius if the two faces
    // are not orthogonal so this correction is needed
    double angle = props1.dir.GetAngle(props2.dir);
    double correction = 1 / std::tan(angle / 2);

    radiusGizmo->setMultFactor(correction);
    radiusGizmo2->setMultFactor(correction);
}

void TaskFilletParameters::setRadiusControlsEnabled(bool enabled)
{
    ui->startRadiusLabel->setEnabled(enabled);
    ui->endRadiusLabel->setEnabled(enabled && isVariableRadius());
    ui->filletRadius->setEnabled(enabled);
    ui->filletEndRadius->setEnabled(enabled && isVariableRadius());
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
        item->setToolTip(tr("Radius: %1").arg(QString::fromStdString(start.getUserString())));
    }
}

void TaskFilletParameters::updateFilletTypeUi()
{
    const bool variable = isVariableRadius();
    ui->startRadiusLabel->setText(variable ? tr("Start radius") : tr("Radius"));
    ui->endRadiusLabel->setVisible(variable);
    ui->filletEndRadius->setVisible(variable);

    if (radiusGizmo && radiusGizmo2) {
        radiusGizmo->setOriginLabel(variable ? tr("Start").toStdString() : std::string());
        radiusGizmo2->setOriginLabel(variable ? tr("End").toStdString() : std::string());
        radiusGizmo2->setVisibility(variable);
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
