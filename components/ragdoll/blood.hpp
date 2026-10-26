#ifndef OPENMW_COMPONENTS_RAGDOLL_BLOOD_HPP
#define OPENMW_COMPONENTS_RAGDOLL_BLOOD_HPP

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace Ragdoll
{
    // The setting is a multiplier of the new blood baseline: 100% emits
    // twice as much as the original baseline, and 300% emits six times as much.
    // Pool formation keeps the stable preview.2 multiplier separately.
    inline float bloodAmountMultiplier(float amount)
    {
        return std::isfinite(amount) ? std::clamp(amount, 0.f, 3.f) : 1.f;
    }

    inline float bloodEmissionScale(float amount)
    {
        return 2.f * bloodAmountMultiplier(amount);
    }

    // Raising the quantity also increases the physical footprint of a spray.
    // At 100% the launch/cone multipliers are one; at 300% they are 1.8/1.6.
    inline float bloodLaunchScale(float amount)
    {
        return .6f + .4f * bloodAmountMultiplier(amount);
    }

    inline float bloodSpreadScale(float amount)
    {
        return .7f + .3f * bloodAmountMultiplier(amount);
    }

    // TES3 has no mechanical/no-blood record flag. Use canonical record and
    // mesh identifiers (including the original of a generated creature), never
    // localized display names. Flesh-bearing undead are intentionally allowed.
    inline bool creatureHasBlood(std::string_view id, std::string_view originalId, std::string_view model)
    {
        constexpr std::array families = {
            std::string_view("centurion"), std::string_view("centurio"), std::string_view("steamcent"),
            std::string_view("steamcenturion"), std::string_view("spherecenturion"), std::string_view("sphercenturion"),
            std::string_view("spidercenturion"), std::string_view("dwarvenspider"), std::string_view("dwarvensphere"),
            std::string_view("robot"), std::string_view("automaton"),
            std::string_view("fabricant"), std::string_view("hulkfabricant"), std::string_view("verminousfabricant"),
            std::string_view("skeleton"), std::string_view("skeletal"), std::string_view("bonelord"),
            std::string_view("bonewolf"), std::string_view("lich"), std::string_view("ghost"),
            std::string_view("ancestorghost"), std::string_view("ancestralghost"), std::string_view("wraith"),
            std::string_view("spectre"), std::string_view("specter"), std::string_view("wisp"),
            std::string_view("atronach"), std::string_view("atronachfire"), std::string_view("atronachflame"),
            std::string_view("atronachfrost"), std::string_view("atronachstorm")
        };
        for (const std::string_view value : { id, originalId, model })
        {
            std::string token;
            std::string previous;
            const auto excluded = [&]() {
                std::string_view stem(token);
                while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back())))
                    stem.remove_suffix(1);
                if ((previous == "dwarven" || previous == "dwemer")
                    && (stem == "spider" || stem == "sphere" || stem == "robot"))
                    return true;
                if (previous == "bone" && (stem == "lord" || stem == "wolf"))
                    return true;
                for (const auto family : families)
                    if (token.compare(0, family.size(), family) == 0)
                    {
                        const std::string_view suffix(token.data() + family.size(), token.size() - family.size());
                        if (suffix.empty())
                            return true;
                        bool digits = true;
                        for (const unsigned char c : suffix)
                            digits &= std::isdigit(c) != 0;
                        if (digits)
                            return true;
                    }
                return false;
            };
            for (const unsigned char c : value)
            {
                if (std::isalnum(c))
                    token.push_back(static_cast<char>(std::tolower(c)));
                else
                {
                    if (excluded())
                        return false;
                    previous = token;
                    token.clear();
                }
            }
            if (excluded())
                return false;
        }
        return true;
    }

    // A loaded corpse never starts a new fountain. A newly observed death
    // fires once, and resurrection arms the next life without accumulating IDs.
    struct DeathBloodTrigger
    {
        bool mObservedAlive = false;

        bool update(bool dead)
        {
            if (!dead)
            {
                mObservedAlive = true;
                return false;
            }
            const bool freshDeath = mObservedAlive;
            mObservedAlive = false;
            return freshDeath;
        }
    };
}

#endif
