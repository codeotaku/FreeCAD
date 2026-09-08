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

    def _settle_gui(self):
        import FreeCADGui as Gui
        from PySide import QtCore

        Gui.updateGui()
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(100, loop.quit)
        loop.exec()

    def _edit_fillet(self, fillet):
        import FreeCADGui as Gui
        from PySide import QtWidgets

        Gui.activateWorkbench("PartDesignWorkbench")
        self.Doc.openTransaction("Edit fillet")
        self.assertTrue(Gui.activeDocument().setEdit(fillet.Name))
        Gui.Control.showTaskView()
        self._settle_gui()
        window = Gui.getMainWindow()
        window.findChild(QtWidgets.QToolButton, "buttonRefSel").setChecked(False)
        return window

    @unittest.skipUnless(FreeCAD.GuiUp, "Requires the native task panel")
    def testLegacyUniformFilletUsesCurrentTaskPanel(self):
        import FreeCADGui as Gui
        from PySide import QtWidgets

        with tempfile.TemporaryDirectory() as directory:
            for refs in ([""], ["Face50"]):
                with self.subTest(refs=refs):
                    fillet = self._open_legacy_fillet(directory, refs, True)
                    for accept in (False, True):
                        window = self._edit_fillet(fillet)
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
                            for box in tasks.currentWidget().findChildren(QtWidgets.QDialogButtonBox)
                            if box.button(role)
                        )
                        button.click()
                        self._settle_gui()
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

    @unittest.skipUnless(FreeCAD.GuiUp, "Requires the native task panel")
    def testPositionEditorsFollowUpstreamEdgeLength(self):
        from PySide import QtWidgets

        _body, box, fillet = self._create_box_with_fillet()
        box.Length, box.Width, box.Height = 30, 20, 16
        fillet.Base = (box, ["Edge2"])
        fillet.RadiusMode = 1
        fillet.VariableRadiusData = {"Edge2": "0,2;0.4,3;0.72,2.5;1,2"}
        fillet.VariableRadiusControlPointIds = {"Edge2": "cp1;cp2"}
        self.Doc.recompute()
        window = self._edit_fillet(fillet)
        table = window.findChild(QtWidgets.QTableWidget, "controlPointTable")
        position = table.cellWidget(1, 1)
        self.assertAlmostEqual(position.property("rawValue"), 8)

        box.Width = 30
        self.Doc.recompute()
        self._settle_gui()
        self.assertEqual(table.cellWidget(1, 1), position)
        self.assertAlmostEqual(position.property("rawValue"), 12)
        self.assertAlmostEqual(position.property("maximum"), 30)
        self.assertAlmostEqual(table.cellWidget(3, 1).property("rawValue"), 30)

        position.setProperty("rawValue", 9)
        self._settle_gui()
        self.assertAlmostEqual(position.property("rawValue"), 9)
        self.assertAlmostEqual(float(fillet.VariableRadiusControlPointValues["Edge2|cp1|length"]), 9)
        box.Width = 40
        self.Doc.recompute()
        self._settle_gui()
        self.assertAlmostEqual(position.property("rawValue"), 9)  # Absolute point stays put.
        self.assertAlmostEqual(table.cellWidget(2, 1).property("rawValue"), 28.8)
        self.assertAlmostEqual(position.property("maximum"), 40)

        units = window.findChild(QtWidgets.QComboBox, "controlPointPositionUnits")
        units.setCurrentIndex(1)
        self._settle_gui()
        table.cellWidget(1, 1).setProperty("value", 0.25)
        self._settle_gui()
        box.Width = 50
        self.Doc.recompute()
        self._settle_gui()
        self.assertAlmostEqual(table.cellWidget(1, 1).property("value"), 0.25)
        units.setCurrentIndex(0)
        self._settle_gui()
        self.assertAlmostEqual(table.cellWidget(1, 1).property("rawValue"), 12.5)
        box.Width = 25
        self.Doc.recompute()
        self._settle_gui()
        self.assertAlmostEqual(table.cellWidget(1, 1).property("rawValue"), 6.25)
        self.assertAlmostEqual(table.cellWidget(1, 1).property("maximum"), 25)
        self.assertTrue(fillet.isValid())

    @unittest.skipUnless(FreeCAD.GuiUp, "Requires the native task panel")
    def testRemovingFacePreservesSharedBoundary(self):
        import FreeCADGui as Gui
        from PySide import QtWidgets

        for mode in (0, 1):
            with self.subTest(mode=mode):
                _body, box, fillet = self._create_box_with_fillet()
                fillet.Radius = 0.5
                fillet.RadiusMode = mode
                # Explicit references also represent independently edited face boundaries.
                fillet.Base = (box, ["Face1", "Face3", "Edge1", "Edge2"])
                fillet.VariableRadiusData = {"Edge1": "0,0.5;1,0.5", "Edge2": "0,0.5;1,0.5"}
                fillet.VariableRadiusControlPointValues = {"Edge1|start|radius": "0.5"}
                fillet.setExpression(
                    "VariableRadiusControlPointValues[<<Edge1|start|radius>>]", "0.5 mm"
                )
                self.Doc.recompute()
                window = self._edit_fillet(fillet)
                tree = window.findChild(QtWidgets.QTreeWidget, "treeWidgetReferences")
                model = tree.model()
                # Model indexes avoid ownership changes to Python wrappers of cloned tree items.
                face1 = next(
                    model.index(i, 0) for i in range(model.rowCount())
                    if model.index(i, 0).data() == "Face1"
                )
                tree.setCurrentIndex(face1)
                remove = window.findChild(QtWidgets.QToolButton, "removeGeometry")
                remove.click()
                self._settle_gui()
                self.assertEqual(set(fillet.Base[1]), {"Face3", "Edge1"})
                self.assertNotIn("Edge2", fillet.VariableRadiusData)
                self.assertIn("Edge1", fillet.VariableRadiusData)
                self.assertEqual(len(fillet.ExpressionEngine), 1)
                self.assertTrue(fillet.isValid())

                face3 = model.index(0, 0)
                self.assertEqual(face3.data(), "Face3")
                self.assertEqual(model.rowCount(face3), 4)
                edge1 = next(
                    model.index(i, 0, face3) for i in range(model.rowCount(face3))
                    if model.index(i, 0, face3).data() == "Edge1"
                )
                tree.setCurrentIndex(edge1)
                remove.click()
                self._settle_gui()
                # Explicitly deleting the edge removes it from the retained face and Base.
                self.assertEqual(set(fillet.Base[1]), {"Edge9", "Edge5", "Edge10"})
                self.assertNotIn("Edge1", fillet.VariableRadiusData)
                self.assertEqual(fillet.ExpressionEngine, [])
                self.assertTrue(fillet.isValid())
                Gui.activeDocument().resetEdit()
                self._settle_gui()

    @unittest.skipUnless(FreeCAD.GuiUp, "Requires the native task panel")
    def testAddAllEdgesUpdatesUniformPreview(self):
        from PySide import QtWidgets

        _body, box, fillet = self._create_box_with_fillet()
        fillet.Radius = 0.5
        self.Doc.recompute()
        window = self._edit_fillet(fillet)
        tree = window.findChild(QtWidgets.QTreeWidget, "treeWidgetReferences")
        next(action for action in tree.actions() if action.text() == "Add All Edges").trigger()
        self._settle_gui()
        self.assertEqual(len(fillet.Base[1]), 12)
        expected = box.Shape.makeFillet(0.5, box.Shape.Edges)
        self.assertAlmostEqual(fillet.Shape.Volume, expected.Volume)
        self.assertTrue(fillet.isValid())

        # The native radius gizmo writes this editor, not the visible Uniform field.
        window.findChild(QtWidgets.QAbstractSpinBox, "filletRadius").setProperty("rawValue", 0.6)
        self._settle_gui()
        self.assertAlmostEqual(fillet.Radius.Value, 0.6)
        self.assertAlmostEqual(
            window.findChild(QtWidgets.QAbstractSpinBox, "defaultRadiusEditor").property("rawValue"),
            0.6,
        )
        self.assertAlmostEqual(fillet.Shape.Volume, box.Shape.makeFillet(0.6, box.Shape.Edges).Volume)

    def tearDown(self):
        if FreeCAD.GuiUp:
            import FreeCADGui as Gui
            if Gui.activeDocument() and Gui.activeDocument().getInEdit():
                Gui.activeDocument().resetEdit()
        # closing doc
        FreeCAD.closeDocument(self.Doc.Name)
        # print ("omit closing document for debugging")
