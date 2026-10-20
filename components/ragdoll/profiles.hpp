#ifndef OPENMW_RAGDOLL_PROFILES_HPP
#define OPENMW_RAGDOLL_PROFILES_HPP

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace Ragdoll
{
    struct CreatureProfile
    {
        float mMass = 70;
        float mReferenceSize = 1.75f;
        bool mExcluded = false;
    };
    inline CreatureProfile creatureProfile(
        std::string id, std::string model, const std::vector<std::string>& overrides = {})
    {
        const auto lower = [](std::string& s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        lower(id);
        lower(model);
        const auto matches = [&](std::string_view key) {
            return id.find(key) != std::string::npos || model.find(key) != std::string::npos;
        };
        // Record/model identifiers, never translated display names.
        for (const auto key : { "centurion", "centurio", "steamcent", "dwarven_spider", "dwarven_sphere", "fabricant" })
            if (matches(key))
                return { 70, 1.75f, true };
        struct Entry
        {
            std::string_view mKey;
            float mMass, mSize;
        };
        static constexpr Entry entries[] = { { "ogrim", 500, 2.8f }, { "dremora", 110, 2.f },
            { "golden_saint", 100, 1.9f }, { "skeleton", 35, 1.75f }, { "bonelord", 120, 2.3f },
            { "bonewalker", 90, 1.8f }, { "bonewolf", 22, 1.1f }, { "rat", 3, .35f }, { "scrib", 2, .3f },
            { "kwama_forager", 2, .5f }, { "kwama_queen", 700, 3.5f }, { "kwama", 100, 1.5f }, { "guar", 250, 2.f },
            { "kagouti", 220, 2.f }, { "alit", 90, 1.5f }, { "nix", 55, 1.3f }, { "netch", 200, 3.f },
            { "cliffracer", 30, 2.5f }, { "slaughterfish", 15, 1.f }, { "dreugh", 90, 1.8f }, { "daedroth", 250, 2.5f },
            { "clannfear", 120, 1.8f }, { "hunger", 90, 2.f }, { "scamp", 30, 1.f }, { "shalk", 35, 1.f },
            { "sleeper", 160, 2.3f }, { "ash_ghoul", 100, 2.f }, { "corprus", 95, 1.8f }, { "bear", 300, 2.2f },
            { "wolf", 45, 1.4f }, { "horker", 400, 2.5f }, { "boar", 100, 1.3f }, { "grahl", 450, 2.8f },
            { "riekling", 30, 1.f }, { "goblin", 40, 1.2f }, { "durzog", 140, 1.8f }, { "draugr", 65, 1.8f },
            { "spriggan", 110, 2.f }, { "lich", 55, 1.9f }, { "winged", 100, 2.5f } };
        CreatureProfile result;
        for (const auto& entry : entries)
            if (matches(entry.mKey))
            {
                result = { entry.mMass, entry.mSize, false };
                break;
            }
        // selector:mass_kg:reference_size_metres, first matching override wins.
        for (const auto& overrideValue : overrides)
        {
            const std::string_view value(overrideValue);
            const auto first = value.find(':'), second = value.find(':', first == value.npos ? first : first + 1);
            if (first == value.npos || second == value.npos || !matches(value.substr(0, first)))
                continue;
            float mass = 0, size = 0;
            const auto m = std::from_chars(value.data() + first + 1, value.data() + second, mass);
            const auto h = std::from_chars(value.data() + second + 1, value.data() + value.size(), size);
            if (m.ec == std::errc{} && h.ec == std::errc{} && m.ptr == value.data() + second
                && h.ptr == value.data() + value.size() && std::isfinite(mass) && std::isfinite(size) && mass >= 0
                && mass <= 5000 && size > 0 && size < 100)
                return { mass, size, mass == 0 };
        }
        return result;
    }
}

#endif
