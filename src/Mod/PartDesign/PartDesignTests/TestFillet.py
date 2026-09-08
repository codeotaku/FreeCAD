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

from __future__ import division
from math import pi
import unittest
import os
import tempfile
import xml.etree.ElementTree as ET
import zipfile

import FreeCAD


class TestFillet(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestFillet")

    def _create_box_with_fillet(self):
        body = self.Doc.addObject("PartDesign::Body", "Body")
        box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        box.Length = 10.00
        box.Width = 10.00
        box.Height = 10.00
        body.addObject(box)
        self.Doc.recompute()

        fillet = self.Doc.addObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 1.0
        body.addObject(fillet)
        self.Doc.recompute()
        self.assertTrue(fillet.isValid())
        return body, box, fillet

    def _find_edge_with_match_count(self, source_shape, target_shape, match_count):
        for index in range(1, source_shape.countElement("Edge") + 1):
            source_name = "Edge" + str(index)
            source_edge = source_shape.getElement(source_name, True)
            matches = target_shape.findSubShapesWithSharedVertex(
                source_edge,
                needName=True,
                checkGeometry=True,
            )
            if len(matches) == match_count:
                return source_name, matches[0][0] if matches else None
        self.skipTest("Test model did not contain a suitable edge")

    def testFilletCubeToSphere(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(self.Box)
        self.Box.Length = 10.00
        self.Box.Width = 10.00
        self.Box.Height = 10.00
        self.Doc.recompute()
        self.Fillet = self.Doc.addObject("PartDesign::Fillet", "Fillet")
        self.Fillet.Base = (self.Box, ["Face" + str(i + 1) for i in range(6)])
        self.Fillet.Radius = 4.999999
        self.Body.addObject(self.Fillet)
        self.Doc.recompute()
        self.assertAlmostEqual(self.Fillet.Shape.Volume, 4 / 3 * pi * 5**3, places=3)
        # test UseAllEdges property
        self.Fillet.UseAllEdges = True
        self.Fillet.Base = (self.Box, [""])  # no subobjects, should still work
        self.Doc.recompute()
        self.assertAlmostEqual(self.Fillet.Shape.Volume, 4 / 3 * pi * 5**3, places=3)
        self.Fillet.Base = (self.Box, ["Face50"])  # non-existent face, topo naming resilience
        self.Doc.recompute()
        self.assertAlmostEqual(self.Fillet.Shape.Volume, 4 / 3 * pi * 5**3, places=3)
        self.Fillet.UseAllEdges = False
        self.Fillet.Base = (self.Box, ["Face1"])
        self.Doc.recompute()
        self.assertNotAlmostEqual(self.Fillet.Shape.Volume, 4 / 3 * pi * 5**3, places=3)

    def testDeletingPreviousFeatureRelinksUniqueMatchingBaseEdge(self):
        body, box, fillet = self._create_box_with_fillet()
        old_edge, new_edge = self._find_edge_with_match_count(fillet.Shape, box.Shape, 1)

        followup = self.Doc.addObject("PartDesign::Fillet", "FollowupFillet")
        followup.Base = (fillet, [old_edge])
        followup.Radius = 0.25
        body.addObject(followup)
        self.Doc.recompute()
        self.assertTrue(followup.isValid())

        body.removeObject(fillet)

        self.assertEqual(followup.Base[0].Name, box.Name)
        self.assertEqual(list(followup.Base[1]), [new_edge])

    def testDeletingPreviousFeatureDoesNotRelinkUnsafeBaseEdge(self):
        body, box, fillet = self._create_box_with_fillet()
        old_edge, _new_edge = self._find_edge_with_match_count(fillet.Shape, box.Shape, 0)

        followup = self.Doc.addObject("PartDesign::Fillet", "FollowupFillet")
        followup.Base = (fillet, [old_edge])
        followup.Radius = 0.25
        body.addObject(followup)
        self.Doc.recompute()

        body.removeObject(fillet)

        if followup.Base[0]:
            self.assertNotEqual(followup.Base[0].Name, box.Name)

    def _open_legacy_fillet(self, directory, refs, use_all, old_float=False):
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.newDocument("PartDesignTestFillet")
        _body, box, fillet = self._create_box_with_fillet()
        fillet.Base = (box, refs)
        fillet.UseAllEdges = use_all
        self.Doc.recompute()
        volume = fillet.Shape.Volume
        current = os.path.join(directory, "current.FCStd")
        legacy = os.path.join(directory, "PartDesignTestFillet.FCStd")
        self.Doc.saveAs(current)
        # Simulate the old file schema, including pre-QuantityConstraint radii.
        new_properties = {
            "RadiusMode",
            "RadiusLawModes",
            "VariableRadiusData",
            "VariableRadiusControlPointIds",
            "VariableRadiusControlPointValues",
        }
        with zipfile.ZipFile(current) as source, zipfile.ZipFile(legacy, "w") as target:
            for entry in source.infolist():
                data = source.read(entry.filename)
                if entry.filename == "Document.xml":
                    root = ET.fromstring(data)
                    for props in root.iter("Properties"):
                        removed = 0
                        for prop in list(props):
                            if prop.get("name") in new_properties:
                                props.remove(prop)
                                removed += 1
                            elif old_float and prop.get("name") == "Radius":
                                prop.set("type", "App::PropertyFloatConstraint")
                        props.set("Count", str(int(props.get("Count")) - removed))
                    data = ET.tostring(root, encoding="utf-8", xml_declaration=True)
                target.writestr(entry, data)
        FreeCAD.closeDocument(self.Doc.Name)
        self.Doc = FreeCAD.openDocument(legacy)
        fillet = self.Doc.getObject("Fillet")
        fillet.touch()
        self.Doc.recompute()
        self.assertEqual(fillet.RadiusMode, "Constant Radius")
        self.assertEqual(list(fillet.Base[1]), refs)
        self.assertEqual(fillet.UseAllEdges, use_all)
        self.assertTrue(fillet.isValid())
        self.assertAlmostEqual(fillet.Shape.Volume, volume)
        return fillet

    def testLegacyUniformFilletSaveRestore(self):
        with tempfile.TemporaryDirectory() as directory:
            for refs, use_all, old_float in [
                (["Edge1"], False, False),
                (["Face1"], False, False),
                ([""], True, False),
                (["Face50"], True, False),
                (["Edge1"], False, True),
            ]:
                with self.subTest(refs=refs, use_all=use_all, old_float=old_float):
                    fillet = self._open_legacy_fillet(directory, refs, use_all, old_float)
                    volume = fillet.Shape.Volume
                    self.Doc.save()
                    self.Doc.restore()
                    self.Doc.getObject("Fillet").touch()
                    self.Doc.recompute()
                    self.assertAlmostEqual(self.Doc.getObject("Fillet").Shape.Volume, volume)

    @unittest.skipUnless(FreeCAD.GuiUp, "Requires the native task panel")
    def testLegacyUniformFilletUsesCurrentTaskPanel(self):
        import FreeCADGui as Gui
        from PySide import QtCore, QtWidgets

        def settle():
            loop = QtCore.QEventLoop()
            QtCore.QTimer.singleShot(100, loop.quit)
            loop.exec()

        Gui.activateWorkbench("PartDesignWorkbench")
        window = Gui.getMainWindow()
        with tempfile.TemporaryDirectory() as directory:
            for refs in ([""], ["Face50"]):
                with self.subTest(refs=refs):
                    fillet = self._open_legacy_fillet(directory, refs, True)
                    for accept in (False, True):
                        self.Doc.openTransaction("Edit fillet")
                        self.assertTrue(Gui.activeDocument().setEdit(fillet.Name))
                        Gui.updateGui()
                        settle()
                        mode = window.findChild(QtWidgets.QComboBox, "filletType")
                        self.assertEqual(
                            [mode.itemText(i) for i in range(mode.count())], ["Uniform", "Various"]
                        )
                        radius = window.findChild(
                            QtWidgets.QAbstractSpinBox, "defaultRadiusEditor"
                        )
                        radius.setProperty("rawValue", 1.1)
                        Gui.updateGui()
                        role = (
                            QtWidgets.QDialogButtonBox.Ok
                            if accept else QtWidgets.QDialogButtonBox.Cancel
                        )
                        tasks = window.findChild(QtWidgets.QStackedWidget, "Tasks")
                        button = next(
                            box.button(role)
                            for box in tasks.findChildren(QtWidgets.QDialogButtonBox)
                            if box.button(role) and box.button(role).isVisible()
                        )
                        button.click()
                        Gui.updateGui()
                        settle()
                        self.assertFalse(
                            Gui.activeDocument().getInEdit(), "OK" if accept else "Cancel"
                        )
                        self.assertAlmostEqual(fillet.Radius.Value, 1.1 if accept else 1.0)
                        self.assertEqual(list(fillet.Base[1]), refs)
                        self.assertTrue(fillet.UseAllEdges)
                    self.Doc.save()
                    self.Doc.restore()
                    self.Doc.getObject("Fillet").touch()
                    self.Doc.recompute()
                    self.assertTrue(self.Doc.getObject("Fillet").isValid())
                    self.assertAlmostEqual(self.Doc.getObject("Fillet").Radius.Value, 1.1)

    def tearDown(self):
        if FreeCAD.GuiUp:
            import FreeCADGui as Gui
            if Gui.activeDocument() and Gui.activeDocument().getInEdit():
                Gui.activeDocument().resetEdit()
        # closing doc
        FreeCAD.closeDocument(self.Doc.Name)
        # print ("omit closing document for debugging")
