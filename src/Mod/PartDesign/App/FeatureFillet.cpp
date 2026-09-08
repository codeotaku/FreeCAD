// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

#include <BRepAlgo.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Circle.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>

#include <Base/Exception.h>
#include <Base/Quantity.h>
#include <Base/Reader.h>
#include <App/ExpressionParser.h>
#include <Mod/Part/App/TopoShape.h>

#include "FeatureFillet.h"


using namespace PartDesign;


PROPERTY_SOURCE(PartDesign::Fillet, PartDesign::DressUp)

const App::PropertyQuantityConstraint::Constraints floatRadius
    = {0.0, std::numeric_limits<float>::max(), 0.1};

const char* radiusModeEnums[] = {"Constant Radius", "Variable Radius", nullptr};

namespace
{
std::string controlPointValueKey(
    const std::string& edgeName,
    const std::string& id,
    PartDesign::Fillet::ControlPointComponent component
)
{
    const char* suffix = nullptr;
    switch (component) {
        case PartDesign::Fillet::ControlPointComponent::Position:
            suffix = "|position";
            break;
        case PartDesign::Fillet::ControlPointComponent::Length:
            suffix = "|length";
            break;
        case PartDesign::Fillet::ControlPointComponent::Radius:
            suffix = "|radius";
            break;
    }
    return edgeName + '|' + id + suffix;
}

std::string serializeNumber(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

std::optional<double> controlPointValue(
    const Base::Quantity& quantity,
    PartDesign::Fillet::ControlPointComponent component
)
{
    if (component == PartDesign::Fillet::ControlPointComponent::Position) {
        if (!quantity.isDimensionless()) {
            return std::nullopt;
        }
    }
    else if (!quantity.isDimensionless() && quantity.getUnit() != Base::Unit::Length) {
        return std::nullopt;
    }
    return quantity.getValue();
}

std::optional<double> parseControlPointValue(
    const std::string& text,
    PartDesign::Fillet::ControlPointComponent component
)
{
    try {
        return controlPointValue(Base::Quantity::parse(text), component);
    }
    catch (const Base::Exception&) {
        return std::nullopt;
    }
}

std::optional<double> resolveControlPointValue(
    const PartDesign::Fillet& fillet,
    const std::string& key,
    const std::string& stored,
    PartDesign::Fillet::ControlPointComponent component
)
{
    const auto path = fillet.VariableRadiusControlPointValues.getItemPath(key);
    const auto expression = fillet.getExpression(path).expression;
    if (!expression) {
        return parseControlPointValue(stored, component);
    }
    try {
        const auto evaluated = expression->eval();
        const auto* number = freecad_cast<App::NumberExpression*>(evaluated.get());
        return number ? controlPointValue(number->getQuantity(), component) : std::nullopt;
    }
    catch (const Base::Exception&) {
        return std::nullopt;
    }
}
}  // namespace

Fillet::Fillet()
{
    ADD_PROPERTY_TYPE(RadiusMode, (0L), "Fillet", App::Prop_None, "Fillet radius mode.");
    RadiusMode.setEnums(radiusModeEnums);

    ADD_PROPERTY_TYPE(Radius, (1.0), "Fillet", App::Prop_None, "Fillet radius.");
    Radius.setUnit(Base::Unit::Length);
    Radius.setConstraints(&floatRadius);
    ADD_PROPERTY_TYPE(
        RadiusLawModes,
        (std::map<std::string, std::string>()),
        "Fillet",
        App::Prop_Hidden,
        "Per-edge radius law editor: Constant or Variable."
    );
    ADD_PROPERTY_TYPE(
        VariableRadiusData,
        (std::map<std::string, std::string>()),
        "Fillet",
        App::Prop_Hidden,
        "Per-edge variable-radius laws encoded as normalized-position/radius pairs."
    );
    ADD_PROPERTY_TYPE(
        VariableRadiusControlPointIds,
        (std::map<std::string, std::string>()),
        "Fillet",
        App::Prop_Hidden,
        "Stable identifiers for variable-radius control points."
    );
    ADD_PROPERTY_TYPE(
        VariableRadiusControlPointValues,
        (std::map<std::string, std::string>()),
        "Fillet",
        App::Prop_Hidden,
        "Expression-bindable variable-radius control-point values."
    );
    ADD_PROPERTY_TYPE(
        UseAllEdges,
        (false),
        "Fillet",
        App::Prop_None,
        "Fillet all edges if true, else use only those edges in Base property.\n"
        "If true, then this overrides any edge changes made to the Base property or in the "
        "dialog.\n"
    );
}

void Fillet::setRadiusLaw(const std::string& edgeName, const Part::FilletRadiusLaw& law)
{
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (std::size_t i = 0; i < law.size(); ++i) {
        if (i != 0) {
            stream << ';';
        }
        stream << law[i].position << ',' << law[i].radius;
    }
    VariableRadiusData.setValue(edgeName, stream.str());
}

Part::FilletRadiusLaw Fillet::getRadiusLaw(
    const std::string& edgeName,
    std::optional<double> edgeLength
) const
{
    Part::FilletRadiusLaw law;
    std::istringstream stream(VariableRadiusData.getValue(edgeName));
    std::string point;
    while (std::getline(stream, point, ';')) {
        const auto separator = point.find(',');
        if (separator == std::string::npos || point.find(',', separator + 1) != std::string::npos) {
            return {};
        }
        try {
            const std::string positionText = point.substr(0, separator);
            const std::string radiusText = point.substr(separator + 1);
            std::size_t positionLength = 0;
            std::size_t radiusLength = 0;
            const double position = std::stod(positionText, &positionLength);
            const double radius = std::stod(radiusText, &radiusLength);
            if (positionLength != positionText.size() || radiusLength != radiusText.size()
                || !std::isfinite(position) || !std::isfinite(radius)) {
                return {};
            }
            law.push_back({position, radius});
        }
        catch (const std::exception&) {
            return {};
        }
    }
    const auto& values = VariableRadiusControlPointValues.getValues();
    const auto applyRadius = [this, &values, &edgeName](const std::string& id, double& radius) {
        const auto value = values.find(
            controlPointValueKey(edgeName, id, ControlPointComponent::Radius)
        );
        if (value == values.end()) {
            return true;
        }
        const auto resolved = resolveControlPointValue(
            *this,
            value->first,
            value->second,
            ControlPointComponent::Radius
        );
        if (!resolved) {
            return false;
        }
        radius = *resolved;
        return true;
    };
    if (law.size() >= 2
        && (!applyRadius("start", law.front().radius) || !applyRadius("end", law.back().radius))) {
        return {};
    }

    const auto ids = getRadiusControlPointIds(edgeName);
    if (ids.size() == (law.size() >= 2 ? law.size() - 2 : 0)) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto position = values.find(
                controlPointValueKey(edgeName, ids[i], ControlPointComponent::Position)
            );
            const auto length = values.find(
                controlPointValueKey(edgeName, ids[i], ControlPointComponent::Length)
            );
            const auto radius = values.find(
                controlPointValueKey(edgeName, ids[i], ControlPointComponent::Radius)
            );
            const bool hasLengthExpression = length != values.end()
                && getExpression(VariableRadiusControlPointValues.getItemPath(length->first)).expression;
            if (hasLengthExpression || isRadiusControlPointAbsolute(edgeName, ids[i])) {
                if (length == values.end()) {
                    return {};
                }
                const auto parsed = resolveControlPointValue(
                    *this,
                    length->first,
                    length->second,
                    ControlPointComponent::Length
                );
                if (!parsed || !edgeLength || *edgeLength <= Precision::Confusion()) {
                    return {};
                }
                law[i + 1].position = *parsed / *edgeLength;
            }
            else if (position != values.end()) {
                const auto parsed = resolveControlPointValue(
                    *this,
                    position->first,
                    position->second,
                    ControlPointComponent::Position
                );
                if (!parsed) {
                    return {};
                }
                law[i + 1].position = *parsed;
            }
            if (radius != values.end()) {
                const auto parsed = resolveControlPointValue(
                    *this,
                    radius->first,
                    radius->second,
                    ControlPointComponent::Radius
                );
                if (!parsed) {
                    return {};
                }
                law[i + 1].radius = *parsed;
            }
        }
    }
    if (law.size() >= 2 && RadiusLawModes.getValue(edgeName) == "Constant") {
        return {{0, law.front().radius}, {1, law.front().radius}};
    }
    return law;
}

bool Fillet::isVariableRadiusLaw(const std::string& edgeName) const
{
    const auto mode = RadiusLawModes.getValue(edgeName);
    if (!mode.empty()) {
        return mode == "Variable";
    }
    // Documents created before the task-panel law selector have no editor-mode metadata.
    if (!getRadiusControlPointIds(edgeName).empty()) {
        return true;
    }
    const auto law = getRadiusLaw(edgeName);
    return law.size() > 2 || (law.size() == 2 && law.front().radius != law.back().radius);
}

std::vector<std::string> Fillet::getRadiusControlPointIds(const std::string& edgeName) const
{
    std::vector<std::string> ids;
    std::istringstream stream(VariableRadiusControlPointIds.getValue(edgeName));
    std::string id;
    while (std::getline(stream, id, ';')) {
        if (!id.empty()) {
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

void Fillet::setRadiusControlPointIds(const std::string& edgeName, const std::vector<std::string>& ids)
{
    const auto previous = getRadiusControlPointIds(edgeName);
    for (const auto& id : previous) {
        if (std::ranges::find(ids, id) == ids.end()) {
            VariableRadiusControlPointValues.deleteValue(edgeName + '|' + id + "|absolute");
            VariableRadiusControlPointValues.deleteValue(
                controlPointValueKey(edgeName, id, ControlPointComponent::Position)
            );
            VariableRadiusControlPointValues.deleteValue(
                controlPointValueKey(edgeName, id, ControlPointComponent::Length)
            );
            VariableRadiusControlPointValues.deleteValue(
                controlPointValueKey(edgeName, id, ControlPointComponent::Radius)
            );
        }
    }

    std::ostringstream stream;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            stream << ';';
        }
        stream << ids[i];
    }
    VariableRadiusControlPointIds.setValue(edgeName, stream.str());
}

std::string Fillet::newRadiusControlPointId(const std::string& edgeName) const
{
    unsigned long next = 1;
    for (const auto& id : getRadiusControlPointIds(edgeName)) {
        if (!id.starts_with("cp")) {
            continue;
        }
        constexpr unsigned long decimalBase = 10;
        unsigned long value = 0;
        bool valid = id.size() > 2;
        for (std::size_t i = 2; valid && i < id.size(); ++i) {
            const char digit = id[i];
            valid = digit >= '0' && digit <= '9';
            if (valid) {
                value = (value * decimalBase) + static_cast<unsigned long>(digit - '0');
            }
        }
        if (valid) {
            next = std::max(next, value + 1);
        }
    }
    return "cp" + std::to_string(next);
}

App::ObjectIdentifier Fillet::ensureRadiusControlPointValue(
    const std::string& edgeName,
    const std::string& id,
    ControlPointComponent component,
    double value
)
{
    const std::string key = controlPointValueKey(edgeName, id, component);
    if (!VariableRadiusControlPointValues.getValues().contains(key)) {
        VariableRadiusControlPointValues.setValue(key, serializeNumber(value));
    }
    return VariableRadiusControlPointValues.getItemPath(key);
}

void Fillet::setRadiusControlPointValue(
    const std::string& edgeName,
    const std::string& id,
    ControlPointComponent component,
    double value
)
{
    VariableRadiusControlPointValues.setValue(
        controlPointValueKey(edgeName, id, component),
        serializeNumber(value)
    );
}

bool Fillet::isRadiusControlPointAbsolute(const std::string& edgeName, const std::string& id) const
{
    return VariableRadiusControlPointValues.getValue(edgeName + '|' + id + "|absolute") == "1";
}

void Fillet::setRadiusControlPointPosition(
    const std::string& edgeName,
    const std::string& id,
    double position,
    double edgeLength,
    bool absolute
)
{
    setRadiusControlPointValue(edgeName, id, ControlPointComponent::Position, position);
    setRadiusControlPointValue(edgeName, id, ControlPointComponent::Length, position * edgeLength);
    VariableRadiusControlPointValues.setValue(edgeName + '|' + id + "|absolute", absolute ? "1" : "0");
}

void Fillet::clearRadiusControlPoints(const std::string& edgeName)
{
    setRadiusControlPointIds(edgeName, {});
    VariableRadiusControlPointIds.deleteValue(edgeName);
    VariableRadiusControlPointValues.deleteValue(
        controlPointValueKey(edgeName, "start", ControlPointComponent::Radius)
    );
    VariableRadiusControlPointValues.deleteValue(
        controlPointValueKey(edgeName, "end", ControlPointComponent::Radius)
    );
}

short Fillet::mustExecute() const
{
    if (Placement.isTouched() || RadiusMode.isTouched() || Radius.isTouched()
        || VariableRadiusData.isTouched() || RadiusLawModes.isTouched()
        || VariableRadiusControlPointIds.isTouched() || VariableRadiusControlPointValues.isTouched()) {
        return 1;
    }
    return DressUp::mustExecute();
}

std::vector<std::pair<std::string, Part::TopoShape>> Fillet::getRadiusEdges(Part::TopoShape shape) const
{
    if (shape.isNull()) {
        shape = getBaseTopoShape(true);
    }
    const auto& stored = Base.getSubValues();
    const auto resolved = Base.getSubValues(true);
    std::vector<std::pair<std::string, Part::TopoShape>> result;
    if (stored.size() != resolved.size()) {
        throw Base::ValueError("Invalid fillet references");
    }
    const auto append = [&](const std::string& name, const Part::TopoShape& edge) {
        if (std::ranges::none_of(result, [&](const auto& existing) {
                return existing.second.getShape().IsSame(edge.getShape());
            })) {
            result.emplace_back(name, edge);
        }
    };
    // Explicit edges retain their stored (topologically mapped) law keys.
    for (size_t i = 0; i < stored.size(); ++i) {
        auto selected = shape.getSubTopoShape(resolved[i].c_str());
        if (selected.shapeType() == TopAbs_EDGE) {
            append(stored[i], selected);
        }
        else if (selected.shapeType() != TopAbs_FACE) {
            throw Base::ValueError("Fillets require edges or faces");
        }
    }
    for (size_t i = 0; i < stored.size(); ++i) {
        auto selected = shape.getSubTopoShape(resolved[i].c_str());
        if (selected.shapeType() == TopAbs_FACE) {
            for (const auto& edge : selected.getSubTopoShapes(TopAbs_EDGE)) {
                append("Edge" + std::to_string(shape.findShape(edge.getShape())), edge);
            }
        }
    }
    return result;
}

App::DocumentObjectExecReturn* Fillet::execute()
{
    if (onlyHaveRefined()) {
        return App::DocumentObject::StdReturn;
    }


    Part::TopoShape baseShape;
    try {
        baseShape = getBaseTopoShape();
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    baseShape.setTransform(Base::Matrix4D());

    const bool variableRadius = RadiusMode.getValue() == static_cast<int>(RadiusModeValue::Variable);
    std::vector<TopoShape> edges;
    std::vector<Part::FilletRadiusLaw> radiusLaws;

    if (variableRadius) {
        if (UseAllEdges.getValue()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Variable-radius fillets require explicit edges")
            );
        }

        const auto& storedLaws = VariableRadiusData.getValues();
        std::vector<std::pair<std::string, Part::TopoShape>> selectedEdges;
        try {
            selectedEdges = getRadiusEdges(baseShape);
        }
        catch (const Base::Exception& error) {
            return new App::DocumentObjectExecReturn(error.what());
        }
        catch (const Standard_Failure& error) {
            return new App::DocumentObjectExecReturn(error.GetMessageString());
        }
        for (const auto& [name, edge] : selectedEdges) {
            Part::FilletRadiusLaw law;
            if (storedLaws.contains(name)) {
                GProp_GProps edgeProperties;
                BRepGProp::LinearProperties(edge.getShape(), edgeProperties);
                law = getRadiusLaw(name, edgeProperties.Mass());
                if (law.empty()) {
                    return new App::DocumentObjectExecReturn(
                        QT_TRANSLATE_NOOP("Exception", "Invalid stored variable-radius law")
                    );
                }
            }
            else {
                law = {{0.0, Radius.getValue()}, {1.0, Radius.getValue()}};
            }
            edges.push_back(std::move(edge));
            radiusLaws.push_back(std::move(law));
        }
    }
    else {
        edges = UseAllEdges.getValue() ? baseShape.getSubTopoShapes(TopAbs_EDGE)
                                       : getContinuousEdges(baseShape);
        if (Radius.getValue() <= 0) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Fillet radius must be greater than zero")
            );
        }
    }

    if (edges.empty()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "Fillet not possible on selected shapes")
        );
    }

    this->positionByBaseFeature();

    try {
        TopoShape shape(0);  //,getDocument()->getStringHasher());
        std::vector<Part::FilletRadiusLaw> profiles;

        // Add signal handler for segfault protection
#if defined(__GNUC__) && defined(FC_OS_LINUX)
        Base::SignalException se;
#endif

        if (variableRadius) {
            shape.makeElementFillet(baseShape, edges, radiusLaws, nullptr, &profiles);
        }
        else {
            shape.makeElementFillet(baseShape, edges, Radius.getValue(), Radius.getValue());
        }
        if (shape.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Resulting shape is null")
            );
        }

        TopTools_ListOfShape aLarg;
        aLarg.Append(baseShape.getShape());
        if (!BRepAlgo::IsValid(aLarg, shape.getShape(), Standard_False, Standard_False)) {
            ShapeFix_ShapeTolerance aSFT;
            aSFT.LimitTolerance(
                shape.getShape(),
                Precision::Confusion(),
                Precision::Confusion(),
                TopAbs_SHAPE
            );
        }

        // store shape before refinement
        this->rawShape = shape;
        shape = refineShapeIfActive(shape);
        if (!isSingleSolidRuleSatisfied(shape.getShape())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                "Exception",
                "Result has multiple solids: enable 'Allow Compound' in the active body."
            ));
        }

        shape = getSolid(shape);
        this->Shape.setValue(shape);
        radiusProfiles = std::move(profiles);
        return App::DocumentObject::StdReturn;
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (...) {
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
            "Exception",
            "Fillet operation failed. The selected edges may contain geometry that cannot be "
            "filleted together. "
            "Try filleting edges individually or with a smaller radius."
        ));
    }
}

void Fillet::Restore(Base::XMLReader& reader)
{
    DressUp::Restore(reader);
}

void Fillet::handleChangedPropertyType(Base::XMLReader& reader, const char* TypeName, App::Property* prop)
{
    if (prop && strcmp(TypeName, "App::PropertyFloatConstraint") == 0
        && prop->getTypeId().getName() == "App::PropertyQuantityConstraint") {
        App::PropertyFloatConstraint p;
        p.Restore(reader);
        static_cast<App::PropertyQuantityConstraint*>(prop)->setValue(p.getValue());
    }
    else {
        DressUp::handleChangedPropertyType(reader, TypeName, prop);
    }
}
