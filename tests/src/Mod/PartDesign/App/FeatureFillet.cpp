// SPDX-License-Identifier: LGPL-2.1-or-later

#include <filesystem>

#include <gtest/gtest.h>

#include "src/App/InitApplication.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <Law_Function.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <App/Application.h>
#include <App/Document.h>
#include <App/ExpressionParser.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/Feature.h>
#include <Mod/PartDesign/App/FeatureFillet.h>

class FeatureFilletTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        documentName = App::GetApplication().getUniqueDocumentName("FeatureFillet_test");
        document = App::GetApplication().newDocument(documentName.c_str(), "testUser");
        fillet = document->addObject<PartDesign::Fillet>("Fillet");
    }

    void TearDown() override
    {
        if (document) {
            App::GetApplication().closeDocument(document->getName());
        }
        if (!savedPath.empty()) {
            std::error_code error;
            std::filesystem::remove(savedPath, error);
        }
    }

    std::string documentName;
    std::filesystem::path savedPath;
    App::Document* document = nullptr;
    PartDesign::Fillet* fillet = nullptr;
};

TEST_F(FeatureFilletTest, OpenEdgeProfileInterpolationMatchesKernelLaw)
{
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(30, 20, 16).Shape();
    const TopoDS_Edge edge = TopoDS::Edge(TopExp_Explorer(box, TopAbs_EDGE).Current());
    for (const Part::FilletRadiusLaw& points :
         {Part::FilletRadiusLaw {{0, 2}, {.3, 3}, {.72, 2.5}, {1, 2}},
          Part::FilletRadiusLaw {{0, 2}, {1, 3}},
          Part::FilletRadiusLaw {{0, 2}, {1, 2}}}) {
        TColgp_Array1OfPnt2d values(1, int(points.size()));
        for (size_t i = 0; i < points.size(); ++i) {
            values.SetValue(int(i + 1), gp_Pnt2d(points[i].position, points[i].radius));
        }
        BRepFilletAPI_MakeFillet builder(box);
        builder.Add(values, edge);
        builder.Build();
        ASSERT_TRUE(builder.IsDone());
        const bool constant = builder.IsConstant(builder.Contour(edge), edge);
        auto kernelLaw = constant ? Handle(Law_Function)()
                                  : builder.GetLaw(builder.Contour(edge), edge);
        const double length = builder.Length(builder.Contour(edge));
        std::vector<Part::FilletRadiusLaw> profiles;
        Part::TopoShape result;
        result.makeElementFillet(Part::TopoShape(box), {Part::TopoShape(edge)}, {points}, nullptr, &profiles);
        ASSERT_EQ(profiles.size(), 1);
        ASSERT_EQ(profiles.front().size(), 201);
        for (const auto& sample : profiles.front()) {
            const double expected = constant ? points.front().radius
                                             : kernelLaw->Value(sample.position * length);
            EXPECT_NEAR(sample.radius, expected, 1e-7);
        }
    }
}

TEST_F(FeatureFilletTest, RadiusLawRoundTripsThroughTypedAccessors)
{
    const Part::FilletRadiusLaw expected {
        {0.0, 0.5},
        {0.375, 1.25},
        {1.0, 0.75},
    };

    fillet->setRadiusLaw("Edge1", expected);
    const auto actual = fillet->getRadiusLaw("Edge1");

    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_DOUBLE_EQ(actual[i].position, expected[i].position);
        EXPECT_DOUBLE_EQ(actual[i].radius, expected[i].radius);
    }
}

TEST_F(FeatureFilletTest, LiteralDistanceAndPercentageKeepDifferentDesignIntent)
{
    fillet->setRadiusLaw("Edge1", {{0, 1}, {.4, 2}, {1, 1}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    fillet->setRadiusControlPointPosition("Edge1", "cp1", .4, 24, true);
    EXPECT_TRUE(fillet->isRadiusControlPointAbsolute("Edge1", "cp1"));
    auto law = fillet->getRadiusLaw("Edge1", 30);
    ASSERT_EQ(law.size(), 3);
    EXPECT_NEAR(law[1].position, 9.6 / 30, 1e-12);
    fillet->setRadiusControlPointPosition("Edge1", "cp1", .4, 24, false);
    law = fillet->getRadiusLaw("Edge1", 30);
    ASSERT_EQ(law.size(), 3);
    EXPECT_DOUBLE_EQ(law[1].position, .4);
}

TEST_F(FeatureFilletTest, AbsolutePositionPersistsAndIsRemovedWithItsPoint)
{
    fillet->setRadiusLaw("Edge1", {{0, 1}, {.4, 2}, {1, 1}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    fillet->setRadiusControlPointPosition("Edge1", "cp1", .4, 24, true);
    savedPath = std::filesystem::temp_directory_path() / (documentName + ".FCStd");
    ASSERT_TRUE(document->saveAs(savedPath.string().c_str()));
    App::GetApplication().closeDocument(document->getName());
    document = nullptr;
    document = App::GetApplication().openDocument(savedPath.string().c_str());
    ASSERT_NE(document, nullptr);
    fillet = dynamic_cast<PartDesign::Fillet*>(document->getObject("Fillet"));
    ASSERT_NE(fillet, nullptr);
    EXPECT_TRUE(fillet->isRadiusControlPointAbsolute("Edge1", "cp1"));
    auto law = fillet->getRadiusLaw("Edge1", 30);
    ASSERT_EQ(law.size(), 3);
    EXPECT_NEAR(law[1].position, .32, 1e-12);
    fillet->setRadiusControlPointIds("Edge1", {});
    EXPECT_FALSE(fillet->isRadiusControlPointAbsolute("Edge1", "cp1"));
}

TEST_F(FeatureFilletTest, OutOfRangeAbsoluteDistanceIsNotSilentlyClamped)
{
    fillet->setRadiusLaw("Edge1", {{0, 1}, {.4, 2}, {1, 1}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    fillet->setRadiusControlPointPosition("Edge1", "cp1", .4, 24, true);
    const auto law = fillet->getRadiusLaw("Edge1", 5);
    ASSERT_EQ(law.size(), 3);
    EXPECT_GT(law[1].position, 1);
    EXPECT_TRUE(fillet->getRadiusLaw("Edge1").empty());
}

TEST_F(FeatureFilletTest, ConstantModeIgnoresVariableDataAndKeepsSourceUnchanged)
{
    auto* body = document->addObject<PartDesign::Body>("Body");
    auto* base = document->addObject<PartDesign::Feature>("BaseFeature");
    body->addObject(base);
    base->Shape.setValue(BRepPrimAPI_MakeBox(10, 10, 10).Shape());
    body->addObject(fillet);
    fillet->Base.setValue(base, {"Edge1"});
    fillet->Radius.setValue(1);
    ASSERT_EQ(fillet->RadiusMode.getValue(), 0);
    document->recompute();
    ASSERT_FALSE(fillet->isError());
    const Part::TopoShape original = fillet->Shape.getShape();
    GProp_GProps before, after, source;
    BRepGProp::VolumeProperties(original.getShape(), before);
    fillet->VariableRadiusData.setValue("Edge1", "deliberately invalid variable law");
    fillet->setRadiusControlPointPosition("Edge1", "cp1", .9, 1000, true);
    document->recompute();
    ASSERT_FALSE(fillet->isError());
    BRepGProp::VolumeProperties(fillet->Shape.getShape().getShape(), after);
    BRepGProp::VolumeProperties(base->Shape.getShape().getShape(), source);
    EXPECT_NEAR(before.Mass(), after.Mass(), 1e-9);
    EXPECT_NEAR(source.Mass(), 1000, 1e-9);
    EXPECT_EQ(
        original.countSubShapes(TopAbs_FACE),
        fillet->Shape.getShape().countSubShapes(TopAbs_FACE)
    );
}

TEST_F(FeatureFilletTest, ControlPointValuesSupportExpressions)
{
    fillet->setRadiusLaw("Edge1", {{0.0, 0.5}, {0.25, 1.0}, {1.0, 0.75}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    const auto positionPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Position,
        0.25
    );
    const auto radiusPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Radius,
        1.0
    );

    fillet->setExpression(positionPath, App::ExpressionParser::parse(fillet, "1 / 3"));
    fillet->setExpression(radiusPath, App::ExpressionParser::parse(fillet, "2.5 mm"));
    document->recompute();

    const auto law = fillet->getRadiusLaw("Edge1");
    ASSERT_EQ(law.size(), 3);
    EXPECT_DOUBLE_EQ(law[1].position, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(law[1].radius, 2.5);

    fillet->setRadiusControlPointIds("Edge1", {});
    EXPECT_EQ(fillet->getExpression(positionPath).expression, nullptr);
    EXPECT_EQ(fillet->getExpression(radiusPath).expression, nullptr);
}

TEST_F(FeatureFilletTest, EndpointAndLengthValuesSupportExpressions)
{
    fillet->setRadiusLaw("Edge1", {{0.0, 0.5}, {0.25, 1.0}, {1.0, 0.75}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    const auto startPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "start",
        PartDesign::Fillet::ControlPointComponent::Radius,
        0.5
    );
    const auto endPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "end",
        PartDesign::Fillet::ControlPointComponent::Radius,
        0.75
    );
    const auto lengthPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Length,
        2.5
    );

    fillet->setExpression(startPath, App::ExpressionParser::parse(fillet, "1.5 mm"));
    fillet->setExpression(endPath, App::ExpressionParser::parse(fillet, "2 mm"));
    fillet->setExpression(lengthPath, App::ExpressionParser::parse(fillet, "5 mm"));
    document->recompute();

    const auto law = fillet->getRadiusLaw("Edge1", 20.0);
    ASSERT_EQ(law.size(), 3);
    EXPECT_DOUBLE_EQ(law.front().radius, 1.5);
    EXPECT_DOUBLE_EQ(law[1].position, 0.25);
    EXPECT_DOUBLE_EQ(law.back().radius, 2.0);
    EXPECT_TRUE(fillet->getRadiusLaw("Edge1").empty());

    fillet->setRadiusControlPointIds("Edge1", {});
    EXPECT_EQ(fillet->getExpression(lengthPath).expression, nullptr);
    fillet->clearRadiusControlPoints("Edge1");
    EXPECT_EQ(fillet->getExpression(startPath).expression, nullptr);
    EXPECT_EQ(fillet->getExpression(endPath).expression, nullptr);
}

TEST_F(FeatureFilletTest, EndpointAndLengthExpressionsPersistAcrossDocumentSaveAndRestore)
{
    fillet->setRadiusLaw("Edge1", {{0.0, 0.5}, {0.25, 1.0}, {1.0, 0.75}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    const auto startPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "start",
        PartDesign::Fillet::ControlPointComponent::Radius,
        0.5
    );
    const auto lengthPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Length,
        2.5
    );
    fillet->setExpression(startPath, App::ExpressionParser::parse(fillet, "1.5 mm"));
    fillet->setExpression(lengthPath, App::ExpressionParser::parse(fillet, "5 mm"));

    savedPath = std::filesystem::temp_directory_path() / (documentName + ".FCStd");
    ASSERT_TRUE(document->saveAs(savedPath.string().c_str()));
    App::GetApplication().closeDocument(document->getName());
    document = nullptr;
    fillet = nullptr;

    document = App::GetApplication().openDocument(savedPath.string().c_str());
    ASSERT_NE(document, nullptr);
    fillet = dynamic_cast<PartDesign::Fillet*>(document->getObject("Fillet"));
    ASSERT_NE(fillet, nullptr);

    const auto law = fillet->getRadiusLaw("Edge1", 20.0);
    ASSERT_EQ(law.size(), 3);
    EXPECT_DOUBLE_EQ(law.front().radius, 1.5);
    EXPECT_DOUBLE_EQ(law[1].position, 0.25);
}

TEST_F(FeatureFilletTest, ControlPointExpressionsPersistAcrossDocumentSaveAndRestore)
{
    fillet->setRadiusLaw("Edge1", {{0.0, 0.5}, {0.25, 1.0}, {1.0, 0.75}});
    fillet->setRadiusControlPointIds("Edge1", {"cp1"});
    const auto radiusPath = fillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Radius,
        1.0
    );
    fillet->setExpression(radiusPath, App::ExpressionParser::parse(fillet, "2.5 mm"));

    savedPath = std::filesystem::temp_directory_path() / (documentName + ".FCStd");
    ASSERT_TRUE(document->saveAs(savedPath.string().c_str()));
    App::GetApplication().closeDocument(document->getName());
    document = nullptr;
    fillet = nullptr;

    document = App::GetApplication().openDocument(savedPath.string().c_str());
    ASSERT_NE(document, nullptr);
    fillet = dynamic_cast<PartDesign::Fillet*>(document->getObject("Fillet"));
    ASSERT_NE(fillet, nullptr);

    const auto law = fillet->getRadiusLaw("Edge1");
    ASSERT_EQ(law.size(), 3);
    EXPECT_DOUBLE_EQ(law[1].radius, 2.5);
}

TEST_F(FeatureFilletTest, RadiusLawPersistsAcrossDocumentSaveAndRestore)
{
    const Part::FilletRadiusLaw expected {
        {0.0, 0.4},
        {0.25, 1.0},
        {1.0, 0.8},
    };
    fillet->RadiusMode.setValue(static_cast<int>(PartDesign::Fillet::RadiusModeValue::Variable));
    fillet->setRadiusLaw("Edge1", expected);

    savedPath = std::filesystem::temp_directory_path() / (documentName + ".FCStd");
    ASSERT_TRUE(document->saveAs(savedPath.string().c_str()));
    App::GetApplication().closeDocument(document->getName());
    document = nullptr;
    fillet = nullptr;

    document = App::GetApplication().openDocument(savedPath.string().c_str());
    ASSERT_NE(document, nullptr);
    fillet = dynamic_cast<PartDesign::Fillet*>(document->getObject("Fillet"));
    ASSERT_NE(fillet, nullptr);
    EXPECT_EQ(
        fillet->RadiusMode.getValue(),
        static_cast<int>(PartDesign::Fillet::RadiusModeValue::Variable)
    );

    const auto actual = fillet->getRadiusLaw("Edge1");
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_DOUBLE_EQ(actual[i].position, expected[i].position);
        EXPECT_DOUBLE_EQ(actual[i].radius, expected[i].radius);
    }
}

TEST_F(FeatureFilletTest, InvalidStoredRadiusLawReturnsEmptyLaw)
{
    fillet->VariableRadiusData.setValue("Edge1", "0,1;invalid;1,1");
    EXPECT_TRUE(fillet->getRadiusLaw("Edge1").empty());

    fillet->VariableRadiusData.setValue("Edge1", "0,1;0.5,nan;1,1");
    EXPECT_TRUE(fillet->getRadiusLaw("Edge1").empty());
}

TEST_F(FeatureFilletTest, EqualEndpointVariableLawModeSurvivesSaveRestore)
{
    fillet->setRadiusLaw("Edge2", {{0, 2}, {1, 2}});
    EXPECT_FALSE(fillet->isVariableRadiusLaw("Edge2"));
    fillet->RadiusLawModes.setValue("Edge2", "Variable");
    EXPECT_TRUE(fillet->isVariableRadiusLaw("Edge2"));
    savedPath = std::filesystem::temp_directory_path() / (documentName + ".FCStd");
    document->saveAs(savedPath.string().c_str());
    document->restore();
    auto* restored = dynamic_cast<PartDesign::Fillet*>(document->getObject("Fillet"));
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->isVariableRadiusLaw("Edge2"));
}

TEST_F(FeatureFilletTest, PerEdgeConstantLawUsesSingleExpression)
{
    fillet->setRadiusLaw("Edge2", {{0, 2}, {1, 2}});
    fillet->RadiusLawModes.setValue("Edge2", "Constant");
    const auto path = fillet->ensureRadiusControlPointValue(
        "Edge2",
        "start",
        PartDesign::Fillet::ControlPointComponent::Radius,
        2
    );
    fillet->setExpression(path, App::ExpressionParser::parse(fillet, "3 mm"));
    const auto law = fillet->getRadiusLaw("Edge2");
    ASSERT_EQ(law.size(), 2);
    EXPECT_DOUBLE_EQ(law.front().radius, 3);
    EXPECT_DOUBLE_EQ(law.back().radius, 3);
}

TEST_F(FeatureFilletTest, FaceGroupsDeduplicateAndAllowIndependentBoundaryLaws)
{
    auto* body = document->addObject<PartDesign::Body>("Body");
    auto* base = document->addObject<PartDesign::Feature>("BaseFeature");
    body->addObject(base);
    body->addObject(fillet);
    base->Shape.setValue(BRepPrimAPI_MakeBox(30, 20, 16).Shape());
    fillet->Base.setValue(base, {"Face1"});
    auto edges = fillet->getRadiusEdges();
    ASSERT_EQ(edges.size(), 4);
    const auto first = edges.front().first;
    fillet->Base.setValue(base, {"Face1", first});
    EXPECT_EQ(fillet->getRadiusEdges().size(), 4);
    fillet->RadiusMode.setValue(1L);
    fillet->setRadiusLaw(first, {{0, .5}, {.5, .7}, {1, .5}});
    fillet->setRadiusControlPointIds(first, {"cp1"});
    document->recompute();
    EXPECT_FALSE(fillet->isError());
    EXPECT_TRUE(fillet->Shape.getShape().hasSubShape(TopAbs_SOLID));
}

TEST_F(FeatureFilletTest, ExecutesVariableRadiusFillet)
{
    auto* body = document->addObject<PartDesign::Body>("Body");
    auto* base = document->addObject<PartDesign::Feature>("BaseFeature");
    body->addObject(base);
    base->Shape.setValue(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());

    auto* variableFillet = document->addObject<PartDesign::Fillet>("VariableFillet");
    body->addObject(variableFillet);
    variableFillet->Base.setValue(base, {"Edge1"});
    variableFillet->RadiusMode.setValue(
        static_cast<int>(PartDesign::Fillet::RadiusModeValue::Variable)
    );
    variableFillet->setRadiusLaw("Edge1", {{0.0, 0.4}, {0.5, 1.0}, {1.0, 0.8}});
    variableFillet->setRadiusControlPointIds("Edge1", {"cp1"});
    const auto startPath = variableFillet->ensureRadiusControlPointValue(
        "Edge1",
        "start",
        PartDesign::Fillet::ControlPointComponent::Radius,
        0.4
    );
    const auto endPath = variableFillet->ensureRadiusControlPointValue(
        "Edge1",
        "end",
        PartDesign::Fillet::ControlPointComponent::Radius,
        0.8
    );
    const auto lengthPath = variableFillet->ensureRadiusControlPointValue(
        "Edge1",
        "cp1",
        PartDesign::Fillet::ControlPointComponent::Length,
        5.0
    );
    variableFillet->setExpression(startPath, App::ExpressionParser::parse(variableFillet, "0.4 mm"));
    variableFillet->setExpression(endPath, App::ExpressionParser::parse(variableFillet, "0.8 mm"));
    variableFillet->setExpression(lengthPath, App::ExpressionParser::parse(variableFillet, "5 mm"));

    document->recompute();

    EXPECT_FALSE(variableFillet->isError());
    EXPECT_FALSE(variableFillet->Shape.getShape().isNull());
    EXPECT_TRUE(variableFillet->Shape.getShape().hasSubShape(TopAbs_SOLID));
}
