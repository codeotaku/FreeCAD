// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FeatureProjectOnSurface.h"

using namespace PartDesign;

PROPERTY_SOURCE(PartDesign::ProjectOnSurface, Part::ProjectOnSurface)

ProjectOnSurface::ProjectOnSurface()
{
    Projection.setScope(App::LinkScope::Global);
    SupportFace.setScope(App::LinkScope::Global);
    SupportFaces.setScope(App::LinkScope::Global);
    AutoDirection.setValue(true);
    Mode.setValue(Part::ProjectOnSurface::EdgesMode);

    Direction.setStatus(App::Property::Hidden, true);
    SupportFace.setStatus(App::Property::Hidden, true);
    SupportFaces.setStatus(App::Property::Hidden, false);
    AutoDirection.setStatus(App::Property::Hidden, false);
    AutoDirection.setStatus(App::Property::ReadOnly, true);
}

const char* ProjectOnSurface::getViewProviderName() const
{
    return "PartDesignGui::ViewProviderProjectOnSurface";
}
