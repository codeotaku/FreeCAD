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


#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#include <Gui/Inventor/Draggers/Gizmo.h>

#include "TaskDressUpParameters.h"
#include "ViewProviderFillet.h"

class Ui_TaskFilletParameters;
class QComboBox;
class QToolButton;
class QLabel;

namespace Gui
{
class LinearGizmo;
class GizmoContainer;
}  // namespace Gui

namespace PartDesignGui
{

class EdgePositionGizmo;

class TaskFilletParameters: public TaskDressUpParameters
{
    Q_OBJECT

public:
    explicit TaskFilletParameters(ViewProviderDressUp* DressUpView, QWidget* parent = nullptr);
    ~TaskFilletParameters() override;

    void apply() override;
    Gui::TaskView::TaskBox* advancedBox = nullptr;

private Q_SLOTS:
    void onStartRadiusChanged(double value);
    void onEndRadiusChanged(double value);
    void onFilletTypeChanged(int index);
    void onCurrentEdgeChanged(QListWidgetItem* current, QListWidgetItem* previous);
    void onRefDeleted() override;
    void onAddAllEdges();
    void onCheckBoxUseAllEdgesToggled(bool checked);
    void onDefaultRadiusChanged(double value);

protected:
    void setButtons(const selectionModes mode) override;
    void changeEvent(QEvent* e) override;
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ControlPoint
    {
        double position;
        double radius;
        std::string id;
    };

    struct EdgeRadii
    {
        double start;
        double end;
        std::vector<ControlPoint> controlPoints;
    };

    struct ControlPointGizmoSet
    {
        std::string edgeName;
        std::string pointId;
        Gui::LinearGizmo* radius = nullptr;
        EdgePositionGizmo* position = nullptr;
        Gui::QuantitySpinBox* radiusEditor = nullptr;
    };

    std::unique_ptr<Ui_TaskFilletParameters> ui;
    std::unordered_map<std::string, EdgeRadii> edgeRadii;
    bool geometryTreeDirty = true;
    double defaultRadius = 1.0;

    std::unique_ptr<Gui::GizmoContainer> gizmoContainer;
    Gui::LinearGizmo* radiusGizmo = nullptr;
    Gui::LinearGizmo* radiusGizmo2 = nullptr;
    EdgePositionGizmo* startPointGizmo = nullptr;
    EdgePositionGizmo* endPointGizmo = nullptr;
    std::vector<ControlPointGizmoSet> controlPointGizmos;
    fastsignals::scoped_connection filletChangedConnection;
    bool addingControlPoint = false;
    bool controlPointRefreshQueued = false;
    std::string activePoint = "start";
    bool pointEditActive = false;
    void setupTaskPanel();
    void refreshReferences();
    std::vector<std::pair<std::string, Part::TopoShape>> radiusEdges() const;
    bool hasPositionExpression(const std::string& edge, const std::string& id) const;
    std::optional<Part::TopoShape> edgeShape(const std::string& name) const;
    void refreshPointTable();
    void changeRadiusLaw(int index);
    bool currentLawIsVariable() const;
    QComboBox* radiusLaw = nullptr;
    QComboBox* positionUnits = nullptr;
    QToolButton* removePointButton = nullptr;
    QLabel* errorLabel = nullptr;
    void selectPoint(const std::string& id);
    void updatePointHighlight();
    void editRadius(const std::string& edge, const std::string& id, double radius);
    void editPoint(
        const std::string& edge,
        const std::string& id,
        double position,
        double radius,
        bool absolute
    );
    void insertPoint(double position);
    void pointAction(const std::string& action);
    void beginPointEdit();
    void finishPointEdit();
    void updatePreview();

    void clearGizmos();
    void rebuildAllGizmos();
    void rebuildGizmos();
    void refreshEdgeTree();
    void refreshControlPointValuesFromModel();
    void activateEdge(const std::string& edgeName);
    void selectEdgeTreeItem(const QString& edgeName);
    void syncEdgeTreeSelection();
    void setGizmoPositions();
    void setRadiusControlsEnabled(bool enabled);
    void updateFilletTypeUi();
    bool isVariableRadius() const;
    void ensureCurrentEdge();
    void setAddControlPointMode(bool enabled);
    bool addControlPointFromSelection(const Gui::SelectionChanges& msg);
    std::optional<Part::TopoShape> currentEdgeShape() const;
    std::optional<double> currentEdgeLength() const;
    std::optional<double> edgeLength(const std::string& edgeName) const;
    void syncRadiusLaw(const std::string& edgeName);
};

/// simulation dialog for the TaskView
class TaskDlgFilletParameters: public TaskDlgDressUpParameters
{
    Q_OBJECT

public:
    explicit TaskDlgFilletParameters(ViewProviderFillet* DressUpView);
    ~TaskDlgFilletParameters() override;

public:
    /// is called by the framework if the dialog is accepted (Ok)
    bool accept() override;
};

}  // namespace PartDesignGui
