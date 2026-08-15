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


#pragma once

#include <string>
#include <vector>

#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/ObjectIdentifier.h>

#include "FeatureDressUp.h"

namespace PartDesign
{

class PartDesignExport Fillet: public DressUp
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::Fillet);

public:
    Fillet();

    enum class RadiusModeValue
    {
        Constant,
        Variable
    };

    App::PropertyEnumeration RadiusMode;
    App::PropertyQuantityConstraint Radius;
    App::PropertyMap VariableRadiusData;
    App::PropertyMap VariableRadiusControlPointIds;
    App::PropertyMap VariableRadiusControlPointValues;
    App::PropertyBool UseAllEdges;

    /** Persist a variable-radius law for a Base edge reference.
     *
     * Positions are relative parameters on the OCCT fillet spine in the range [0, 1]. The law is
     * stored in VariableRadiusData so it survives document save/restore. Geometry validation is
     * performed by TopoShape::makeElementFillet() when the feature executes.
     */
    void setRadiusLaw(const std::string& edgeName, const Part::FilletRadiusLaw& law);

    /** Return the persisted law for an edge, or an empty law if no valid data is stored. */
    Part::FilletRadiusLaw getRadiusLaw(const std::string& edgeName) const;

    enum class ControlPointComponent
    {
        Position,
        Radius
    };

    std::vector<std::string> getRadiusControlPointIds(const std::string& edgeName) const;
    void setRadiusControlPointIds(
        const std::string& edgeName,
        const std::vector<std::string>& ids
    );
    std::string newRadiusControlPointId(const std::string& edgeName) const;
    App::ObjectIdentifier ensureRadiusControlPointValue(
        const std::string& edgeName,
        const std::string& id,
        ControlPointComponent component,
        double value
    );
    void setRadiusControlPointValue(
        const std::string& edgeName,
        const std::string& id,
        ControlPointComponent component,
        double value
    );
    void clearRadiusControlPoints(const std::string& edgeName);

    /** @name methods override feature */
    //@{
    /// recalculate the feature
    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    /// returns the type name of the view provider
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderFillet";
    }
    //@}

protected:
    void Restore(Base::XMLReader& reader) override;
    void handleChangedPropertyType(
        Base::XMLReader& reader,
        const char* TypeName,
        App::Property* prop
    ) override;
};

}  // namespace PartDesign
