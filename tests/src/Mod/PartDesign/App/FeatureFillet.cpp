// SPDX-License-Identifier: LGPL-2.1-or-later

#include <filesystem>

#include <gtest/gtest.h>

#include "src/App/InitApplication.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <App/Application.h>
#include <App/Document.h>
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

TEST_F(FeatureFilletTest, RadiusLawPersistsAcrossDocumentSaveAndRestore)
{
    const Part::FilletRadiusLaw expected {
        {0.0, 0.4},
        {0.25, 1.0},
        {1.0, 0.8},
    };
    fillet->RadiusMode.setValue(
        static_cast<int>(PartDesign::Fillet::RadiusModeValue::Variable)
    );
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

    document->recompute();

    EXPECT_FALSE(variableFillet->isError());
    EXPECT_FALSE(variableFillet->Shape.getShape().isNull());
    EXPECT_TRUE(variableFillet->Shape.getShape().hasSubShape(TopAbs_SOLID));
}
