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

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include <BRepAlgo.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Circle.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>

#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Mod/Part/App/TopoShape.h>

#include "FeatureFillet.h"


using namespace PartDesign;


PROPERTY_SOURCE(PartDesign::Fillet, PartDesign::DressUp)

const App::PropertyQuantityConstraint::Constraints floatRadius
    = {0.0, std::numeric_limits<float>::max(), 0.1};

const char* radiusModeEnums[] = {"Constant Radius", "Variable Radius", nullptr};

Fillet::Fillet()
{
    ADD_PROPERTY_TYPE(RadiusMode, (0L), "Fillet", App::Prop_None, "Fillet radius mode.");
    RadiusMode.setEnums(radiusModeEnums);

    ADD_PROPERTY_TYPE(Radius, (1.0), "Fillet", App::Prop_None, "Fillet radius.");
    Radius.setUnit(Base::Unit::Length);
    Radius.setConstraints(&floatRadius);
    ADD_PROPERTY_TYPE(
        VariableRadiusData,
        (std::map<std::string, std::string>()),
        "Fillet",
        App::Prop_Hidden,
        "Per-edge variable-radius laws encoded as normalized-position/radius pairs."
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

Part::FilletRadiusLaw Fillet::getRadiusLaw(const std::string& edgeName) const
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
    return law;
}

short Fillet::mustExecute() const
{
    if (Placement.isTouched() || RadiusMode.isTouched() || Radius.isTouched()
        || VariableRadiusData.isTouched()) {
        return 1;
    }
    return DressUp::mustExecute();
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

    const bool variableRadius
        = RadiusMode.getValue() == static_cast<int>(RadiusModeValue::Variable);
    std::vector<TopoShape> edges;
    std::vector<Part::FilletRadiusLaw> radiusLaws;

    if (variableRadius) {
        if (UseAllEdges.getValue()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Variable-radius fillets require explicit edges")
            );
        }

        const auto& storedRefs = Base.getSubValues();
        const auto resolvedRefs = Base.getSubValues(true);
        if (storedRefs.size() != resolvedRefs.size()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Invalid variable-radius edge references")
            );
        }

        const auto& storedLaws = VariableRadiusData.getValues();
        for (std::size_t i = 0; i < storedRefs.size(); ++i) {
            auto edge = baseShape.getSubTopoShape(resolvedRefs[i].c_str(), true);
            if (edge.isNull() || edge.shapeType() != TopAbs_EDGE) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP(
                        "Exception",
                        "Variable-radius fillets require explicit edge selections"
                    )
                );
            }

            Part::FilletRadiusLaw law;
            if (storedLaws.contains(storedRefs[i])) {
                law = getRadiusLaw(storedRefs[i]);
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

        // Add signal handler for segfault protection
#if defined(__GNUC__) && defined(FC_OS_LINUX)
        Base::SignalException se;
#endif

        if (variableRadius) {
            shape.makeElementFillet(baseShape, edges, radiusLaws);
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
