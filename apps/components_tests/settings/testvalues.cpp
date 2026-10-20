#include "components/misc/strings/conversion.hpp"
#include "components/settings/parser.hpp"
#include "components/settings/values.hpp"

#include <MyGUI_XmlDocument.h>
#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <set>
#include <unicode/msgfmt.h>
#include <yaml-cpp/yaml.h>

#ifndef OPENMW_PROJECT_SOURCE_DIR
#define OPENMW_PROJECT_SOURCE_DIR "."
#endif

namespace Settings
{
    namespace
    {
        using namespace testing;

        struct SettingsValuesTest : Test
        {
            const std::filesystem::path mSettingsDefaultPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR }
                / "files" / Misc::StringUtils::stringToU8String("settings-default.cfg");

            SettingsValuesTest()
            {
                Manager::mDefaultSettings.clear();
                Manager::mUserSettings.clear();
                Manager::mChangedSettings.clear();
                SettingsFileParser parser;
                parser.loadSettingsFile(mSettingsDefaultPath, Manager::mDefaultSettings);
            }
        };

        TEST_F(SettingsValuesTest, shouldLoadFromSettingsManager)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 60);
        }

        TEST_F(SettingsValuesTest, shouldFillIndexOnLoad)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(index.get<float>("Camera", "field of view").get(), 60);
        }

        TEST_F(SettingsValuesTest, constructorShouldThrowExceptionOnMissingSetting)
        {
            Manager::mDefaultSettings.erase({ "Camera", "field of view" });
            Index index;
            EXPECT_THROW([&] { Values values(index); }(), std::runtime_error);
        }

        TEST_F(SettingsValuesTest, constructorShouldSanitize)
        {
            Manager::mUserSettings[std::make_pair("Camera", "field of view")] = "-1";
            Index index;
            Values values(index);
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 1);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldDoLookup)
        {
            Manager::mUserSettings[std::make_pair("category", "value")] = "13";
            Index index;
            SettingValue<int> value{ index, "category", "value", 42 };
            EXPECT_EQ(value.get(), 13);
            value.reset();
            EXPECT_EQ(value.get(), 42);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldSanitize)
        {
            Manager::mUserSettings[std::make_pair("category", "value")] = "2";
            Index index;
            SettingValue<int> value{ index, "category", "value", -1, Settings::makeClampSanitizerInt(0, 1) };
            EXPECT_EQ(value.get(), 1);
            value.reset();
            EXPECT_EQ(value.get(), 0);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldFallbackToDefault)
        {
            Index index;
            const SettingValue<int> value{ index, "category", "value", 42 };
            EXPECT_EQ(value.get(), 42);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldSetDefaults)
        {
            Index index;
            Values defaultValues(index);
            Manager::mUserSettings.emplace(std::make_pair("Camera", "field of view"), "61");
            Values values(std::move(defaultValues));
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 61);
            values.mCamera.mFieldOfView.reset();
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 60);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldSanitize)
        {
            Index index;
            Values defaultValues(index);
            Manager::mUserSettings[std::make_pair("Camera", "field of view")] = "-1";
            Values values(std::move(defaultValues));
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 1);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldThrowOnMissingSetting)
        {
            Index index;
            SettingValue<int> defaultValue{ index, "category", "value", 42 };
            EXPECT_THROW([&] { SettingValue<int> value(std::move(defaultValue)); }(), std::runtime_error);
        }

        TEST_F(SettingsValuesTest, findShouldThrowExceptionOnTypeMismatch)
        {
            Index index;
            Values values(index);
            EXPECT_THROW(index.find<int>("Camera", "field of view"), std::invalid_argument);
        }

        TEST_F(SettingsValuesTest, findShouldReturnNullptrForAbsentSetting)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(index.find<int>("foo", "bar"), nullptr);
        }

        TEST_F(SettingsValuesTest, getShouldThrowExceptionForAbsentSetting)
        {
            Index index;
            Values values(index);
            EXPECT_THROW(index.get<int>("foo", "bar").get(), std::invalid_argument);
        }

        TEST_F(SettingsValuesTest, setShouldChangeManagerUserSettings)
        {
            Index index;
            Values values(index);
            values.mCamera.mFieldOfView.set(42);
            EXPECT_EQ(Manager::mUserSettings.at({ "Camera", "field of view" }), "42");
            EXPECT_THAT(Manager::mChangedSettings, ElementsAre(std::make_pair("Camera", "field of view")));
        }

        TEST_F(SettingsValuesTest, setShouldNotChangeManagerChangedSettingsForNoChange)
        {
            Index index;
            Values values(index);
            values.mCamera.mFieldOfView.set(values.mCamera.mFieldOfView.get());
            EXPECT_THAT(Manager::mChangedSettings, ElementsAre());
        }
        TEST_F(SettingsValuesTest, physicsMenuCanDoubleBudgetsAndResetEveryControl)
        {
            Index index;
            Values defaults(index);
            Values values(std::move(defaults));
            auto& physics = values.mRagdoll;
            for (auto* scale : { &physics.mMeleeImpulseScale, &physics.mCrossbowImpulseScale, &physics.mBowImpulseScale,
                     &physics.mThrownImpulseScale, &physics.mStatusDurationScale })
            {
                scale->set(2.f);
                EXPECT_FLOAT_EQ(scale->get(), 2.f);
                scale->set(.5f);
                EXPECT_FLOAT_EQ(scale->get(), .5f);
                scale->reset();
                EXPECT_FLOAT_EQ(scale->get(), 1.f);
            }
            physics.mMagicDistanceScale.set(2.f);
            physics.mMaxSimulationTime.set(20.f);
            physics.mItemSimulationTime.set(5.f);
            physics.mPinMaxDistance.set(5.f);
            physics.mMaxActive.set(200);
            physics.mMaxActiveItems.set(200);
            physics.mDismembermentChance.set(.6f);
            EXPECT_FLOAT_EQ(physics.mDismembermentChance.get(), .6f);
            physics.mDismembermentChance.set(.15f);
            EXPECT_FLOAT_EQ(physics.mDismembermentChance.get(), .15f);
            EXPECT_EQ(physics.mMaxActive.get(), 200);
            EXPECT_EQ(physics.mMaxActiveItems.get(), 200);
            physics.mMagicDistanceScale.reset();
            physics.mMaxSimulationTime.reset();
            physics.mItemSimulationTime.reset();
            physics.mPinMaxDistance.reset();
            physics.mMaxActive.reset();
            physics.mMaxActiveItems.reset();
            physics.mDismembermentChance.reset();
            EXPECT_FLOAT_EQ(physics.mMagicDistanceScale.get(), 1.f);
            EXPECT_FLOAT_EQ(physics.mMaxSimulationTime.get(), 10.f);
            EXPECT_FLOAT_EQ(physics.mItemSimulationTime.get(), 10.f);
            EXPECT_FLOAT_EQ(physics.mPinMaxDistance.get(), 2.5f);
            EXPECT_EQ(physics.mMaxActive.get(), 100);
            EXPECT_EQ(physics.mMaxActiveItems.get(), 100);
            EXPECT_FLOAT_EQ(physics.mDismembermentChance.get(), .3f);
            EXPECT_EQ(Manager::getFloat("bow impulse scale", "Ragdoll"), 1.f);
        }
        TEST_F(SettingsValuesTest, physicsLayoutBindsEverySliderToHalfAndDoubleItsActualDefault)
        {
            const auto data = mSettingsDefaultPath.parent_path() / "data";
            MyGUI::xml::Document document;
            ASSERT_TRUE(document.open((data / "mygui/openmw_settings_window.layout").string()));
            Index index;
            Values values(index);
            const auto english = YAML::LoadFile((data / "l10n/OMWEngine/en.yaml").string());
            const auto russian = YAML::LoadFile((data / "l10n/OMWEngine/ru.yaml").string());
            std::set<std::string> seen;
            std::function<void(MyGUI::xml::Element*)> visit = [&](MyGUI::xml::Element* element) {
                std::map<std::string, std::string> metadata;
                auto child = element->getElementEnumerator();
                while (child.next())
                {
                    auto* value = child.current();
                    if (value->getName() == "UserString")
                        metadata[std::string(value->findAttribute("key"))] = value->findAttribute("value");
                }
                if (metadata["SettingCategory"] == "Ragdoll")
                {
                    const auto& name = metadata["SettingName"];
                    EXPECT_TRUE(seen.insert(name).second);
                    const bool integer = metadata["SettingValueType"] == "Integer";
                    double base;
                    if (integer)
                    {
                        ASSERT_NE(index.find<int>("Ragdoll", name), nullptr);
                        base = Manager::getInt(name, "Ragdoll");
                    }
                    else
                    {
                        ASSERT_NE(index.find<float>("Ragdoll", name), nullptr);
                        base = Manager::getFloat(name, "Ragdoll");
                    }
                    // Widgets and settings both store float boundaries. Decimal
                    // probabilities such as 0.3 do not have exact binary doubles.
                    EXPECT_FLOAT_EQ(std::stof(metadata["SettingMin"]), static_cast<float>(base * .5));
                    EXPECT_FLOAT_EQ(std::stof(metadata["SettingMax"]), static_cast<float>(base * 2));
                    for (const auto& [messages, locale] : { std::pair(english, "en"), std::pair(russian, "ru") })
                    {
                        const auto message = messages[metadata["SettingLabelCaption"]];
                        ASSERT_TRUE(message.IsScalar());
                        UErrorCode error = U_ZERO_ERROR;
                        icu::MessageFormat format(
                            icu::UnicodeString::fromUTF8(message.as<std::string>()), icu::Locale(locale), error);
                        EXPECT_TRUE(U_SUCCESS(error)) << u_errorName(error);
                    }
                }
                child = element->getElementEnumerator();
                while (child.next())
                    visit(child.current());
            };
            visit(document.getRoot());
            EXPECT_EQ(seen, (std::set<std::string>{ "melee impulse scale", "crossbow impulse scale",
                "bow impulse scale", "thrown impulse scale", "magic distance scale", "max simulation time",
                "item simulation time", "max active", "max active items", "pin max distance",
                "status duration scale", "dismemberment chance" }));
        }
    }
}
