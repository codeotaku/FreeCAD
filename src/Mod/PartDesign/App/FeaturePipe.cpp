// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2015 Stefan Tröger <stefantroeger@gmx.net>              *
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


#include <Mod/Part/App/FCBRepAlgoAPI_Cut.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Fuse.h>
#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <gp_Ax2.hxx>
#include <Law_Function.hxx>
#include <NCollection_List.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_HSequenceOfShape.hxx>


#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Mod/Part/App/FaceMakerCheese.h>

#include "FeaturePipe.h"
#include "Mod/Part/App/TopoShapeOpCode.h"
#include "Mod/Part/App/TopoShapeMapper.h"
#include "FeatureLoft.h"

FC_LOG_LEVEL_INIT("PartDesign", true, true);

using namespace PartDesign;

const char* Pipe::TypeEnums[] = {"FullPath", "UpToFace", nullptr};
const char* Pipe::TransitionEnums[] = {"Transformed", "Right corner", "Round corner", nullptr};
const char* Pipe::ModeEnums[] = {"Standard", "Fixed", "Frenet", "Auxiliary", "Binormal", nullptr};
const char* Pipe::TransformEnums[]
    = {"Constant", "Multisection", "Linear", "S-shape", "Interpolation", nullptr};

namespace
{
// OCCT creates one side face for every (profile edge, spine edge) pair. This structure keeps
// those faces grouped by spine edge for one profile wire, together with the section wire at each
// end of the segment. A pipe with holes has one instance per profile wire.
struct PipeShellSegmentHistory
{
    std::vector<std::vector<Part::TopoShape>> sideFacesBySegment;
    std::vector<Part::TopoShape> sectionWires;

    size_t segmentCount() const
    {
        return sideFacesBySegment.size();
    }
};

// Wrap an OCCT-generated subshape while preserving the source TopoShape's tag and element map
// to preserve topological naming.
Part::TopoShape makeMappedSubShape(const Part::TopoShape& source, const TopoDS_Shape& shape)
{
    Part::TopoShape result(source);
    result.setShape(shape, false);
    return result;
}

Part::TopoShape splitPipePath(const Part::TopoShape& path, int segmentsPerEdge)
{
    BRepBuilderAPI_MakeWire wireMaker;
    BRepTools_WireExplorer spine(TopoDS::Wire(path.getShape()));
    for (; spine.More(); spine.Next()) {
        const TopoDS_Edge& edge = spine.Current();
        BRepAdaptor_Curve adaptor(edge);
        GCPnts_UniformAbscissa split(adaptor, segmentsPerEdge + 1);
        if (!split.IsDone() || split.NbPoints() != segmentsPerEdge + 1) {
            return {};
        }

        double first;
        double last;
        const auto curve = BRep_Tool::Curve(edge, first, last);
        if (curve.IsNull()) {
            return {};
        }

        std::vector<TopoDS_Edge> pieces;
        pieces.reserve(segmentsPerEdge);
        for (int i = 1; i <= segmentsPerEdge; ++i) {
            const double start = i == 1 ? first : split.Parameter(i);
            const double end = i == segmentsPerEdge ? last : split.Parameter(i + 1);
            BRepBuilderAPI_MakeEdge edgeMaker(curve, start, end);
            if (!edgeMaker.IsDone()) {
                return {};
            }
            pieces.push_back(edgeMaker.Edge());
        }

        if (edge.Orientation() == TopAbs_REVERSED) {
            for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
                wireMaker.Add(TopoDS::Edge(it->Reversed()));
            }
        }
        else {
            for (const auto& piece : pieces) {
                wireMaker.Add(piece);
            }
        }
    }

    if (!wireMaker.IsDone()) {
        return {};
    }
    return makeMappedSubShape(path, wireMaker.Wire());
}

Part::TopoShape makeSectionWire(
    const NCollection_List<TopoDS_Shape>& generated,
    const Part::TopoShape& source
)
{
    // OCCT reports the section at a spine vertex as generated subshapes rather than as a wire.
    // Keep only its edges and inherit the completed shell's element map through source, so the
    // reconstructed segment cap retains stable topological names.
    std::vector<Part::TopoShape> edges;
    for (const auto& shape : generated) {
        if (shape.ShapeType() == TopAbs_EDGE) {
            edges.push_back(makeMappedSubShape(source, shape));
        }
    }
    if (edges.empty()) {
        return {};
    }

    // Reassemble the individual section edges into the boundary used to cap a pipe segment.
    // makeElementWires() returns a wire directly for one connected group, but a compound when
    // the input contains disconnected groups.
    Part::TopoShape wire(source);
    wire.makeElementWires(edges);
    if (wire.shapeType() == TopAbs_WIRE) {
        return wire;
    }

    // Each history object represents one profile wire, so exactly one section wire is expected.
    // Accept that wire if it is wrapped in a compound, but reject ambiguous multi-wire results.
    if (wire.countSubShapes(TopAbs_WIRE) == 1) {
        return wire.getSubTopoShape(TopAbs_WIRE, 1);
    }
    return {};
}

bool collectPipeShellSegmentHistory(
    BRepOffsetAPI_MakePipeShell& maker,
    const Part::TopoShape& path,
    const Part::TopoShape& profile,
    const Part::TopoShape& shell,
    PipeShellSegmentHistory& history
)
{
    // OCCT maps each spine edge directly to its generated faces and each spine vertex to the
    // section edges at that junction. Retain that history while the MakePipeShell builder is alive.
    history.sectionWires.push_back(makeMappedSubShape(profile, maker.FirstShape()));
    if (history.sectionWires.back().isNull()) {
        return false;
    }

    BRepTools_WireExplorer spine(TopoDS::Wire(path.getShape()));
    for (; spine.More(); spine.Next()) {
        if (history.segmentCount()) {
            Part::TopoShape wire = makeSectionWire(maker.Generated(spine.CurrentVertex()), shell);
            if (wire.isNull()) {
                return false;
            }
            history.sectionWires.push_back(wire);
        }

        std::vector<Part::TopoShape> faces;
        const auto& generated = maker.Generated(spine.Current());
        for (const auto& shape : generated) {
            if (shape.ShapeType() == TopAbs_FACE) {
                faces.push_back(makeMappedSubShape(shell, shape));
            }
        }
        if (faces.empty()) {
            return false;
        }
        history.sideFacesBySegment.push_back(std::move(faces));
    }

    if (history.segmentCount() < 2) {
        return false;
    }

    history.sectionWires.push_back(makeMappedSubShape(profile, maker.LastShape()));
    return !history.sectionWires.back().isNull();
}

bool hasSelfIntersections(const TopoDS_Shape& shape)
{
    BOPAlgo_ArgumentAnalyzer checker;
    checker.SetShape1(BRepBuilderAPI_Copy(shape).Shape());
    checker.StopOnFirstFaulty() = true;
    checker.SelfInterMode() = true;
    checker.Perform();
    return checker.HasFaulty();
}

Part::TopoShape makePipeSegmentSolid(
    const std::vector<PipeShellSegmentHistory>& histories,
    size_t segmentIndex,
    App::StringHasherRef hasher
)
{
    std::vector<Part::TopoShape> sideFaces;
    std::vector<Part::TopoShape> startWires;
    std::vector<Part::TopoShape> endWires;
    for (const auto& history : histories) {
        const auto& faces = history.sideFacesBySegment[segmentIndex];
        sideFaces.insert(sideFaces.end(), faces.begin(), faces.end());
        startWires.push_back(history.sectionWires[segmentIndex]);
        endWires.push_back(history.sectionWires[segmentIndex + 1]);
    }

    // Cap and sew this segment independently. Unlike the complete self-crossing sweep, the
    // resulting shell bounds an unambiguous solid that OCCT's Boolean operations can consume.
    Part::TopoShape startFace(0, hasher);
    Part::TopoShape endFace(0, hasher);
    startFace.makeElementFace(startWires);
    endFace.makeElementFace(endWires, Part::OpCodes::Sewing);

    BRepBuilderAPI_Sewing sewer;
    sewer.SetTolerance(Precision::Confusion());
    for (const auto& face : sideFaces) {
        sewer.Add(face.getShape());
    }
    sewer.Add(startFace.getShape());
    sewer.Add(endFace.getShape());
    sewer.Perform();

    std::vector<Part::TopoShape> sources = sideFaces;
    sources.push_back(startFace);
    sources.push_back(endFace);
    Part::TopoShape segment(0, hasher);
    segment = segment
                  .makeShapeWithElementMap(
                      sewer.SewedShape(),
                      Part::MapperSewing(sewer),
                      sources,
                      Part::OpCodes::Sewing
                  )
                  .makeElementSolid();

    if (segment.shapeType() != TopAbs_SOLID || !segment.isValid()
        || hasSelfIntersections(segment.getShape())) {
        return {};
    }

    BRepClass3d_SolidClassifier classifier(segment.getShape());
    classifier.PerformInfinitePoint(Precision::Confusion());
    if (classifier.State() == TopAbs_IN) {
        segment.setShape(segment.getShape().Reversed(), false);
    }
    return segment;
}

Part::TopoShape getSingleValidSolid(const Part::TopoShape& shape)
{
    if (shape.shapeType() == TopAbs_SOLID) {
        if (shape.isValid() && !hasSelfIntersections(shape.getShape())) {
            return shape;
        }
        return {};
    }

    auto solids = shape.getSubTopoShapes(TopAbs_SOLID);
    if (solids.size() == 1 && solids.front().isValid()
        && !hasSelfIntersections(solids.front().getShape())) {
        return solids.front();
    }
    return {};
}

Part::TopoShape fusePipeSegments(
    const std::vector<PipeShellSegmentHistory>& histories,
    App::StringHasherRef hasher,
    double fuzzyTolerance
)
{
    try {
        if (histories.empty()) {
            return {};
        }

        const size_t segmentCount = histories.front().segmentCount();
        for (const auto& history : histories) {
            if (history.segmentCount() != segmentCount) {
                return {};
            }
        }

        // The complete shell has overlapping faces and cannot represent the intended tool. Fuse
        // the valid per-spine-edge solids instead; the existing FreeCAD Boolean wrapper then
        // resolves both adjacent and non-adjacent overlaps.
        std::vector<Part::TopoShape> segmentSolids;
        segmentSolids.reserve(segmentCount);
        for (size_t i = 0; i < segmentCount; ++i) {
            Part::TopoShape segment = makePipeSegmentSolid(histories, i, hasher);
            if (segment.isNull()) {
                return {};
            }
            segmentSolids.push_back(segment);
        }

        Part::TopoShape repaired(0, hasher);
        repaired.makeElementFuse(segmentSolids, Part::OpCodes::Fuse, fuzzyTolerance);
        return getSingleValidSolid(repaired);
    }
    catch (const Base::Exception&) {
        return {};
    }
    catch (const Standard_Failure&) {
        return {};
    }
    return {};
}
}  // namespace


PROPERTY_SOURCE(PartDesign::Pipe, PartDesign::ProfileBased)

Pipe::Pipe()
{
    ADD_PROPERTY_TYPE(Sections, (nullptr), "Sweep", App::Prop_None, "List of sections");
    Sections.setValue(nullptr);
    ADD_PROPERTY_TYPE(Spine, (nullptr), "Sweep", App::Prop_None, "Path to sweep along");
    ADD_PROPERTY_TYPE(SpineTangent, (false), "Sweep", App::Prop_None, "Include tangent edges into path");
    ADD_PROPERTY_TYPE(AuxiliarySpine, (nullptr), "Sweep", App::Prop_None, "Secondary path to orient sweep");
    ADD_PROPERTY_TYPE(
        AuxiliarySpineTangent,
        (false),
        "Sweep",
        App::Prop_None,
        "Include tangent edges into secondary path"
    );
    ADD_PROPERTY_TYPE(
        AuxiliaryCurvilinear,
        (true),
        "Sweep",
        App::Prop_None,
        "Calculate normal between equidistant points on both spines"
    );
    ADD_PROPERTY_TYPE(Mode, (long(0)), "Sweep", App::Prop_None, "Profile mode");
    ADD_PROPERTY_TYPE(
        Binormal,
        (Base::Vector3d()),
        "Sweep",
        App::Prop_None,
        "Binormal vector for corresponding orientation mode"
    );
    ADD_PROPERTY_TYPE(Transition, (long(0)), "Sweep", App::Prop_None, "Transition mode");
    ADD_PROPERTY_TYPE(Transformation, (long(0)), "Sweep", App::Prop_None, "Section transformation mode");
    Mode.setEnums(ModeEnums);
    Transition.setEnums(TransitionEnums);
    Transformation.setEnums(TransformEnums);
}

short Pipe::mustExecute() const
{
    if (Sections.isTouched()) {
        return 1;
    }
    if (Spine.isTouched()) {
        return 1;
    }
    if (Mode.isTouched()) {
        return 1;
    }
    if (Transition.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

App::DocumentObjectExecReturn* Pipe::execute()
{
    if (onlyHaveRefined()) {
        return App::DocumentObject::StdReturn;
    }

    auto getSectionShape = [](App::DocumentObject* feature,
                              const std::vector<std::string>& subs) -> Part::TopoShape {
        if (!feature || !feature->isDerivedFrom<Part::Feature>()) {
            throw Base::TypeError("Pipe: Invalid profile/section");
        }

        auto subName = subs.empty() ? "" : subs.front();

        // only take the entire shape when we have a sketch selected, but
        // not a point of the sketch
        if (feature->isDerivedFrom<Part::Part2DObject>() && subName.compare(0, 6, "Vertex") != 0) {
            return static_cast<Part::Part2DObject*>(feature)->Shape.getShape();
        }
        else {
            if (subName.empty()) {
                throw Base::ValueError("Pipe: No valid subelement linked in Part::Feature");
            }
            return static_cast<Part::Feature*>(feature)->Shape.getShape().getSubTopoShape(
                subName.c_str()
            );
        }
    };


    std::vector<std::vector<Part::TopoShape>> wiresections;

    auto addWiresToWireSections = [](TopoShape& section,
                                     std::vector<std::vector<TopoShape>>& wiresections) -> size_t {
        std::vector<Part::TopoShape> subs = section.getSubTopoShapes(TopAbs_WIRE);
        bool initialWireSectionsEmpty = wiresections.empty();
        size_t i = 0;
        for (const auto& sub : subs) {
            if (i >= wiresections.size()) {
                if (initialWireSectionsEmpty) {
                    wiresections.emplace_back(1, sub);
                }
                else {
                    throw Base::ValueError(
                        "Pipe: Sections need to have the same amount of inner wires (except "
                        "profile and last section, which can be points)"
                    );
                }
            }
            else {
                wiresections[i].push_back(sub);
            }
            ++i;
        }
        return i;
    };

    // TODO: currently we can only allow planar faces, so add that check.
    // The reason for this is that with other faces in front, we could not use the
    // current simulate approach and build the start and end face from the wires.
    // As the shell begins always at the spine and not the profile, the sketchshape
    // cannot be used directly as front face. We would need a method to translate
    // the front shape to match the shell starting position somehow...
    std::vector<TopoShape> wires;
    Part::TopoShape profilePoint;

    // if the Base property has a valid shape, fuse the pipe into it
    Part::TopoShape base;
    try {
        base = getBaseTopoShape();
    }
    catch (const Base::Exception&) {
        base = Part::TopoShape(getID(), this->getDocument()->getStringHasher());
    }

    auto hasher = getDocument()->getStringHasher();

    try {
        // setup the location
        this->positionByPrevious();
        TopLoc_Location invObjLoc = this->getLocation().Inverted();
        if (!base.isNull()) {
            base.move(invObjLoc);
        }

        // setup the profile section
        Part::TopoShape profileShape = getSectionShape(Profile.getValue(), Profile.getSubValues());
        if (profileShape.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Pipe: Could not obtain profile shape")
            );
        }

        // build the paths
        App::DocumentObject* spine = Spine.getValue();
        if (!(spine && spine->isDerivedFrom<Part::Feature>())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "No spine linked"));
        }

        std::vector<std::string> subedge = Spine.getSubValues();
        Part::TopoShape path;
        const Part::TopoShape& shape = static_cast<Part::Feature*>(spine)->Shape.getShape();
        buildPipePath(shape, subedge, path);
        path.move(invObjLoc);

        // auxiliary
        Part::TopoShape auxpath;
        if (Mode.getValue() == 3) {
            App::DocumentObject* auxspine = AuxiliarySpine.getValue();
            if (!(auxspine && auxspine->isDerivedFrom<Part::Feature>())) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "No auxiliary spine linked.")
                );
            }
            std::vector<std::string> auxsubedge = AuxiliarySpine.getSubValues();

            const Part::TopoShape& auxshape = static_cast<Part::Feature*>(auxspine)->Shape.getValue();
            buildPipePath(auxshape, auxsubedge, auxpath);
            auxpath.move(invObjLoc);
        }

        // build up multisections
        auto multisections = Sections.getSubListValues();
        std::vector<std::vector<TopoShape>> wiresections;

        size_t numWires = addWiresToWireSections(profileShape, wiresections);
        if (numWires == 0) {
            // profileShape had no wires so only other valid option is single point section
            size_t i = 0;
            for (auto& vertex : profileShape.getSubTopoShapes(TopAbs_VERTEX)) {
                profilePoint = vertex;
            }
            if (i > 1) {
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                    "Exception",
                    "Pipe: Only one isolated point is needed if using a sketch with isolated "
                    "points for section"
                ));
            }
        }

        if (!profilePoint.isNull() && (Transformation.getValue() != 1 || multisections.empty())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                "Exception",
                "Pipe: At least one section is needed when using a single point for profile"
            ));
        }

        // maybe we need a scaling law
        Handle(Law_Function) scalinglaw;

        bool isLastSectionVertex = false;

        // see if we shall use multiple sections
        if (Transformation.getValue() == 1) {
            // TODO: we need to order the sections to prevent occ from crashing,
            // as makepipeshell connects the sections in the order of adding
            for (auto& subSet : multisections) {
                if (!subSet.first->isDerivedFrom<Part::Feature>()) {
                    return new App::DocumentObjectExecReturn(
                        QT_TRANSLATE_NOOP("Exception", "Pipe: All sections need to be Part features")
                    );
                }

                // if the section is an object's face then take just the face
                Part::TopoShape shape = getSectionShape(subSet.first, subSet.second);
                if (shape.isNull()) {
                    return new App::DocumentObjectExecReturn(
                        QT_TRANSLATE_NOOP("Exception", "Pipe: Could not obtain section shape")
                    );
                }

                size_t nWiresAdded = addWiresToWireSections(shape, wiresections);
                if (nWiresAdded == 0) {
                    for (auto& vertex : shape.getSubTopoShapes(TopAbs_VERTEX)) {
                        if (isLastSectionVertex) {
                            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                                "Exception",
                                "Pipe: Only the profile and last section can be vertices"
                            ));
                        }
                        isLastSectionVertex = true;
                        for (auto& wires : wiresections) {
                            wires.push_back(vertex);
                        }
                    }
                }

                if (!isLastSectionVertex && nWiresAdded < wiresections.size()) {
                    return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                        "Exception",
                        "Multisections need to have the same amount of inner wires as the base "
                        "section"
                    ));
                }
            }
        }
        /*//build the law functions instead
        else if (Transformation.getValue() == 2) {
            if (ScalingData.getValues().size()<1)
                return new App::DocumentObjectExecReturn("No valid data given for linear scaling
        mode");

            Handle(Law_Linear) lin = new Law_Linear();
            lin->Set(0, 1, 1, ScalingData[0].x);

            scalinglaw = lin;
        }
        else if (Transformation.getValue() == 3) {
            if (ScalingData.getValues().size()<1)
                return new App::DocumentObjectExecReturn("No valid data given for S-shape scaling
        mode");

            Handle(Law_S) s = new Law_S();
            s->Set(0, 1, ScalingData[0].y, 1, ScalingData[0].x, ScalingData[0].z);

            scalinglaw = s;
        }*/

        // Verify that path is not a null shape
        if (path.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Path must not be a null shape")
            );
        }

        // build all shells
        std::vector<Part::TopoShape> shells;
        std::vector<PipeShellSegmentHistory> pipeShellSegmentHistory;
        bool collectSegmentHistory = profilePoint.isNull();

        Part::TopoShape copyProfilePoint(profilePoint);
        if (!profilePoint.isNull()) {
            copyProfilePoint.move(invObjLoc);
        }

        for (auto& wires : wiresections) {
            for (auto& wire : wires) {
                wire.move(invObjLoc);
            }
        }

        auto configurePipeShell = [&](BRepOffsetAPI_MakePipeShell& maker,
                                      const std::vector<Part::TopoShape>& wires) {
            setupAlgorithm(maker, auxpath.getShape());
            if (!scalinglaw) {
                if (!profilePoint.isNull()) {
                    maker.Add(copyProfilePoint.getShape());
                }
                for (const auto& wire : wires) {
                    maker.Add(wire.getShape());
                }
            }
            else {
                if (!profilePoint.isNull()) {
                    maker.SetLaw(copyProfilePoint.getShape(), scalinglaw);
                }
                for (const auto& wire : wires) {
                    maker.SetLaw(wire.getShape(), scalinglaw);
                }
            }
        };

        std::vector<Part::TopoShape> frontwires, backwires;
        for (auto& wires : wiresections) {
            BRepOffsetAPI_MakePipeShell mkPS(TopoDS::Wire(path.getShape()));
            configurePipeShell(mkPS, wires);

            if (!mkPS.IsReady()) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Pipe could not be built")
                );
            }

            Part::TopoShape shell = Part::TopoShape(0, this->getDocument()->getStringHasher());
            shell.makeElementShape(mkPS, wires, Part::OpCodes::PipeShell);
            shells.push_back(shell);

            if (collectSegmentHistory) {
                PipeShellSegmentHistory history;
                if (wires.empty()
                    || !collectPipeShellSegmentHistory(mkPS, path, wires.front(), shell, history)) {
                    collectSegmentHistory = false;
                    pipeShellSegmentHistory.clear();
                }
                else {
                    pipeShellSegmentHistory.push_back(std::move(history));
                }
            }

            if (!shell.isClosed()) {
                // shell is not closed - use simulate to get the end wires
                TopTools_ListOfShape sim;
                mkPS.Simulate(2, sim);

                if (wires.front().shapeType() != TopAbs_VERTEX) {
                    TopoShape front(sim.First());
                    if (front.countSubShapes(TopAbs_EDGE)
                        == wires.front().countSubShapes(TopAbs_EDGE)) {
                        front = wires.front();
                        front.setShape(sim.First(), false);
                    }
                    else {
                        front.Tag = -wires.front().Tag;
                    }
                    frontwires.push_back(front);
                }

                if (wires.back().shapeType() != TopAbs_VERTEX) {
                    TopoShape back(sim.Last());
                    if (back.countSubShapes(TopAbs_EDGE) == wires.back().countSubShapes(TopAbs_EDGE)) {
                        back = wires.back();
                        back.setShape(sim.Last(), false);
                    }
                    else {
                        back.Tag = -wires.back().Tag;
                    }
                    backwires.push_back(back);
                }
            }
        }

        auto collectRepairHistory = [&](const Part::TopoShape& repairPath,
                                        std::vector<PipeShellSegmentHistory>& histories) {
            histories.clear();
            for (const auto& wires : wiresections) {
                if (wires.empty()) {
                    return false;
                }

                BRepOffsetAPI_MakePipeShell maker(TopoDS::Wire(repairPath.getShape()));
                configurePipeShell(maker, wires);
                if (!maker.IsReady()) {
                    return false;
                }

                Part::TopoShape shell(0, hasher);
                shell.makeElementShape(maker, wires, Part::OpCodes::PipeShell);
                PipeShellSegmentHistory history;
                if (!collectPipeShellSegmentHistory(maker, repairPath, wires.front(), shell, history)) {
                    return false;
                }
                histories.push_back(std::move(history));
            }
            return !histories.empty();
        };

        Part::TopoShape result(0, getDocument()->getStringHasher());

        if (!frontwires.empty() || !backwires.empty()) {
            BRepBuilderAPI_Sewing sewer;
            sewer.SetTolerance(Precision::Confusion());
            for (auto& s : shells) {
                sewer.Add(s.getShape());
            }

            Part::TopoShape frontface, backface;
            gp_Pln pln;

            if (!frontwires.empty() && frontwires.front().hasSubShape(TopAbs_EDGE)) {
                if (!TopoShape(-1).makeElementCompound(frontwires).findPlane(pln)) {
                    try {
                        frontface.makeElementBSplineFace(frontwires);
                    }
                    catch (Base::Exception&) {
                        frontface.makeElementFilledFace(
                            frontwires,
                            Part::TopoShape::BRepFillingParams()
                        );
                    }
                }
                else {
                    frontface.makeElementFace(frontwires);
                }
                sewer.Add(frontface.getShape());
            }

            if (!backwires.empty() && backwires.front().hasSubShape(TopAbs_EDGE)) {
                // Explicitly set op code when making face to generate different
                // topo name than the front face.
                if (!Part::TopoShape(-1).makeElementCompound(backwires).findPlane(pln)) {
                    try {
                        backface.makeElementBSplineFace(
                            backwires,
                            Part::FillingStyle::stretch,
                            false,
                            Part::OpCodes::Sewing
                        );
                    }
                    catch (Base::Exception&) {
                        backface.makeElementFilledFace(
                            backwires,
                            TopoShape::BRepFillingParams(),
                            Part::OpCodes::Sewing
                        );
                    }
                }
                else {
                    backface.makeElementFace(backwires, Part::OpCodes::Sewing);
                }
                sewer.Add(backface.getShape());
            }

            sewer.Perform();
            result = result
                         .makeShapeWithElementMap(
                             sewer.SewedShape(),
                             Part::MapperSewing(sewer),
                             shells,
                             Part::OpCodes::Sewing
                         )
                         .makeElementSolid();
        }
        else {
            // shells are already closed - add them directly
            Part::TopoShape partCompound = TopoShape(0, getDocument()->getStringHasher());
            partCompound.makeElementCompound(shells);

            result.makeElementSolid(partCompound);
        }

        if (!result.countSubShapes(TopAbs_SHELL)) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Loft: Failed to create shell")  // isn't the prefix
                                                                                // supposed to be "Pipe:"
            );
        }

        auto shapes = result.getSubTopoShapes(TopAbs_SHELL);
        for (auto& s : shapes) {
            // build the solid
            s = s.makeElementSolid();
            BRepClass3d_SolidClassifier SC(s.getShape());
            SC.PerformInfinitePoint(Precision::Confusion());
            if (SC.State() == TopAbs_IN) {
                s.setShape(s.getShape().Reversed(), false);
            }
        }

        // A completed sweep can contain a self-crossing shell. Detect that case after the normal
        // sweep is complete, then replace it with the union of its valid per-spine-edge solids.
        // Non-self-intersecting pipes stay on the existing fast path.
        if (hasSelfIntersections(result.getShape())) {
            Part::TopoShape repaired;
            if (collectSegmentHistory && pipeShellSegmentHistory.size() == shells.size()) {
                repaired = fusePipeSegments(pipeShellSegmentHistory, hasher, FuzzyTolerance.getValue());
            }
            // A curved spine edge can overlap itself before OCCT creates another topological edge.
            // Subdivide the exact curves until every swept segment is suitable for Boolean fusion.
            for (int segmentsPerEdge = 2;
                 repaired.isNull() && profilePoint.isNull() && segmentsPerEdge <= 64;
                 segmentsPerEdge *= 2) {
                Part::TopoShape repairPath = splitPipePath(path, segmentsPerEdge);
                if (repairPath.isNull()) {
                    break;
                }
                std::vector<PipeShellSegmentHistory> histories;
                if (!collectRepairHistory(repairPath, histories)) {
                    continue;
                }
                repaired = fusePipeSegments(histories, hasher, FuzzyTolerance.getValue());
            }
            if (repaired.isNull()) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Pipe: Failed to repair self-intersecting sweep")
                );
            }
            result = repaired;
            shapes = {repaired};
        }

        // Do not use result itself as the makeElementCompound() target. In the repaired case that
        // would replace the valid solid with a compound before it reaches the final Boolean cut.
        if (shapes.size() == 1) {
            AddSubShape.setValue(shapes.front());
        }
        else {
            Part::TopoShape addSubShape(0, hasher);
            AddSubShape.setValue(addSubShape.makeElementCompound(shapes));
        }

        if (shapes.size() > 1) {
            result.makeElementFuse(shapes);
        }
        else {
            result = shapes.front();
        }

        if (base.isNull()) {
            if (getAddSubType() == FeatureAddSub::Subtractive) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Pipe: There is nothing to subtract from")
                );
            }

            if (!isSingleSolidRuleSatisfied(result.getShape())) {
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                    "Exception",
                    "Result has multiple solids: enable 'Allow Compound' in the active body."
                ));
            }

            // store shape before refinement
            this->rawShape = result;

            result = refineShapeIfActive(result);
            Shape.setValue(getSolid(result));
            return App::DocumentObject::StdReturn;
        }

        std::string maker;
        Part::TopoShape boolOp = Part::TopoShape(base.Tag, getDocument()->getStringHasher());

        if (getAddSubType() == FeatureAddSub::Additive) {
            maker = Part::OpCodes::Fuse;
        }
        else if (getAddSubType() == FeatureAddSub::Subtractive) {
            maker = Part::OpCodes::Cut;
        }

        if (!maker.empty()) {
            result.Tag = -getID();  // invert tag to differentiate the pre-boolean pipe
            //                        from the post-boolean pipe
            //                        setting result to the negative tag is a bit confusing,
            //                        because you would expect this to be set to the feature's shape,
            //                        but boolOp is the topoShape that is actually being copied

            boolOp.makeElementBoolean(maker.c_str(), {base, result}, nullptr, FuzzyTolerance.getValue());

            if (!isSingleSolidRuleSatisfied(boolOp.getShape())) {
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                    "Exception",
                    "Result has multiple solids: enable 'Allow Compound' in the active body."
                ));
            }

            // store shape before refinement
            this->rawShape = boolOp;
            boolOp = refineShapeIfActive(boolOp);
            Shape.setValue(getSolid(boolOp));
        }
        else {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Pipe: Invalid Boolean Type")
            );
        }

        TopoShape solid = getSolid(boolOp);
        // lets check if the result is a solid
        if (solid.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Resulting shape is not a solid")
            );
        }

        // store shape before refinement
        this->rawShape = boolOp;
        boolOp = refineShapeIfActive(boolOp);
        if (!isSingleSolidRuleSatisfied(boolOp.getShape())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                "Exception",
                "Result has multiple solids: enable 'Allow Compound' in the active body."
            ));
        }
        boolOp = getSolid(boolOp);
        Shape.setValue(boolOp);
        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (...) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "A fatal error occurred when making the pipe")
        );
    }
}

void Pipe::setupAlgorithm(BRepOffsetAPI_MakePipeShell& mkPipeShell, const TopoDS_Shape& auxshape)
{

    mkPipeShell.SetTolerance(Precision::Confusion());

    switch (Transition.getValue()) {
        case 0:
            mkPipeShell.SetTransitionMode(BRepBuilderAPI_Transformed);
            break;
        case 1:
            mkPipeShell.SetTransitionMode(BRepBuilderAPI_RightCorner);
            break;
        case 2:
            mkPipeShell.SetTransitionMode(BRepBuilderAPI_RoundCorner);
            break;
    }

    bool auxiliary = false;
    const Base::Vector3d& bVec = Binormal.getValue();
    switch (Mode.getValue()) {
        case 1:
            mkPipeShell.SetMode(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
            break;
        case 2:
            mkPipeShell.SetMode(true);
            break;
        case 3:
            auxiliary = true;
            break;
        case 4:
            mkPipeShell.SetMode(gp_Dir(bVec.x, bVec.y, bVec.z));
            break;
    }

    if (auxiliary) {
        mkPipeShell.SetMode(TopoDS::Wire(auxshape), AuxiliaryCurvilinear.getValue());
        // mkPipeShell.SetMode(TopoDS::Wire(auxshape), AuxiliaryCurvilinear.getValue(),
        // BRepFill_ContactOnBorder);
    }
}


void Pipe::getContinuousEdges(Part::TopoShape /*TopShape*/, std::vector<std::string>& /*SubNames*/)
{
    /*
    TopTools_IndexedMapOfShape mapOfEdges;
    TopTools_IndexedDataMapOfShapeListOfShape mapEdgeEdge;
    TopExp::MapShapesAndAncestors(TopShape.getShape(), TopAbs_EDGE, TopAbs_EDGE, mapEdgeEdge);
    TopExp::MapShapes(TopShape.getShape(), TopAbs_EDGE, mapOfEdges);

    Base::Console().message("Initial edges:\n");
    for (int i=0; i<SubNames.size(); ++i)
        Base::Console().message("Subname: %s\n", SubNames[i].c_str());

    unsigned int i = 0;
    while(i < SubNames.size())
    {
        std::string aSubName = static_cast<std::string>(SubNames.at(i));

        if (aSubName.compare(0, 4, "Edge") == 0) {
            TopoDS_Edge edge = TopoDS::Edge(TopShape.getSubShape(aSubName.c_str()));
            const TopTools_ListOfShape& los = mapEdgeEdge.FindFromKey(edge);

            if (los.Extent() != 2)
            {
                SubNames.erase(SubNames.begin()+i);
                continue;
            }

            const TopoDS_Shape& face1 = los.First();
            const TopoDS_Shape& face2 = los.Last();
            GeomAbs_Shape cont = BRep_Tool::Continuity(TopoDS::Edge(edge),
                                                       TopoDS::Face(face1),
                                                       TopoDS::Face(face2));
            if (cont != GeomAbs_C0) {
                SubNames.erase(SubNames.begin()+i);
                continue;
            }

            i++;
        }
        // empty name or any other sub-element
        else {
            SubNames.erase(SubNames.begin()+i);
        }
    }

    Base::Console().message("Final edges:\n");
    for (int i=0; i<SubNames.size(); ++i)
        Base::Console().message("Subname: %s\n", SubNames[i].c_str());
    */
}

void Pipe::buildPipePath(
    const Part::TopoShape& shape,
    const std::vector<std::string>& subedge,
    Part::TopoShape& path
)
{
    if (!shape.isNull()) {
        try {
            if (!subedge.empty()) {
                // if (SpineTangent.getValue())
                // getContinuousEdges(shape, subedge);

                std::vector<Part::TopoShape> shapes;
                for (const auto& it : subedge) {
                    shapes.push_back(shape.getSubTopoShape(it.c_str()));
                }
                path = path.makeElementWires(shapes);
            }
            else if (shape.shapeType() == TopAbs_EDGE) {
                path = shape.getShape();
            }
            else if (shape.shapeType() == TopAbs_WIRE) {
                path = path.makeElementWires(shape);
            }
            else if (shape.shapeType() == TopAbs_COMPOUND) {
                TopoDS_Iterator it(shape.getShape());
                for (; it.More(); it.Next()) {
                    if (it.Value().IsNull()) {
                        throw Base::ValueError(
                            QT_TRANSLATE_NOOP("Exception", "Invalid element in spine.")
                        );
                    }
                    if ((it.Value().ShapeType() != TopAbs_EDGE)
                        && (it.Value().ShapeType() != TopAbs_WIRE)) {
                        throw Base::TypeError(QT_TRANSLATE_NOOP(
                            "Exception",
                            "Element in spine is neither an edge nor a wire."
                        ));
                    }
                }

                std::vector<Part::TopoShape> edges = shape.getSubTopoShapes(TopAbs_EDGE);

                path = path.makeElementWires(edges);
            }
            else {
                throw Base::TypeError(
                    QT_TRANSLATE_NOOP("Exception", "Spine is neither an edge nor a wire.")
                );
            }
        }
        catch (Standard_Failure&) {
            throw Base::CADKernelError(QT_TRANSLATE_NOOP("Exception", "Invalid spine."));
        }
    }
}
PROPERTY_SOURCE(PartDesign::AdditivePipe, PartDesign::Pipe)
AdditivePipe::AdditivePipe()
{
    addSubType = Additive;
}

PROPERTY_SOURCE(PartDesign::SubtractivePipe, PartDesign::Pipe)
SubtractivePipe::SubtractivePipe()
{
    addSubType = Subtractive;
}

void Pipe::handleChangedPropertyType(Base::XMLReader& reader, const char* TypeName, App::Property* prop)
{
    // property Sections had the App::PropertyLinkList and was changed to App::PropertyXLinkSubList
    if (prop == &Sections && strcmp(TypeName, "App::PropertyLinkList") == 0) {
        Sections.upgrade(reader, TypeName);
    }
    else {
        ProfileBased::handleChangedPropertyType(reader, TypeName, prop);
    }
}

void Pipe::handleChangedPropertyName(Base::XMLReader& reader, const char* TypeName, const char* PropName)
{
    // The AuxiliarySpine property was AuxillerySpine in the past
    std::string strAuxillerySpine("AuxillerySpine");
    // The AuxiliarySpineTangent property was AuxillerySpineTangent in the past
    std::string strAuxillerySpineTangent("AuxillerySpineTangent");
    // The AuxiliaryCurvilinear property was AuxilleryCurvelinear in the past
    std::string strAuxilleryCurvelinear("AuxilleryCurvelinear");
    Base::Type type = Base::Type::fromName(TypeName);
    if (AuxiliarySpine.getClassTypeId() == type && strAuxillerySpine == PropName) {
        AuxiliarySpine.Restore(reader);
    }
    else if (AuxiliarySpineTangent.getClassTypeId() == type && strAuxillerySpineTangent == PropName) {
        AuxiliarySpineTangent.Restore(reader);
    }
    else if (AuxiliaryCurvilinear.getClassTypeId() == type && strAuxilleryCurvelinear == PropName) {
        AuxiliaryCurvilinear.Restore(reader);
    }
    else {
        ProfileBased::handleChangedPropertyName(reader, TypeName, PropName);
    }
}
