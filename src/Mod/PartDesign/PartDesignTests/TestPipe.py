# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with this program; if not, write to the Free Software   *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

import os
import tempfile
import unittest

import FreeCAD
import Part
import Sketcher
import TestSketcherApp

App = FreeCAD


class TestPipe(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestPipe")

    def createSelfIntersectingPipe(self, pipe_type="PartDesign::SubtractivePipe"):
        body = self.Doc.addObject("PartDesign::Body", "Body")
        profile = body.newObject("Sketcher::SketchObject", "ProfileSketch")
        TestSketcherApp.CreateCircleSketch(profile, (0, 0), 0.5)

        spine = body.newObject("Sketcher::SketchObject", "SpineSketch")
        spine.MapMode = "FlatFace"
        spine.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        points = [(0, 0), (0, 8), (8, 0), (8, 8), (-2, 2)]
        for start, end in zip(points, points[1:]):
            spine.addGeometry(Part.LineSegment(App.Vector(*start, 0), App.Vector(*end, 0)), False)

        is_additive = pipe_type == "PartDesign::AdditivePipe"
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = 2 if is_additive else 13
        box.Width = 2
        box.Height = 2 if is_additive else 11
        box.Placement.Base = App.Vector(-1 if is_additive else -3, -1, -1)

        pipe = body.newObject(pipe_type, pipe_type.removeprefix("PartDesign::"))
        pipe.Profile = profile
        pipe.Spine = spine
        self.Doc.recompute()
        return box, pipe

    def testSimpleAdditivePipeCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.ProfileSketch = self.Doc.addObject("Sketcher::SketchObject", "ProfileSketch")
        self.Body.addObject(self.ProfileSketch)
        TestSketcherApp.CreateCircleSketch(self.ProfileSketch, (0, 0), 1)
        self.Doc.recompute()
        self.SpineSketch = self.Doc.addObject("Sketcher::SketchObject", "SpineSketch")
        self.Body.addObject(self.SpineSketch)
        self.SpineSketch.MapMode = "FlatFace"
        self.SpineSketch.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        self.SpineSketch.addGeometry(
            Part.LineSegment(App.Vector(0.0, 0.0, 0), App.Vector(0, 1, 0)), False
        )
        self.SpineSketch.addConstraint(Sketcher.Constraint("Coincident", 0, 1, -1, 1))
        self.SpineSketch.addConstraint(Sketcher.Constraint("PointOnObject", 0, 2, -2))
        self.SpineSketch.addConstraint(Sketcher.Constraint("DistanceY", 0, 1, 0, 2, 1))
        self.Doc.recompute()
        self.AdditivePipe = self.Doc.addObject("PartDesign::AdditivePipe", "AdditivePipe")
        self.Body.addObject(self.AdditivePipe)
        self.AdditivePipe.Profile = self.ProfileSketch
        self.AdditivePipe.Spine = self.SpineSketch
        self.Doc.recompute()
        self.assertAlmostEqual(self.AdditivePipe.Shape.Volume, 3.14159265)

    def testSimpleSubtractivePipeCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.ProfileSketch = self.Doc.addObject("Sketcher::SketchObject", "ProfileSketch")
        self.Body.addObject(self.ProfileSketch)
        TestSketcherApp.CreateCircleSketch(self.ProfileSketch, (0, 0), 1)
        self.Doc.recompute()
        self.SpineSketch = self.Doc.addObject("Sketcher::SketchObject", "SpineSketch")
        self.Body.addObject(self.SpineSketch)
        self.SpineSketch.MapMode = "FlatFace"
        self.SpineSketch.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        self.SpineSketch.addGeometry(
            Part.LineSegment(App.Vector(0.0, 0.0, 0), App.Vector(0, 1, 0)), False
        )
        self.SpineSketch.addConstraint(Sketcher.Constraint("Coincident", 0, 1, -1, 1))
        self.SpineSketch.addConstraint(Sketcher.Constraint("PointOnObject", 0, 2, -2))
        self.SpineSketch.addConstraint(Sketcher.Constraint("DistanceY", 0, 1, 0, 2, 1))
        self.Doc.recompute()
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (-5, -5), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1.0
        self.Doc.recompute()
        self.SubtractivePipe = self.Doc.addObject("PartDesign::SubtractivePipe", "SubtractivePipe")
        self.Body.addObject(self.SubtractivePipe)
        self.SubtractivePipe.Profile = self.ProfileSketch
        self.SubtractivePipe.Spine = self.SpineSketch
        self.Doc.recompute()
        self.assertAlmostEqual(self.SubtractivePipe.Shape.Volume, 100 - 3.14159265)

    def testSelfIntersectingSubtractivePipeCase(self):
        box, pipe = self.createSelfIntersectingPipe()
        tool = pipe.AddSubShape
        self.assertEqual(pipe.getStatusString(), "Valid")
        self.assertTrue(tool.isValid())
        self.assertEqual(len(tool.Solids), 1)
        self.assertIsNone(tool.check(True))
        self.assertAlmostEqual(tool.Volume, 22.0452675083, delta=1e-5)
        self.assertTrue(pipe.Shape.isValid())
        self.assertEqual(len(pipe.Shape.Solids), 1)
        self.assertAlmostEqual(box.Shape.Volume - pipe.Shape.Volume, tool.Volume, delta=1e-5)

    def testSelfIntersectingAdditivePipeCase(self):
        box, pipe = self.createSelfIntersectingPipe("PartDesign::AdditivePipe")
        tool = pipe.AddSubShape
        self.assertEqual(pipe.getStatusString(), "Valid")
        self.assertTrue(tool.isValid())
        self.assertEqual(len(tool.Solids), 1)
        self.assertIsNone(tool.check(True))
        self.assertAlmostEqual(tool.Volume, 22.0452675083, delta=1e-5)
        self.assertTrue(pipe.Shape.isValid())
        self.assertEqual(len(pipe.Shape.Solids), 1)
        self.assertGreater(pipe.Shape.Volume, box.Shape.Volume)

    def testSelfIntersectingSingleEdgePipeCase(self):
        body = self.Doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = 14
        box.Width = 1
        box.Height = 12
        box.Placement.Base = App.Vector(-12, -0.5, 0)
        self.Doc.recompute()
        box_volume = box.Shape.Volume

        spine = body.newObject("Sketcher::SketchObject", "SpineSketch")
        spine.MapMode = "FlatFace"
        spine.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        # Exact spline from the issue #15589 manual reproduction file.
        curve = Part.BSplineCurve()
        curve.buildFromPolesMultsKnots(
            [
                App.Vector(-5.82199, 8.02192),
                App.Vector(-6.315050892114689, 7.612817129127397),
                App.Vector(-7.291235321015923, 6.230286162661495),
                App.Vector(-7.885170342922163, 3.7620686205933835),
                App.Vector(-5.739791047664699, 2.2020004258669634),
                App.Vector(-2.7248019245967763, 2.8162663187320494),
                App.Vector(-3.752281544780397, 5.075280773396591),
                App.Vector(-6.426355229054949, 5.019006957748416),
                App.Vector(-11.394062807358608, 10.050042378929394),
                App.Vector(1.0861958819427306, 9.40303998632162),
                App.Vector(-6.079900799147438, 2.3057206254345544),
                App.Vector(-8.87071117400314, 2.4081037391419593),
                App.Vector(-9.049073, 2.43216),
            ],
            [4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4],
            [
                0.0,
                2.3290237725922847,
                4.5186637559913825,
                6.816426554824195,
                9.319324823392057,
                10.560636879645438,
                12.57280392522093,
                17.251637871932438,
                22.63888182561599,
                30.608234361456024,
                31.14641731334135,
            ],
            False,
            3,
        )
        spine.addGeometry(curve, False)
        self.Doc.recompute()
        self.assertEqual(len(spine.Shape.Edges), 1)

        profile = body.newObject("Sketcher::SketchObject", "ProfileSketch")
        profile.AttachmentSupport = (spine, ["Edge1"])
        profile.MapMode = "NormalToEdge"
        TestSketcherApp.CreateCircleSketch(profile, (0, 0), 0.25)

        pipe = body.newObject("PartDesign::SubtractivePipe", "SubtractivePipe")
        pipe.Profile = profile
        pipe.Spine = spine
        self.Doc.recompute()

        tool = pipe.AddSubShape
        self.assertEqual(pipe.getStatusString(), "Valid")
        self.assertTrue(tool.isValid())
        self.assertEqual(len(tool.Solids), 1)
        self.assertIsNone(tool.check(True))
        self.assertAlmostEqual(tool.Volume, 6.3124, delta=1e-4)
        self.assertAlmostEqual(box_volume - pipe.Shape.Volume, tool.Volume, delta=1e-4)

        # Reuse the repaired tool additively; it should exactly restore the original box.
        additive_pipe = body.newObject("PartDesign::AdditivePipe", "AdditivePipe")
        additive_pipe.Profile = profile
        additive_pipe.Spine = spine
        self.Doc.recompute()

        additive_tool = additive_pipe.AddSubShape
        self.assertEqual(additive_pipe.getStatusString(), "Valid")
        self.assertTrue(additive_tool.isValid())
        self.assertEqual(len(additive_tool.Solids), 1)
        self.assertIsNone(additive_tool.check(True))
        self.assertAlmostEqual(additive_tool.Volume, tool.Volume, delta=1e-4)
        self.assertTrue(additive_pipe.Shape.isValid())
        self.assertEqual(len(additive_pipe.Shape.Solids), 1)
        self.assertAlmostEqual(additive_pipe.Shape.Volume, box_volume, delta=1e-4)

    def testSelfIntersectingSubtractivePipeNamingAndPersistence(self):
        _, pipe = self.createSelfIntersectingPipe()
        mapped_face = pipe.AddSubShape.ElementReverseMap["Face1"]
        self.assertNotEqual(mapped_face, "Face1")
        self.assertTrue(
            pipe.AddSubShape.getElement("Face1").isSame(
                pipe.AddSubShape.getElement(mapped_face)
            )
        )
        element_map_size = pipe.AddSubShape.ElementMapSize

        reference_body = self.Doc.addObject("PartDesign::Body", "ReferenceBody")
        binder = reference_body.newObject("PartDesign::SubShapeBinder", "ResultBinder")
        binder.Support = [(pipe, ["Face1"])]
        self.Doc.recompute()
        self.assertTrue(binder.Shape.isValid())

        with tempfile.TemporaryDirectory() as directory:
            filename = os.path.join(directory, self.Doc.Name + ".FCStd")
            self.Doc.saveAs(filename)
            FreeCAD.closeDocument(self.Doc.Name)
            self.Doc = FreeCAD.openDocument(filename)
            pipe = self.Doc.getObject("SubtractivePipe")
            pipe.touch()
            self.Doc.recompute()

            self.assertEqual(pipe.getStatusString(), "Valid")
            self.assertTrue(pipe.AddSubShape.isValid())
            self.assertEqual(pipe.AddSubShape.ElementMapSize, element_map_size)
            self.assertEqual(pipe.AddSubShape.ElementReverseMap["Face1"], mapped_face)
            self.assertTrue(self.Doc.getObject("ResultBinder").Shape.isValid())

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestPipe")
        # print ("omit closing document for debugging")
