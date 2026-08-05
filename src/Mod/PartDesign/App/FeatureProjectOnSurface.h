// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/App/FeatureProjectOnSurface.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

namespace PartDesign
{

/** Helper geometry that projects planar source geometry onto one or more faces.
 *
 * Unlike the Part workbench feature, the projection direction is derived from
 * each source plane. The feature is intentionally not a PartDesign::Feature:
 * like ShapeBinder, it may contain wires or faces and must not become the
 * solid tip of a Body.
 */
class PartDesignExport ProjectOnSurface: public Part::ProjectOnSurface
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::ProjectOnSurface);

public:
    ProjectOnSurface();

    const char* getViewProviderName() const override;
};

}  // namespace PartDesign
