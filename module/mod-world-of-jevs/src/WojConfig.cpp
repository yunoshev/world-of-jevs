#include "WojConfig.h"
#include "Config.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
    // N4: set_max_timeout(0) (WojBridge.cpp, C3) is exactly the "no overall
    // budget" branch the C3 fix exists to get away from, so 0 cannot be a
    // legal TimeoutMs. At the other end, httplib's poll() wait reads a
    // timeout that overflows time_t/int arithmetic as negative, and a
    // negative timeout means "wait forever" (httplib.h:6449) - the same
    // failure mode from the opposite direction. Both ends are clamped away
    // here rather than trusted to never be configured.
    constexpr uint32_t MIN_TIMEOUT_MS = 50;
    constexpr uint32_t MAX_TIMEOUT_MS = 60000;
    constexpr uint32_t MIN_FRESHNESS_MS = 100;
    constexpr uint32_t MAX_FRESHNESS_MS = 30000;
    // SmartAI encounter timers can legitimately exceed ten minutes (the
    // Deadmines Defias Evoker repeats Frost Armor after twenty). Keep the
    // wire/parser guard finite while preserving those original cadences.
    constexpr uint32_t MAX_BEHAVIOR_TIMER_MS = 3600000;
    // Scenario actor identity is always checked individually. Keep a finite
    // ceiling while allowing a full static dungeon roster rather than forcing
    // required actors into the optional-assistant channel.
    constexpr std::streamoff MAX_BEHAVIOR_FILE_BYTES = 256 * 1024;

    uint32_t NextBindingGeneration(uint32_t current)
    {
        // Protocol forbids zero. Epoch is fenced on every handoff, so a
        // uint32 wrap cannot revive an old decision or cache record.
        return current == std::numeric_limits<uint32_t>::max() ? 1u : current + 1u;
    }

    std::string Trim(std::string const& s)
    {
        size_t const begin = s.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return "";
        size_t const end = s.find_last_not_of(" \t\r\n");
        return s.substr(begin, end - begin + 1);
    }

    // A guid token must be nothing but digits: no sign, no trailing junk, no
    // hex, no whitespace inside it. std::stoul happily parses "89965abc" as
    // 89965 and "-1" as 4294967295 (unsigned wraparound) — both look like a
    // real guid in the log and neither is one.
    bool IsDigitsOnly(std::string const& s)
    {
        return !s.empty() && std::all_of(s.begin(), s.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    void RequireKeys(json const& value, std::initializer_list<char const*> expected,
                     char const* context)
    {
        if (!value.is_object())
            throw std::runtime_error(std::string(context) + " must be an object");
        std::set<std::string> actual;
        for (auto const& [key, unused] : value.items())
        {
            (void)unused;
            actual.insert(key);
        }
        std::set<std::string> wanted;
        for (char const* key : expected)
            wanted.insert(key);
        if (actual != wanted)
            throw std::runtime_error(std::string(context) + " has unknown or missing fields");
    }

    void RequireKeysOptional(json const& value, std::initializer_list<char const*> required,
        std::initializer_list<char const*> optional, char const* context)
    {
        if (!value.is_object())
            throw std::runtime_error(std::string(context) + " must be an object");
        std::set<std::string> allowed;
        for (char const* key : required)
        {
            allowed.insert(key);
            if (!value.contains(key))
                throw std::runtime_error(std::string(context) + " has unknown or missing fields");
        }
        for (char const* key : optional)
            allowed.insert(key);
        for (auto const& [key, unused] : value.items())
        {
            (void)unused;
            if (!allowed.count(key))
                throw std::runtime_error(std::string(context) + " has unknown or missing fields");
        }
    }

    bool ValidBindingId(std::string const& value)
    {
        return !value.empty() && value.size() <= 32 && value.front() >= 'a' && value.front() <= 'z' &&
            std::all_of(value.begin(), value.end(),
                [](unsigned char c)
                {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                });
    }

    uint32_t ReadUInt(json const& value, char const* field, uint32_t minimum, uint32_t maximum);
    json ParseStrictJson(std::string const& text);

    float ReadFiniteCoordinate(json const& value, char const* field)
    {
        if (!value.is_number())
            throw std::runtime_error(std::string(field) + " must be a finite number");
        float const result = value.get<float>();
        if (!std::isfinite(result))
            throw std::runtime_error(std::string(field) + " must be a finite number");
        return result;
    }

    uint32_t ReadUInt(json const& value, char const* field, uint32_t minimum, uint32_t maximum);
    float ReadDistance(json const& value, char const* field);

    WojControl ReadControl(json const& value, char const* field, bool inheritAllowed)
    {
        if (!value.is_string())
            throw std::runtime_error(std::string(field) + " must be a control string");
        std::string const control = value.get<std::string>();
        if (control == "jev")
            return WojControl::Jev;
        if (control == "original")
            return WojControl::Original;
        if (inheritAllowed && control == "inherit")
            return WojControl::Inherit;
        throw std::runtime_error(std::string(field) + " is invalid");
    }

    WojFleeBehavior ReadFlee(json const& value)
    {
        RequireKeysOptional(value, {"max_health_pct", "once_per_epoch", "require_in_combat",
            "require_not_casting", "require_engaged", "max_distance_yards"}, {"enabled"}, "flee");
        WojFleeBehavior result;
        if (value.contains("enabled"))
        {
            if (!value.at("enabled").is_boolean())
                throw std::runtime_error("flee.enabled must be boolean");
            result.enabled = value.at("enabled").get<bool>();
        }
        result.maxHealthPct = ReadUInt(value.at("max_health_pct"), "flee.max_health_pct", 1, 100);
        if (!value.at("once_per_epoch").is_boolean() || !value.at("require_in_combat").is_boolean() ||
            !value.at("require_not_casting").is_boolean() || !value.at("require_engaged").is_boolean())
            throw std::runtime_error("flee predicate flags must be booleans");
        result.oncePerEpoch = value.at("once_per_epoch").get<bool>();
        result.requireInCombat = value.at("require_in_combat").get<bool>();
        result.requireNotCasting = value.at("require_not_casting").get<bool>();
        result.requireEngaged = value.at("require_engaged").get<bool>();
        result.maxDistanceYards = ReadDistance(value.at("max_distance_yards"), "flee.max_distance_yards");
        return result;
    }

    WojAggroTalkBehavior ReadAggroTalk(json const& value)
    {
        RequireKeys(value, {"enabled", "chance_pct", "text_group"}, "aggro_talk");
        if (!value.at("enabled").is_boolean())
            throw std::runtime_error("aggro_talk.enabled must be boolean");
        WojAggroTalkBehavior result;
        result.enabled = value.at("enabled").get<bool>();
        result.chancePct = ReadUInt(value.at("chance_pct"), "aggro_talk.chance_pct", 0, 100);
        result.textGroup = ReadUInt(value.at("text_group"), "aggro_talk.text_group", 0, 255);
        return result;
    }

    WojActivationBehavior ReadActivation(json const& value)
    {
        RequireKeys(value, {"mode", "radius_yards"}, "activation");
        if (!value.at("mode").is_string())
            throw std::runtime_error("activation.mode must be a string");
        std::string const mode = value.at("mode").get<std::string>();
        WojActivationBehavior result;
        if (mode == "always")
        {
            if (!value.at("radius_yards").is_null())
                throw std::runtime_error("always activation requires null radius_yards");
            return result;
        }
        if (mode != "near_player")
            throw std::runtime_error("activation.mode is invalid");
        result.mode = WojActivationMode::NearPlayer;
        result.radiusYards = ReadDistance(value.at("radius_yards"), "activation.radius_yards");
        if (result.radiusYards > 500.0f)
            throw std::runtime_error("activation.radius_yards exceeds 500 yards");
        return result;
    }

    uint32_t ReadUInt(json const& value, char const* field, uint32_t minimum, uint32_t maximum)
    {
        if (!value.is_number_unsigned())
            throw std::runtime_error(std::string(field) + " must be an unsigned integer");
        uint64_t const parsed = value.get<uint64_t>();
        if (parsed < minimum || parsed > maximum)
            throw std::runtime_error(std::string(field) + " is out of range");
        return static_cast<uint32_t>(parsed);
    }

    float ReadDistance(json const& value, char const* field)
    {
        if (!value.is_number())
            throw std::runtime_error(std::string(field) + " must be a number");
        double const parsed = value.get<double>();
        if (!std::isfinite(parsed) || parsed <= 0.0 || parsed > 100.0)
            throw std::runtime_error(std::string(field) + " is out of range");
        return static_cast<float>(parsed);
    }

    WojTimerRange ReadTimer(json const& value, char const* field)
    {
        if (!value.is_array() || value.size() != 2)
            throw std::runtime_error(std::string(field) + " must contain [minimum, maximum]");
        WojTimerRange range;
        range.minimum = ReadUInt(value[0], field, 0, MAX_BEHAVIOR_TIMER_MS);
        range.maximum = ReadUInt(value[1], field, 0, MAX_BEHAVIOR_TIMER_MS);
        if (range.minimum > range.maximum)
            throw std::runtime_error(std::string(field) + " minimum exceeds maximum");
        return range;
    }

    WojAmbientTalkBehavior ReadAmbientTalk(json const& value)
    {
        RequireKeys(value, {"enabled", "initial_ms", "repeat_ms", "chance_pct", "text_group"},
            "ambient_talk");
        if (!value.at("enabled").is_boolean())
            throw std::runtime_error("ambient_talk.enabled must be boolean");
        WojAmbientTalkBehavior result;
        result.enabled = value.at("enabled").get<bool>();
        result.initial = ReadTimer(value.at("initial_ms"), "ambient_talk.initial_ms");
        result.repeat = ReadTimer(value.at("repeat_ms"), "ambient_talk.repeat_ms");
        result.chancePct = ReadUInt(value.at("chance_pct"), "ambient_talk.chance_pct", 0, 100);
        result.textGroup = ReadUInt(value.at("text_group"), "ambient_talk.text_group", 0, 255);
        return result;
    }

    json ParseStrictJson(std::string const& text)
    {
        bool duplicate = false;
        std::vector<std::unordered_set<std::string>> objectKeys;
        json::parser_callback_t callback = [&](int /*depth*/, json::parse_event_t event, json& parsed)
        {
            if (event == json::parse_event_t::object_start)
                objectKeys.emplace_back();
            else if (event == json::parse_event_t::key &&
                     (objectKeys.empty() || !objectKeys.back().insert(parsed.get<std::string>()).second))
                duplicate = true;
            else if (event == json::parse_event_t::object_end && !objectKeys.empty())
                objectKeys.pop_back();
            return true;
        };
        json parsed = json::parse(text, callback, true, false);
        if (duplicate)
            throw std::runtime_error("duplicate JSON key");
        return parsed;
    }

    WojBehaviorPolicy ParseLegacyBehaviorPolicy(json const& root)
    {
        RequireKeys(root, {"schema_version", "revision", "spells", "flee"}, "root");
        WojBehaviorPolicy policy;
        policy.revision = ReadUInt(root.at("revision"), "revision", 1,
                                   std::numeric_limits<uint32_t>::max());
        policy.sceneId = "murlocs";
        json const& spells = root.at("spells");
        if (!spells.is_array() || spells.size() != 5)
            throw std::runtime_error("legacy spells must contain exactly five records");
        std::set<std::pair<uint32_t, uint32_t>> const expected = {
            {127, 11831}, {127, 744}, {391, 3584}, {517, 9734}, {517, 6074}
        };
        std::unordered_map<uint32_t, std::string> const expectedTargets = {
            {11831, "enemy"}, {744, "enemy"}, {3584, "enemy"},
            {9734, "enemy"}, {6074, "ally"}
        };
        std::set<std::pair<uint32_t, uint32_t>> actual;
        for (json const& value : spells)
        {
            RequireKeys(value, {"entry", "spell", "initial_ms", "repeat_ms",
                "ticks_out_of_combat", "chance_pct", "target", "require_caster_combat",
                "require_target_in_combat", "require_engaged", "max_distance_yards",
                "require_aura_absent", "max_target_health_pct"}, "legacy spell");
            WojSpellBehavior behavior;
            behavior.entry = ReadUInt(value.at("entry"), "entry", 1,
                                      std::numeric_limits<uint32_t>::max());
            behavior.spell = ReadUInt(value.at("spell"), "spell", 1,
                                      std::numeric_limits<uint32_t>::max());
            behavior.initial = ReadTimer(value.at("initial_ms"), "initial_ms");
            behavior.repeat = ReadTimer(value.at("repeat_ms"), "repeat_ms");
            if (!value.at("ticks_out_of_combat").is_boolean() ||
                !value.at("require_caster_combat").is_boolean() ||
                !value.at("require_target_in_combat").is_boolean() ||
                !value.at("require_engaged").is_boolean() ||
                !value.at("require_aura_absent").is_boolean())
                throw std::runtime_error("legacy spell predicate flags must be booleans");
            behavior.ticksOutOfCombat = value.at("ticks_out_of_combat").get<bool>();
            behavior.chancePct = ReadUInt(value.at("chance_pct"), "chance_pct", 1, 100);
            if (!value.at("target").is_string())
                throw std::runtime_error("legacy target must be enemy or ally");
            behavior.target = value.at("target").get<std::string>();
            if (expectedTargets.find(behavior.spell) == expectedTargets.end() ||
                behavior.target != expectedTargets.at(behavior.spell))
                throw std::runtime_error("legacy target disagrees with murloc catalog");
            behavior.requireCasterCombat = value.at("require_caster_combat").get<bool>();
            behavior.requireTargetInCombat = value.at("require_target_in_combat").get<bool>();
            behavior.requireEngaged = value.at("require_engaged").get<bool>();
            behavior.requireAuraAbsent = value.at("require_aura_absent").get<bool>();
            if (behavior.target == "enemy" && behavior.requireTargetInCombat)
                throw std::runtime_error("legacy enemy require_target_in_combat is unsupported by protocol 1.7");
            if (behavior.target == "ally" && behavior.requireEngaged)
                throw std::runtime_error("legacy ally require_engaged is unsupported by protocol 1.7");
            if (!value.at("max_distance_yards").is_null())
            {
                behavior.hasMaxDistance = true;
                behavior.maxDistanceYards = ReadDistance(value.at("max_distance_yards"), "max_distance_yards");
            }
            if (!value.at("max_target_health_pct").is_null())
            {
                behavior.hasMaxTargetHealthPct = true;
                behavior.maxTargetHealthPct = ReadUInt(value.at("max_target_health_pct"),
                                                        "max_target_health_pct", 1, 100);
            }
            if (!actual.emplace(behavior.entry, behavior.spell).second)
                throw std::runtime_error("duplicate legacy spell record");
            policy.spells.emplace(behavior.spell, std::move(behavior));
        }
        if (actual != expected)
            throw std::runtime_error("legacy spell catalog does not match murlocs");

        json const& flee = root.at("flee");
        RequireKeys(flee, {"max_health_pct", "once_per_epoch", "require_in_combat",
            "require_not_casting", "require_engaged", "max_distance_yards"}, "legacy flee");
        policy.flee.maxHealthPct = ReadUInt(flee.at("max_health_pct"), "flee.max_health_pct", 1, 100);
        if (!flee.at("once_per_epoch").is_boolean() || !flee.at("require_in_combat").is_boolean() ||
            !flee.at("require_not_casting").is_boolean() || !flee.at("require_engaged").is_boolean())
            throw std::runtime_error("legacy flee predicate flags must be booleans");
        policy.flee.oncePerEpoch = flee.at("once_per_epoch").get<bool>();
        policy.flee.requireInCombat = flee.at("require_in_combat").get<bool>();
        policy.flee.requireNotCasting = flee.at("require_not_casting").get<bool>();
        policy.flee.requireEngaged = flee.at("require_engaged").get<bool>();
        policy.flee.maxDistanceYards = ReadDistance(flee.at("max_distance_yards"),
                                                    "flee.max_distance_yards");
        policy.canonical = root.dump();
        return policy;
    }

    WojBehaviorPolicy ParseV3BehaviorPolicy(json const& root)
    {
        RequireKeys(root, {"schema_version", "revision", "groups"}, "root");
        WojBehaviorPolicy policy;
        policy.schemaVersion = 3;
        policy.revision = ReadUInt(root.at("revision"), "revision", 1,
            std::numeric_limits<uint32_t>::max());
        json const& groups = root.at("groups");
        if (!groups.is_array() || groups.empty() || groups.size() > 64)
            throw std::runtime_error("groups must contain 1..64 records");

        std::set<std::string> ids;
        for (json const& group : groups)
        {
            RequireKeysOptional(group, {"id", "enabled", "map_id", "control", "actors", "spells", "flee", "aggro_talk"},
                {"activation", "arena_anchor", "death_effects", "runtime_summons", "go_runtime_summons", "ambient_talk",
                 "health_phases", "health_events", "aggro_events"}, "group");
            if (!group.at("id").is_string())
                throw std::runtime_error("group.id must be a string");
            std::string const id = group.at("id").get<std::string>();
            if (!ValidBindingId(id) || !ids.insert(id).second)
                throw std::runtime_error("group.id is invalid or duplicated");
            if (!group.at("enabled").is_boolean())
                throw std::runtime_error("group.enabled must be boolean");
            uint32_t const mapId = ReadUInt(group.at("map_id"), "group.map_id", 0, 65535);
            WojControl const groupControl = ReadControl(group.at("control"), "group.control", false);
            bool const enabled = group.at("enabled").get<bool>();
            policy.groupControls.emplace(id, enabled ? groupControl : WojControl::Original);
            WojActivationBehavior const activation = group.contains("activation")
                ? ReadActivation(group.at("activation")) : WojActivationBehavior{};
            if (group.contains("arena_anchor"))
            {
                json const& anchor = group.at("arena_anchor");
                if (!anchor.is_null())
                {
                    RequireKeys(anchor, {"x", "y", "z"}, "arena_anchor");
                    if (!anchor.at("x").is_number() || !anchor.at("y").is_number() || !anchor.at("z").is_number())
                        throw std::runtime_error("arena_anchor coordinates must be numbers");
                    WojPoint point { anchor.at("x").get<float>(), anchor.at("y").get<float>(), anchor.at("z").get<float>(), 0.f };
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                        throw std::runtime_error("arena_anchor coordinates must be finite");
                    policy.arenaAnchors.emplace(id, point);
                }
            }
            WojFleeBehavior const flee = ReadFlee(group.at("flee"));
            WojAggroTalkBehavior const aggroTalk = ReadAggroTalk(group.at("aggro_talk"));
            WojAmbientTalkBehavior const ambientTalk = group.contains("ambient_talk")
                ? ReadAmbientTalk(group.at("ambient_talk")) : WojAmbientTalkBehavior{};
            json const& actors = group.at("actors");
            if (!actors.is_array() || actors.empty() || actors.size() > 128)
                throw std::runtime_error("group.actors must contain 1..128 records");
            std::vector<uint32_t> actorSpawns;
            for (json const& actor : actors)
            {
                RequireKeysOptional(actor, {"spawn_id", "entry", "control", "aggro_talk"},
                    {"preserve_stock_routine", "ambient_talk", "flee_enabled", "ai_init_despawn",
                     "preferred_combat_range_yards", "executor_repeat_continuation_spell"}, "actor");
                if (!actor.at("aggro_talk").is_boolean())
                    throw std::runtime_error("actor.aggro_talk must be boolean");
                if (actor.contains("ambient_talk") && !actor.at("ambient_talk").is_boolean())
                    throw std::runtime_error("actor.ambient_talk must be boolean");
                if (actor.contains("flee_enabled") && !actor.at("flee_enabled").is_boolean())
                    throw std::runtime_error("actor.flee_enabled must be boolean");
                WojActorBinding binding;
                binding.spawnId = ReadUInt(actor.at("spawn_id"), "actor.spawn_id", 1,
                    std::numeric_limits<uint32_t>::max());
                binding.entry = ReadUInt(actor.at("entry"), "actor.entry", 1,
                    std::numeric_limits<uint32_t>::max());
                binding.mapId = mapId;
                binding.groupId = id;
                binding.control = ReadControl(actor.at("control"), "actor.control", true);
                binding.groupControl = enabled ? groupControl : WojControl::Original;
                binding.groupEnabled = enabled;
                binding.aggroTalk = actor.at("aggro_talk").get<bool>();
                binding.ambientTalk = actor.value("ambient_talk", false);
                binding.hasFleeEnabledOverride = actor.contains("flee_enabled");
                binding.fleeEnabledOverride = actor.value("flee_enabled", false);
                if (actor.contains("preferred_combat_range_yards"))
                {
                    binding.hasPreferredCombatRange = true;
                    binding.preferredCombatRangeYards = ReadDistance(
                        actor.at("preferred_combat_range_yards"), "actor.preferred_combat_range_yards");
                }
                if (actor.contains("executor_repeat_continuation_spell"))
                {
                    // Deliberately no generic fast-path: this permit exists
                    // solely to continue the source-derived Taskmaster Shoot
                    // observation without turning executor continuation into
                    // a second decision mechanism.
                    uint32_t const spell = ReadUInt(actor.at("executor_repeat_continuation_spell"),
                        "actor.executor_repeat_continuation_spell", 1,
                        std::numeric_limits<uint32_t>::max());
                    if (binding.spawnId != 79230 || binding.entry != 4417 || spell != 6660)
                        throw std::runtime_error("executor repeat continuation is reserved for Taskmaster 79230 Shoot 6660");
                    binding.hasExecutorRepeatContinuationSpell = true;
                    binding.executorRepeatContinuationSpell = spell;
                }
                if (actor.contains("ai_init_despawn"))
                {
                    json const& lifecycle = actor.at("ai_init_despawn");
                    RequireKeys(lifecycle, {"chance_pct", "delay_ms"}, "actor.ai_init_despawn");
                    binding.hasAiInitDespawn = true;
                    binding.aiInitDespawn.chancePct = ReadUInt(lifecycle.at("chance_pct"),
                        "actor.ai_init_despawn.chance_pct", 0, 100);
                    binding.aiInitDespawn.delayMs = ReadUInt(lifecycle.at("delay_ms"),
                        "actor.ai_init_despawn.delay_ms", 1, MAX_BEHAVIOR_TIMER_MS);
                }
                if (actor.contains("preserve_stock_routine"))
                {
                    if (!actor.at("preserve_stock_routine").is_boolean())
                        throw std::runtime_error("actor.preserve_stock_routine must be boolean");
                    binding.preserveStockRoutine = actor.at("preserve_stock_routine").get<bool>();
                }
                binding.activation = activation;
                if (!policy.actors.emplace(binding.spawnId, binding).second)
                    throw std::runtime_error("actor.spawn_id is duplicated (world-map registry is spawn keyed)");
                actorSpawns.push_back(binding.spawnId);
            }
            if (group.contains("runtime_summons"))
            {
                json const& summons = group.at("runtime_summons");
                if (!summons.is_array() || summons.size() > 32)
                    throw std::runtime_error("group.runtime_summons must contain 0..32 records");
                std::map<std::pair<uint32_t, uint32_t>, uint32_t> runtimeEntries;
                std::map<std::pair<uint32_t, uint32_t>, uint32_t> runtimeSpells;
                std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> runtimeIndices;
                for (json const& actor : summons)
                {
                    RequireKeysOptional(actor, {"actor_id", "entry", "source_actor_id", "created_by_spell",
                        "control", "aggro_talk"}, {"preserve_stock_routine", "ambient_talk", "flee_enabled",
                        "summon_index"}, "runtime_summon");
                    if (!actor.at("aggro_talk").is_boolean())
                        throw std::runtime_error("runtime_summon.aggro_talk must be boolean");
                    if (actor.contains("ambient_talk") && !actor.at("ambient_talk").is_boolean())
                        throw std::runtime_error("runtime_summon.ambient_talk must be boolean");
                    if (actor.contains("flee_enabled") && !actor.at("flee_enabled").is_boolean())
                        throw std::runtime_error("runtime_summon.flee_enabled must be boolean");
                    WojActorBinding binding;
                    binding.spawnId = ReadUInt(actor.at("actor_id"), "runtime_summon.actor_id",
                        0x80000000u, std::numeric_limits<uint32_t>::max());
                    binding.entry = ReadUInt(actor.at("entry"), "runtime_summon.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    binding.sourceActorId = ReadUInt(actor.at("source_actor_id"),
                        "runtime_summon.source_actor_id", 1, std::numeric_limits<uint32_t>::max());
                    binding.createdBySpell = ReadUInt(actor.at("created_by_spell"),
                        "runtime_summon.created_by_spell", 1, std::numeric_limits<uint32_t>::max());
                    if (actor.contains("summon_index"))
                        binding.summonIndex = ReadUInt(actor.at("summon_index"),
                            "runtime_summon.summon_index", 1, 8);
                    binding.mapId = mapId;
                    binding.runtimeSummon = true;
                    binding.groupId = id;
                    binding.control = ReadControl(actor.at("control"), "runtime_summon.control", true);
                    binding.groupControl = enabled ? groupControl : WojControl::Original;
                    binding.groupEnabled = enabled;
                    binding.aggroTalk = actor.at("aggro_talk").get<bool>();
                    binding.ambientTalk = actor.value("ambient_talk", false);
                    binding.hasFleeEnabledOverride = actor.contains("flee_enabled");
                    binding.fleeEnabledOverride = actor.value("flee_enabled", false);
                    if (actor.contains("preserve_stock_routine"))
                    {
                        if (!actor.at("preserve_stock_routine").is_boolean())
                            throw std::runtime_error("runtime_summon.preserve_stock_routine must be boolean");
                        binding.preserveStockRoutine = actor.at("preserve_stock_routine").get<bool>();
                    }
                    binding.activation = activation;
                    auto const entryKey = std::make_pair(binding.sourceActorId, binding.entry);
                    auto const spellKey = std::make_pair(binding.sourceActorId, binding.createdBySpell);
                    if (auto const it = runtimeEntries.find(entryKey);
                        it != runtimeEntries.end() && it->second != binding.createdBySpell)
                        throw std::runtime_error("runtime_summon source/entry selector is ambiguous");
                    if (auto const it = runtimeSpells.find(spellKey);
                        it != runtimeSpells.end() && it->second != binding.entry)
                        throw std::runtime_error("runtime_summon source/spell selector is ambiguous");
                    runtimeEntries.emplace(entryKey, binding.createdBySpell);
                    runtimeSpells.emplace(spellKey, binding.entry);
                    if (!runtimeIndices[spellKey].insert(binding.summonIndex).second)
                        throw std::runtime_error("runtime_summon summon_index is duplicated within one cast");
                    auto const source = policy.actors.find(binding.sourceActorId);
                    if (source == policy.actors.end() || source->second.runtimeSummon ||
                        source->second.groupId != id || source->second.mapId != mapId)
                        throw std::runtime_error("runtime_summon source must be a static actor in the same group");
                    if (!policy.actors.emplace(binding.spawnId, binding).second)
                        throw std::runtime_error("runtime_summon.actor_id is duplicated");
                    actorSpawns.push_back(binding.spawnId);
                }
                for (auto const& [selector, indices] : runtimeIndices)
                {
                    (void)selector;
                    if (indices.size() > 8 || *indices.rbegin() != indices.size())
                        throw std::runtime_error("runtime_summon summon_index must be contiguous from 1 to 8");
                }
            }
            if (group.contains("go_runtime_summons"))
            {
                json const& summons = group.at("go_runtime_summons");
                if (!summons.is_array() || summons.size() > 16)
                    throw std::runtime_error("group.go_runtime_summons must contain 0..16 records");
                std::set<std::pair<uint32_t, uint32_t>> identities;
                std::map<uint32_t, std::pair<uint32_t, uint32_t>> sourceSelectors;
                std::map<uint32_t, uint32_t> sourceEntries;
                std::map<uint32_t, std::set<uint32_t>> sourceIndices;
                std::map<uint32_t, std::vector<WojPoint>> sourcePositions;
                for (json const& actor : summons)
                {
                    RequireKeysOptional(actor, {"actor_id", "entry", "source_go_spawn_id", "source_go_entry",
                        "trigger", "summon_index", "position", "control", "aggro_talk"},
                        {"item_spell", "flee_enabled", "preferred_combat_range_yards"},
                        "go_runtime_summon");
                    if (!actor.at("aggro_talk").is_boolean())
                        throw std::runtime_error("go_runtime_summon.aggro_talk must be boolean");
                    if (!actor.at("trigger").is_string())
                        throw std::runtime_error("go_runtime_summon.trigger must be a string");
                    std::string const trigger = actor.at("trigger").get<std::string>();
                    if ((trigger != "item_cast" && trigger != "report_use") ||
                        (trigger == "item_cast") != actor.contains("item_spell"))
                        throw std::runtime_error("go_runtime_summon trigger/item_spell combination is invalid");
                    WojActorBinding binding;
                    binding.spawnId = ReadUInt(actor.at("actor_id"), "go_runtime_summon.actor_id",
                        0x80000000u, std::numeric_limits<uint32_t>::max());
                    binding.entry = ReadUInt(actor.at("entry"), "go_runtime_summon.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    binding.sourceActorId = ReadUInt(actor.at("source_go_spawn_id"),
                        "go_runtime_summon.source_go_spawn_id", 1, std::numeric_limits<uint32_t>::max());
                    binding.sourceGameObjectEntry = ReadUInt(actor.at("source_go_entry"),
                        "go_runtime_summon.source_go_entry", 1, std::numeric_limits<uint32_t>::max());
                    binding.sourceGameObjectReportUse = trigger == "report_use";
                    if (!binding.sourceGameObjectReportUse)
                        binding.createdBySpell = ReadUInt(actor.at("item_spell"),
                            "go_runtime_summon.item_spell", 1, std::numeric_limits<uint32_t>::max());
                    binding.summonIndex = ReadUInt(actor.at("summon_index"),
                        "go_runtime_summon.summon_index", 1, 8);
                    json const& position = actor.at("position");
                    RequireKeys(position, {"x", "y", "z"}, "go_runtime_summon.position");
                    binding.sourceGameObjectSummonPosition.x = ReadFiniteCoordinate(position.at("x"), "go_runtime_summon.position.x");
                    binding.sourceGameObjectSummonPosition.y = ReadFiniteCoordinate(position.at("y"), "go_runtime_summon.position.y");
                    binding.sourceGameObjectSummonPosition.z = ReadFiniteCoordinate(position.at("z"), "go_runtime_summon.position.z");
                    for (WojPoint const& other : sourcePositions[binding.sourceActorId])
                    {
                        float const dx = other.x - binding.sourceGameObjectSummonPosition.x;
                        float const dy = other.y - binding.sourceGameObjectSummonPosition.y;
                        float const dz = other.z - binding.sourceGameObjectSummonPosition.z;
                        if (dx * dx + dy * dy + dz * dz <= 4.0f)
                            throw std::runtime_error("go_runtime_summon positions overlap");
                    }
                    sourcePositions[binding.sourceActorId].push_back(binding.sourceGameObjectSummonPosition);
                    binding.runtimeSummon = true;
                    binding.sourceGameObject = true;
                    binding.mapId = mapId;
                    binding.groupId = id;
                    binding.control = ReadControl(actor.at("control"), "go_runtime_summon.control", true);
                    binding.groupControl = enabled ? groupControl : WojControl::Original;
                    binding.groupEnabled = enabled;
                    binding.aggroTalk = actor.at("aggro_talk").get<bool>();
                    if (actor.contains("flee_enabled"))
                    {
                        if (!actor.at("flee_enabled").is_boolean())
                            throw std::runtime_error("go_runtime_summon.flee_enabled must be boolean");
                        binding.hasFleeEnabledOverride = true;
                        binding.fleeEnabledOverride = actor.at("flee_enabled").get<bool>();
                    }
                    if (actor.contains("preferred_combat_range_yards"))
                    {
                        binding.hasPreferredCombatRange = true;
                        binding.preferredCombatRangeYards = ReadDistance(
                            actor.at("preferred_combat_range_yards"),
                            "go_runtime_summon.preferred_combat_range_yards");
                    }
                    binding.activation = activation;
                    auto const selector = std::make_pair(binding.sourceGameObjectEntry, binding.createdBySpell);
                    if (auto const it = sourceSelectors.find(binding.sourceActorId);
                        it != sourceSelectors.end() && it->second != selector)
                        throw std::runtime_error("go_runtime_summon source selector is ambiguous");
                    sourceSelectors.emplace(binding.sourceActorId, selector);
                    if (auto const it = sourceEntries.find(binding.sourceActorId);
                        it != sourceEntries.end() && it->second != binding.entry)
                        throw std::runtime_error("go_runtime_summon expected entry is ambiguous");
                    sourceEntries.emplace(binding.sourceActorId, binding.entry);
                    sourceIndices[binding.sourceActorId].insert(binding.summonIndex);
                    if (!identities.emplace(binding.sourceActorId, binding.summonIndex).second ||
                        !policy.actors.emplace(binding.spawnId, binding).second)
                        throw std::runtime_error("go_runtime_summon identity is duplicated");
                    actorSpawns.push_back(binding.spawnId);
                }
                for (auto const& [sourceId, indices] : sourceIndices)
                {
                    (void)sourceId;
                    if (indices.size() > 8 || *indices.rbegin() != indices.size())
                        throw std::runtime_error("go_runtime_summon indices must be contiguous from 1 to 8");
                }
            }
            bool const hasTalkActor = std::any_of(actorSpawns.begin(), actorSpawns.end(), [&policy](uint32_t spawnId)
            {
                return policy.actors.at(spawnId).aggroTalk;
            });
            if (hasTalkActor && !aggroTalk.enabled)
                throw std::runtime_error("actor aggro_talk requires enabled group policy");
            if (!aggroTalk.enabled && aggroTalk.chancePct != 0)
                throw std::runtime_error("disabled aggro_talk must have zero chance");
            if (aggroTalk.enabled && aggroTalk.chancePct == 0)
                throw std::runtime_error("enabled aggro_talk requires positive chance");
            bool const hasAmbientTalkActor = std::any_of(actorSpawns.begin(), actorSpawns.end(), [&policy](uint32_t spawnId)
            {
                return policy.actors.at(spawnId).ambientTalk;
            });
            if (hasAmbientTalkActor && !ambientTalk.enabled)
                throw std::runtime_error("actor ambient_talk requires enabled group policy");
            if (!ambientTalk.enabled && (ambientTalk.chancePct != 0 || ambientTalk.initial.minimum ||
                ambientTalk.initial.maximum || ambientTalk.repeat.minimum || ambientTalk.repeat.maximum))
                throw std::runtime_error("disabled ambient_talk must have zero timers and chance");
            if (ambientTalk.enabled && (ambientTalk.chancePct == 0 ||
                (ambientTalk.repeat.minimum == 0 && ambientTalk.repeat.maximum == 0)))
                throw std::runtime_error("enabled ambient_talk requires positive chance and repeat timer");

            if (group.contains("death_effects"))
            {
                json const& effects = group.at("death_effects");
                if (!effects.is_array() || effects.size() > 16)
                    throw std::runtime_error("group.death_effects must contain 0..16 records");
                std::set<std::string> effectIds;
                for (json const& value : effects)
                {
                    RequireKeysOptional(value, {"id", "type", "actor_id", "entry"},
                        {"spell", "expected_summon_actor_id", "expected_summon_count",
                         "gameobject_spawn_id", "gameobject_entry", "initial_object_state",
                         "final_object_state", "instance_data_index",
                         "initial_instance_data_value", "instance_data_value",
                         "instance_save_token_index"}, "death_effect");
                    if (!value.at("id").is_string())
                        throw std::runtime_error("death_effect.id must be a string");
                    WojDeathEffectBehavior effect;
                    effect.id = value.at("id").get<std::string>();
                    if (!ValidBindingId(effect.id) || !effectIds.insert(effect.id).second)
                        throw std::runtime_error("death_effect.id is invalid or duplicated in its group");
                    effect.entry = ReadUInt(value.at("entry"), "death_effect.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    effect.actorId = ReadUInt(value.at("actor_id"), "death_effect.actor_id", 1,
                        std::numeric_limits<uint32_t>::max());
                    auto const effectActor = policy.actors.find(effect.actorId);
                    if (effectActor == policy.actors.end() || effectActor->second.groupId != id ||
                        effectActor->second.entry != effect.entry)
                        throw std::runtime_error("death_effect actor_id/entry is not an exact actor in its group");
                    if (!value.at("type").is_string())
                        throw std::runtime_error("death_effect.type must be a string");
                    std::string const type = value.at("type").get<std::string>();
                    if (type == "cast_spell")
                    {
                        RequireKeys(value, {"id", "type", "actor_id", "entry", "spell",
                            "expected_summon_actor_id", "expected_summon_count"}, "cast_spell death_effect");
                        effect.kind = WojDeathEffectKind::CastSpell;
                        effect.spell = ReadUInt(value.at("spell"), "death_effect.spell", 1,
                            std::numeric_limits<uint32_t>::max());
                        effect.expectedSummonActorId = ReadUInt(value.at("expected_summon_actor_id"),
                            "death_effect.expected_summon_actor_id", 0x80000000u,
                            std::numeric_limits<uint32_t>::max());
                        // Runtime actor ids identify one physical summon in
                        // schema v3. Multi-summon identity needs ordinals/a
                        // vector contract and must not be accepted silently.
                        effect.expectedSummonCount = ReadUInt(value.at("expected_summon_count"),
                            "death_effect.expected_summon_count", 1, 1);
                        auto const expected = policy.actors.find(effect.expectedSummonActorId);
                        if (expected == policy.actors.end() || !expected->second.runtimeSummon ||
                            expected->second.groupId != id || expected->second.sourceActorId != effect.actorId ||
                            expected->second.createdBySpell != effect.spell)
                            throw std::runtime_error("cast_spell expected summon lineage is inconsistent");
                    }
                    else if (type == "activate_gameobject")
                    {
                        if (!value.contains("gameobject_spawn_id") || !value.contains("gameobject_entry") ||
                            !value.contains("initial_object_state") || !value.contains("final_object_state"))
                            throw std::runtime_error("activate_gameobject death_effect requires gameobject identity");
                        effect.kind = WojDeathEffectKind::ActivateGameObject;
                        effect.gameObjectSpawnId = ReadUInt(value.at("gameobject_spawn_id"),
                            "death_effect.gameobject_spawn_id", 1, std::numeric_limits<uint32_t>::max());
                        effect.gameObjectEntry = ReadUInt(value.at("gameobject_entry"),
                            "death_effect.gameobject_entry", 1, std::numeric_limits<uint32_t>::max());
                        effect.initialObjectState = ReadUInt(value.at("initial_object_state"),
                            "death_effect.initial_object_state", 0, 3);
                        effect.finalObjectState = ReadUInt(value.at("final_object_state"),
                            "death_effect.final_object_state", 0, 3);
                        if (effect.initialObjectState == effect.finalObjectState)
                            throw std::runtime_error("activate_gameobject must change object state");
                        bool const hasAnyInstance = value.contains("instance_data_index") ||
                            value.contains("initial_instance_data_value") ||
                            value.contains("instance_data_value") || value.contains("instance_save_token_index");
                        bool const hasAllInstance = value.contains("instance_data_index") &&
                            value.contains("initial_instance_data_value") &&
                            value.contains("instance_data_value") && value.contains("instance_save_token_index");
                        if (hasAnyInstance != hasAllInstance)
                            throw std::runtime_error("activate_gameobject instance fields must be all present or all absent");
                        effect.setInstanceData = hasAllInstance;
                        if (hasAllInstance)
                        {
                            effect.instanceDataIndex = ReadUInt(value.at("instance_data_index"),
                                "death_effect.instance_data_index", 0, 255);
                            effect.initialInstanceDataValue = ReadUInt(value.at("initial_instance_data_value"),
                                "death_effect.initial_instance_data_value", 0,
                                std::numeric_limits<uint32_t>::max());
                            effect.instanceDataValue = ReadUInt(value.at("instance_data_value"),
                                "death_effect.instance_data_value", 0, std::numeric_limits<uint32_t>::max());
                            effect.instanceSaveTokenIndex = ReadUInt(value.at("instance_save_token_index"),
                                "death_effect.instance_save_token_index", 0, 255);
                        }
                    }
                    else
                        throw std::runtime_error("death_effect.type is invalid");
                    policy.actorDeathEffects[effect.actorId].push_back(effect);
                }
            }

            if (group.contains("health_phases"))
            {
                json const& phases = group.at("health_phases");
                if (!phases.is_array() || phases.size() > 32)
                    throw std::runtime_error("group.health_phases must contain 0..32 records");
                std::set<std::string> phaseIds;
                std::unordered_map<uint32_t, uint32_t> previousThreshold;
                for (json const& value : phases)
                {
                    RequireKeys(value, {"id", "actor_id", "entry", "trigger_below_health_pct",
                        "transition_spell", "talk_group", "destination", "equipment_id", "dual_wield",
                        "equipment_delay_ms", "restore_combat_ms", "tactical_delay_ms"}, "health_phase");
                    if (!value.at("id").is_string())
                        throw std::runtime_error("health_phase.id must be a string");
                    WojHealthPhaseBehavior phase;
                    phase.id = value.at("id").get<std::string>();
                    if (!ValidBindingId(phase.id) || !phaseIds.insert(phase.id).second)
                        throw std::runtime_error("health_phase.id is invalid or duplicated in its group");
                    phase.actorId = ReadUInt(value.at("actor_id"), "health_phase.actor_id", 1,
                        std::numeric_limits<uint32_t>::max());
                    phase.entry = ReadUInt(value.at("entry"), "health_phase.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    auto const owner = policy.actors.find(phase.actorId);
                    if (owner == policy.actors.end() || owner->second.groupId != id ||
                        owner->second.entry != phase.entry)
                        throw std::runtime_error("health_phase actor_id/entry is not an exact actor in its group");
                    phase.triggerBelowHealthPct = ReadUInt(value.at("trigger_below_health_pct"),
                        "health_phase.trigger_below_health_pct", 1, 100);
                    auto previous = previousThreshold.find(phase.actorId);
                    if (previous != previousThreshold.end() && phase.triggerBelowHealthPct >= previous->second)
                        throw std::runtime_error("health phases for an actor must use strictly descending thresholds");
                    previousThreshold[phase.actorId] = phase.triggerBelowHealthPct;
                    phase.transitionSpell = ReadUInt(value.at("transition_spell"),
                        "health_phase.transition_spell", 1, std::numeric_limits<uint32_t>::max());
                    phase.talkGroup = ReadUInt(value.at("talk_group"), "health_phase.talk_group", 0, 255);
                    json const& destination = value.at("destination");
                    RequireKeys(destination, {"x", "y", "z", "o"}, "health_phase.destination");
                    for (char const* coordinate : {"x", "y", "z", "o"})
                        if (!destination.at(coordinate).is_number() ||
                            !std::isfinite(destination.at(coordinate).get<float>()))
                            throw std::runtime_error("health_phase destination coordinates must be finite numbers");
                    phase.destination = {destination.at("x").get<float>(), destination.at("y").get<float>(),
                        destination.at("z").get<float>(), destination.at("o").get<float>()};
                    if (std::fabs(phase.destination.x) > 100000.f || std::fabs(phase.destination.y) > 100000.f ||
                        std::fabs(phase.destination.z) > 100000.f || std::fabs(phase.destination.o) > 6.283186f)
                        throw std::runtime_error("health_phase destination coordinate is out of bounds");
                    phase.equipmentId = ReadUInt(value.at("equipment_id"), "health_phase.equipment_id", 1, 127);
                    if (!value.at("dual_wield").is_boolean())
                        throw std::runtime_error("health_phase.dual_wield must be boolean");
                    phase.dualWield = value.at("dual_wield").get<bool>();
                    phase.equipmentDelayMs = ReadUInt(value.at("equipment_delay_ms"),
                        "health_phase.equipment_delay_ms", 0, 3600000);
                    phase.restoreCombatMs = ReadUInt(value.at("restore_combat_ms"),
                        "health_phase.restore_combat_ms", 1, 3600000);
                    phase.tacticalDelayMs = ReadUInt(value.at("tactical_delay_ms"),
                        "health_phase.tactical_delay_ms", 0, 3600000);
                    if (phase.restoreCombatMs < phase.equipmentDelayMs)
                        throw std::runtime_error("health_phase restore_combat_ms precedes equipment_delay_ms");
                    policy.actorHealthPhases[phase.actorId].push_back(std::move(phase));
                }
            }

            if (group.contains("health_events"))
            {
                json const& events = group.at("health_events");
                if (!events.is_array() || events.size() > 32)
                    throw std::runtime_error("group.health_events must contain 0..32 records");
                std::set<std::string> eventIds;
                for (json const& value : events)
                {
                    RequireKeys(value, {"id", "actor_id", "entry", "health_pct_min",
                        "health_pct_max", "actions"}, "health_event");
                    if (!value.at("id").is_string())
                        throw std::runtime_error("health_event.id must be a string");
                    WojHealthEventBehavior event;
                    event.id = value.at("id").get<std::string>();
                    if (!ValidBindingId(event.id) || !eventIds.insert(event.id).second)
                        throw std::runtime_error("health_event.id is invalid or duplicated in its group");
                    event.actorId = ReadUInt(value.at("actor_id"), "health_event.actor_id", 1,
                        std::numeric_limits<uint32_t>::max());
                    event.entry = ReadUInt(value.at("entry"), "health_event.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    auto const owner = policy.actors.find(event.actorId);
                    if (owner == policy.actors.end() || owner->second.groupId != id ||
                        owner->second.entry != event.entry)
                        throw std::runtime_error("health_event actor_id/entry is not an exact actor in its group");
                    event.healthPctMin = ReadUInt(value.at("health_pct_min"),
                        "health_event.health_pct_min", 0, 100);
                    event.healthPctMax = ReadUInt(value.at("health_pct_max"),
                        "health_event.health_pct_max", 0, 100);
                    if (event.healthPctMin > event.healthPctMax)
                        throw std::runtime_error("health_event minimum exceeds maximum");
                    json const& actions = value.at("actions");
                    if (!actions.is_array() || actions.empty() || actions.size() > 2)
                        throw std::runtime_error("health_event actions must be talk or cast then talk");
                    for (size_t index = 0; index < actions.size(); ++index)
                    {
                        json const& action = actions.at(index);
                        if (!action.is_object() || !action.contains("kind") ||
                            !action.at("kind").is_string())
                            throw std::runtime_error("health_event action kind is invalid");
                        std::string const kind = action.at("kind").get<std::string>();
                        if (kind == "cast" && index == 0 && actions.size() == 2)
                        {
                            RequireKeys(action, {"kind", "spell", "target"}, "health_event cast");
                            if (!action.at("target").is_string() ||
                                action.at("target").get<std::string>() != "self")
                                throw std::runtime_error("health_event cast target must be self");
                            event.castSpell = ReadUInt(action.at("spell"),
                                "health_event cast spell", 1, std::numeric_limits<uint32_t>::max());
                        }
                        else if (kind == "talk" && index + 1 == actions.size())
                        {
                            RequireKeys(action, {"kind", "text_group"}, "health_event talk");
                            event.talkGroup = ReadUInt(action.at("text_group"),
                                "health_event talk text_group", 0, 255);
                        }
                        else
                            throw std::runtime_error("health_event actions must be talk or cast then talk");
                    }
                    policy.actorHealthEvents[event.actorId].push_back(std::move(event));
                }
            }

            if (group.contains("aggro_events"))
            {
                json const& events = group.at("aggro_events");
                if (!events.is_array() || events.size() > 32)
                    throw std::runtime_error("group.aggro_events must contain 0..32 records");
                std::set<std::string> eventIds;
                for (json const& value : events)
                {
                    RequireKeys(value, {"id", "actor_id", "entry", "actions"}, "aggro_event");
                    if (!value.at("id").is_string())
                        throw std::runtime_error("aggro_event.id must be a string");
                    WojAggroEventBehavior event;
                    event.id = value.at("id").get<std::string>();
                    if (!ValidBindingId(event.id) || !eventIds.insert(event.id).second)
                        throw std::runtime_error("aggro_event.id is invalid or duplicated in its group");
                    event.actorId = ReadUInt(value.at("actor_id"), "aggro_event.actor_id", 1,
                        std::numeric_limits<uint32_t>::max());
                    event.entry = ReadUInt(value.at("entry"), "aggro_event.entry", 1,
                        std::numeric_limits<uint32_t>::max());
                    auto const owner = policy.actors.find(event.actorId);
                    if (owner == policy.actors.end() || owner->second.groupId != id ||
                        owner->second.entry != event.entry)
                        throw std::runtime_error("aggro_event actor_id/entry is not an exact actor in its group");
                    json const& actions = value.at("actions");
                    if (!actions.is_array() || (actions.size() != 1 && actions.size() != 3))
                        throw std::runtime_error("aggro_event actions must be talk or cast/remove_aura/talk");
                    for (size_t index = 0; index < actions.size(); ++index)
                    {
                        json const& action = actions.at(index);
                        if (!action.is_object() || !action.contains("kind") || !action.at("kind").is_string())
                            throw std::runtime_error("aggro_event action kind is invalid");
                        std::string const kind = action.at("kind").get<std::string>();
                        WojAggroEventEffect effect;
                        if (kind == "cast" && actions.size() == 3 && index == 0)
                        {
                            RequireKeys(action, {"kind", "spell", "target"}, "aggro_event cast");
                            if (action.at("target") != "aggro_invoker")
                                throw std::runtime_error("aggro_event cast target must be aggro_invoker");
                            effect.kind = WojAggroEventEffectKind::Cast;
                            effect.spell = ReadUInt(action.at("spell"), "aggro_event cast spell", 1,
                                std::numeric_limits<uint32_t>::max());
                        }
                        else if (kind == "remove_aura" && actions.size() == 3 && index == 1)
                        {
                            RequireKeys(action, {"kind", "spell", "target"}, "aggro_event remove_aura");
                            if (action.at("target") != "self")
                                throw std::runtime_error("aggro_event remove_aura target must be self");
                            effect.kind = WojAggroEventEffectKind::RemoveAura;
                            effect.spell = ReadUInt(action.at("spell"), "aggro_event aura spell", 1,
                                std::numeric_limits<uint32_t>::max());
                        }
                        else if (kind == "talk" && index + 1 == actions.size())
                        {
                            RequireKeys(action, {"kind", "text_group"}, "aggro_event talk");
                            effect.kind = WojAggroEventEffectKind::Talk;
                            effect.textGroup = ReadUInt(action.at("text_group"),
                                "aggro_event talk text_group", 0, 255);
                        }
                        else
                            throw std::runtime_error("aggro_event actions must be talk or cast/remove_aura/talk");
                        event.actions.push_back(effect);
                    }
                    policy.actorAggroEvents[event.actorId].push_back(std::move(event));
                }
            }

            json const& spells = group.at("spells");
            if (!spells.is_array() || spells.size() > 128)
                throw std::runtime_error("group.spells must contain 0..128 records");
            std::set<std::pair<uint32_t, uint32_t>> spellKeys;
            std::unordered_map<uint32_t, uint32_t> spellsPerEntry;
            for (json const& value : spells)
            {
                RequireKeysOptional(value, {"entry", "spell", "initial_ms", "repeat_ms", "ticks_out_of_combat",
                    "chance_pct", "target", "allow_self", "require_caster_combat", "require_target_in_combat",
                    "require_engaged", "max_distance_yards", "require_aura_absent", "max_target_health_pct",
                    "timer_mode", "once_per_epoch", "completion"}, {"target_selector", "target_radius_yards",
                    "max_caster_health_pct", "require_caster_out_of_combat",
                    "min_snapshot_distance_yards"}, "spell");
                WojSpellBehavior behavior;
                behavior.entry = ReadUInt(value.at("entry"), "spell.entry", 1, std::numeric_limits<uint32_t>::max());
                behavior.spell = ReadUInt(value.at("spell"), "spell.spell", 1, std::numeric_limits<uint32_t>::max());
                behavior.initial = ReadTimer(value.at("initial_ms"), "spell.initial_ms");
                behavior.repeat = ReadTimer(value.at("repeat_ms"), "spell.repeat_ms");
                for (char const* field : {"ticks_out_of_combat", "allow_self", "require_caster_combat",
                     "require_target_in_combat", "require_engaged", "require_aura_absent", "once_per_epoch"})
                    if (!value.at(field).is_boolean())
                        throw std::runtime_error(std::string("spell.") + field + " must be boolean");
                behavior.ticksOutOfCombat = value.at("ticks_out_of_combat").get<bool>();
                behavior.allowSelf = value.at("allow_self").get<bool>();
                behavior.requireCasterCombat = value.at("require_caster_combat").get<bool>();
                if (value.contains("require_caster_out_of_combat"))
                {
                    if (!value.at("require_caster_out_of_combat").is_boolean())
                        throw std::runtime_error("spell.require_caster_out_of_combat must be boolean");
                    behavior.requireCasterOutOfCombat = value.at("require_caster_out_of_combat").get<bool>();
                }
                if (behavior.requireCasterCombat && behavior.requireCasterOutOfCombat)
                    throw std::runtime_error("spell cannot require caster both in and out of combat");
                behavior.requireTargetInCombat = value.at("require_target_in_combat").get<bool>();
                behavior.requireEngaged = value.at("require_engaged").get<bool>();
                behavior.requireAuraAbsent = value.at("require_aura_absent").get<bool>();
                behavior.oncePerEpoch = value.at("once_per_epoch").get<bool>();
                behavior.chancePct = ReadUInt(value.at("chance_pct"), "spell.chance_pct", 1, 100);
                if (!value.at("target").is_string() || !value.at("completion").is_string() || !value.at("timer_mode").is_string())
                    throw std::runtime_error("spell target, timer_mode and completion must be strings");
                behavior.target = value.at("target").get<std::string>();
                if (value.contains("target_selector"))
                {
                    if (!value.at("target_selector").is_string())
                        throw std::runtime_error("spell.target_selector must be a string");
                    behavior.targetSelector = value.at("target_selector").get<std::string>();
                }
                if (value.contains("target_radius_yards") && !value.at("target_radius_yards").is_null())
                {
                    behavior.hasTargetRadius = true;
                    behavior.targetRadiusYards = ReadDistance(value.at("target_radius_yards"),
                        "spell.target_radius_yards");
                }
                behavior.completion = value.at("completion").get<std::string>();
                std::string const timerMode = value.at("timer_mode").get<std::string>();
                if ((behavior.target != "enemy" && behavior.target != "ally" && behavior.target != "self") ||
                    (behavior.targetSelector != "any" && behavior.targetSelector != "victim" &&
                     behavior.targetSelector != "hostile_random") ||
                    (behavior.completion != "cast" && behavior.completion != "positive_heal") ||
                    (timerMode != "timer" && timerMode != "event"))
                    throw std::runtime_error("spell target, timer_mode or completion is invalid");
                if (behavior.targetSelector != "any" && behavior.target != "enemy")
                    throw std::runtime_error("spell target_selector requires enemy target");
                if (behavior.targetSelector == "hostile_random" && !behavior.hasTargetRadius)
                    throw std::runtime_error("hostile_random target_selector requires target_radius_yards");
                if (behavior.targetSelector != "hostile_random" && behavior.hasTargetRadius)
                    throw std::runtime_error("target_radius_yards is only valid for hostile_random");
                if (behavior.completion == "positive_heal" && behavior.target != "ally")
                    throw std::runtime_error("positive_heal completion requires ally target");
                if (behavior.target == "enemy" && behavior.allowSelf)
                    throw std::runtime_error("enemy spells cannot allow self");
                if (behavior.target == "self" && !behavior.allowSelf)
                    throw std::runtime_error("self spells must allow self");
                if (behavior.target == "enemy" && behavior.requireTargetInCombat)
                    throw std::runtime_error("enemy target_in_combat is unsupported by protocol 1.8");
                if (behavior.target == "ally" && behavior.requireEngaged)
                    throw std::runtime_error("ally engaged is unsupported by protocol 1.8");
                if (behavior.target == "self" && (behavior.requireTargetInCombat || behavior.requireEngaged))
                    throw std::runtime_error("self target cannot require target combat or engagement");
                behavior.eventDriven = timerMode == "event";
                if (behavior.eventDriven && (behavior.initial.minimum || behavior.initial.maximum || behavior.repeat.minimum ||
                    behavior.repeat.maximum || behavior.chancePct != 100 || behavior.ticksOutOfCombat))
                    throw std::runtime_error("event timer_mode requires zero timers and 100 percent chance");
                if (!value.at("max_distance_yards").is_null())
                {
                    behavior.hasMaxDistance = true;
                    behavior.maxDistanceYards = ReadDistance(value.at("max_distance_yards"), "spell.max_distance_yards");
                }
                if (value.contains("min_snapshot_distance_yards"))
                {
                    behavior.hasMinSnapshotDistance = true;
                    behavior.minSnapshotDistanceYards = ReadDistance(
                        value.at("min_snapshot_distance_yards"), "spell.min_snapshot_distance_yards");
                    // Shoot 6660 is the only observed stock ranged spell with
                    // this asynchronous close-range race. Keep the admission
                    // buffer pinned to its workshop Taskmasters instead of
                    // changing the targeting domain of other spells.
                    if (id != "deadmines_mast_workshop_fringe" || behavior.entry != 4417 ||
                        behavior.spell != 6660 || behavior.minSnapshotDistanceYards != 12.0f)
                        throw std::runtime_error("min_snapshot_distance_yards is reserved for workshop Taskmaster Shoot 6660 at 12 yd");
                }
                if (!value.at("max_target_health_pct").is_null())
                {
                    behavior.hasMaxTargetHealthPct = true;
                    behavior.maxTargetHealthPct = ReadUInt(value.at("max_target_health_pct"),
                        "spell.max_target_health_pct", 1, 100);
                }
                if (value.contains("max_caster_health_pct") && !value.at("max_caster_health_pct").is_null())
                {
                    behavior.hasMaxCasterHealthPct = true;
                    behavior.maxCasterHealthPct = ReadUInt(value.at("max_caster_health_pct"),
                        "spell.max_caster_health_pct", 1, 100);
                }
                if (!spellKeys.emplace(behavior.entry, behavior.spell).second)
                    throw std::runtime_error("duplicate group spell record");
                if (++spellsPerEntry[behavior.entry] > 16)
                    throw std::runtime_error("group.spells exceeds protocol limit of 16 records for one entry");
                bool foundEntry = false;
                for (uint32_t spawnId : actorSpawns)
                    if (policy.actors.at(spawnId).entry == behavior.entry)
                    {
                        policy.actorSpells[spawnId].emplace(behavior.spell, behavior);
                        foundEntry = true;
                    }
                if (!foundEntry)
                    throw std::runtime_error("spell.entry is not present in its group actors");
            }
            for (uint32_t spawnId : actorSpawns)
            {
                WojActorBinding const& binding = policy.actors.at(spawnId);
                if (binding.groupId != id)
                    continue;
                size_t const triggerCount = 6u + (binding.aggroTalk ? 1u : 0u) +
                    (binding.ambientTalk ? 1u : 0u) + policy.actorSpells[spawnId].size() +
                    policy.actorHealthPhases[spawnId].size() +
                    policy.actorHealthEvents[spawnId].size() +
                    policy.actorAggroEvents[spawnId].size();
                if (triggerCount > 24u)
                    throw std::runtime_error(
                        "actor behavior exceeds protocol limit of 24 triggers");
            }
            // Existing field consumers still need per-actor behavior; it is
            // intentionally copied only for v3-agnostic diagnostics.
            for (uint32_t spawnId : actorSpawns)
            {
                WojActorBinding const& binding = policy.actors.at(spawnId);
                if (binding.groupId == id)
                {
                    WojFleeBehavior actorFlee = flee;
                    if (binding.hasFleeEnabledOverride)
                        actorFlee.enabled = binding.fleeEnabledOverride;
                    policy.actorFlee.emplace(spawnId, actorFlee);
                    policy.actorAggroTalk.emplace(spawnId, aggroTalk);
                    policy.actorAmbientTalk.emplace(spawnId, ambientTalk);
                }
            }
        }
        policy.sceneId = "v3";
        policy.hasSceneRoster = true;
        policy.canonical = root.dump();
        return policy;
    }

    void ValidateV3BindingsAgainstDb(WojBehaviorPolicy const& policy)
    {
        if (policy.schemaVersion != 3)
            return;
        for (auto const& [spawnId, binding] : policy.actors)
        {
            if (binding.runtimeSummon)
            {
                if (spawnId < 0x80000000u)
                    throw std::runtime_error("runtime_summon.actor_id must use the reserved high range");
                if (binding.sourceGameObject)
                {
                    GameObjectData const* source = sObjectMgr->GetGameObjectData(binding.sourceActorId);
                    if (!source || source->id != binding.sourceGameObjectEntry ||
                        source->mapid != binding.mapId ||
                        (!binding.sourceGameObjectReportUse && !sSpellMgr->GetSpellInfo(binding.createdBySpell)))
                        throw std::runtime_error("go_runtime_summon exact GO source or item spell is invalid");
                    continue;
                }
                auto const source = policy.actors.find(binding.sourceActorId);
                if (source == policy.actors.end() || source->second.runtimeSummon ||
                    source->second.groupId != binding.groupId || source->second.mapId != binding.mapId)
                    throw std::runtime_error("runtime_summon source binding is invalid");
                SpellInfo const* spell = sSpellMgr->GetSpellInfo(binding.createdBySpell);
                bool summonsExpectedEntry = false;
                if (spell)
                    for (uint8_t index = 0; index < MAX_SPELL_EFFECTS; ++index)
                        if ((spell->Effects[index].Effect == SPELL_EFFECT_SUMMON ||
                             spell->Effects[index].Effect == SPELL_EFFECT_SUMMON_PET) &&
                            spell->Effects[index].MiscValue == static_cast<int32_t>(binding.entry))
                        {
                            summonsExpectedEntry = true;
                            break;
                        }
                if (!summonsExpectedEntry)
                    throw std::runtime_error("runtime_summon created_by_spell does not summon its configured entry");
                continue;
            }
            CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
            if (!data)
                throw std::runtime_error("actor.spawn_id is absent from creature spawn data");
            // A v3 binding is entry-specific. Multi-spawn alternates would
            // make one spawn id legitimately instantiate a different entry
            // while retaining this actor's group/capabilities, so reject the
            // catalog rather than bind an ambiguous live NPC.
            if (data->id2 != 0 || data->id3 != 0)
                throw std::runtime_error("actor.spawn_id has unsupported multi-spawn alternate entries");
            if (data->id != binding.entry)
                throw std::runtime_error("actor.entry disagrees with creature spawn data");
            if (data->mapid != binding.mapId)
                throw std::runtime_error("actor map_id disagrees with creature spawn data");
        }
        for (auto const& [spawnId, effects] : policy.actorDeathEffects)
        {
            WojActorBinding const& actor = policy.actors.at(spawnId);
            for (WojDeathEffectBehavior const& effect : effects)
            {
                if (effect.kind == WojDeathEffectKind::CastSpell)
                {
                    SpellInfo const* spell = sSpellMgr->GetSpellInfo(effect.spell);
                    WojActorBinding const* expected = nullptr;
                    if (auto const it = policy.actors.find(effect.expectedSummonActorId);
                        it != policy.actors.end())
                        expected = &it->second;
                    bool summonsExpectedEntry = false;
                    if (spell && expected)
                        for (uint8_t index = 0; index < MAX_SPELL_EFFECTS; ++index)
                            if ((spell->Effects[index].Effect == SPELL_EFFECT_SUMMON ||
                                 spell->Effects[index].Effect == SPELL_EFFECT_SUMMON_PET) &&
                                spell->Effects[index].MiscValue == static_cast<int32_t>(expected->entry))
                            {
                                summonsExpectedEntry = true;
                                break;
                            }
                    if (!summonsExpectedEntry)
                        throw std::runtime_error("cast_spell does not summon the configured runtime entry");
                    continue;
                }
                GameObjectData const* data = sObjectMgr->GetGameObjectData(effect.gameObjectSpawnId);
                if (!data)
                    throw std::runtime_error("death_effect gameobject spawn is absent from spawn data");
                if (data->id != effect.gameObjectEntry || data->mapid != actor.mapId)
                    throw std::runtime_error("death_effect gameobject entry or map disagrees with spawn data");
            }
        }
    }

    WojBehaviorPolicy ParseBehaviorPolicy(std::string const& text)
    {
        json const root = ParseStrictJson(text);
        if (!root.is_object() || !root.contains("schema_version") ||
            !root.at("schema_version").is_number_unsigned())
            throw std::runtime_error("schema_version must be an unsigned integer");
        if (root.at("schema_version").get<uint64_t>() == 1)
            return ParseLegacyBehaviorPolicy(root);
        if (root.at("schema_version").get<uint64_t>() == 3)
            return ParseV3BehaviorPolicy(root);
        RequireKeys(root, {"schema_version", "revision", "active_scene", "scenes"}, "root");
        if (ReadUInt(root.at("schema_version"), "schema_version", 2, 2) != 2)
            throw std::runtime_error("unsupported schema_version");
        if (!root.at("active_scene").is_string())
            throw std::runtime_error("active_scene must be a string");

        auto validSceneId = [](std::string const& value)
        {
            return !value.empty() && value.size() <= 32 && std::all_of(value.begin(), value.end(),
                [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; });
        };
        std::string const activeScene = root.at("active_scene").get<std::string>();
        if (!validSceneId(activeScene))
            throw std::runtime_error("active_scene has invalid characters");

        uint32_t const revision = ReadUInt(root.at("revision"), "revision", 1,
                                           std::numeric_limits<uint32_t>::max());
        auto approvedSpell = [](uint32_t entry, uint32_t spell, std::string const& target,
                                bool eventDriven)
        {
            if (entry == 127 && spell == 11831)
                return target == "enemy" && !eventDriven;
            if (entry == 127 && spell == 744)
                return target == "enemy" && !eventDriven;
            if (entry == 391 && spell == 3584)
                return target == "enemy" && !eventDriven;
            if (entry == 517 && spell == 9734)
                return target == "enemy" && !eventDriven;
            if (entry == 517 && spell == 6074)
                return target == "ally" && !eventDriven;
            if (entry == 500 && spell == 6660)
                return target == "enemy" && !eventDriven;
            if (entry == 1065 && spell == 9532)
                return target == "enemy" && !eventDriven;
            return entry == 1065 && spell == 913 && target == "ally" && eventDriven;
        };
        auto expectedCatalog = [](std::string const& sceneId)
            -> std::set<std::pair<uint32_t, uint32_t>> const&
        {
            static std::set<std::pair<uint32_t, uint32_t>> const murlocs = {
                {127, 11831}, {127, 744}, {391, 3584}, {517, 9734}, {517, 6074}
            };
            static std::set<std::pair<uint32_t, uint32_t>> const riverpaw = {
                {500, 6660}, {1065, 9532}, {1065, 913}
            };
            if (sceneId == "murlocs")
                return murlocs;
            if (sceneId == "riverpaw")
                return riverpaw;
            throw std::runtime_error("scene.id is not in the curated scene catalog");
        };
        json const& scenes = root.at("scenes");
        if (!scenes.is_array() || scenes.empty() || scenes.size() > 8)
            throw std::runtime_error("scenes must contain 1..8 records");

        std::set<std::string> sceneIds;
        std::set<uint32_t> allConfiguredSpells;
        bool foundActive = false;
        WojBehaviorPolicy selected;
        for (json const& scene : scenes)
        {
            RequireKeys(scene, {"id", "map_id", "owned_guids", "spells", "flee", "aggro_talk"}, "scene");
            if (!scene.at("id").is_string())
                throw std::runtime_error("scene.id must be a string");
            std::string const sceneId = scene.at("id").get<std::string>();
            if (!validSceneId(sceneId) || !sceneIds.insert(sceneId).second)
                throw std::runtime_error("scene.id is invalid or duplicated");
            WojBehaviorPolicy current;
            current.sceneId = sceneId;
            current.mapId = ReadUInt(scene.at("map_id"), "scene.map_id", 0, 65535);
            json const& guids = scene.at("owned_guids");
            if (!guids.is_array() || guids.empty() || guids.size() > 14)
                throw std::runtime_error("scene.owned_guids must contain 1..14 records");
            for (json const& guid : guids)
            {
                uint32_t const parsed = ReadUInt(guid, "scene.owned_guids", 1,
                                                 std::numeric_limits<uint32_t>::max());
                if (!current.ownedGuids.insert(parsed).second)
                    throw std::runtime_error("scene.owned_guids contains a duplicate");
            }

            json const& spells = scene.at("spells");
            if (!spells.is_array() || spells.empty() || spells.size() > 16)
                throw std::runtime_error("scene.spells must contain 1..16 records");
            std::set<std::pair<uint32_t, uint32_t>> actual;
            for (json const& value : spells)
            {
                RequireKeys(value, {"entry", "spell", "initial_ms", "repeat_ms",
                    "ticks_out_of_combat", "chance_pct", "target", "require_caster_combat",
                    "require_target_in_combat", "require_engaged", "max_distance_yards",
                    "require_aura_absent", "max_target_health_pct", "timer_mode"}, "spell");
                WojSpellBehavior behavior;
                behavior.entry = ReadUInt(value.at("entry"), "entry", 1,
                                          std::numeric_limits<uint32_t>::max());
                behavior.spell = ReadUInt(value.at("spell"), "spell", 1,
                                          std::numeric_limits<uint32_t>::max());
                behavior.initial = ReadTimer(value.at("initial_ms"), "initial_ms");
                behavior.repeat = ReadTimer(value.at("repeat_ms"), "repeat_ms");
                if (!value.at("ticks_out_of_combat").is_boolean() ||
                    !value.at("require_caster_combat").is_boolean() ||
                    !value.at("require_target_in_combat").is_boolean() ||
                    !value.at("require_engaged").is_boolean() ||
                    !value.at("require_aura_absent").is_boolean())
                    throw std::runtime_error("spell predicate flags must be booleans");
                behavior.ticksOutOfCombat = value.at("ticks_out_of_combat").get<bool>();
                behavior.chancePct = ReadUInt(value.at("chance_pct"), "chance_pct", 1, 100);
                if (!value.at("target").is_string())
                    throw std::runtime_error("target must be enemy or ally");
                behavior.target = value.at("target").get<std::string>();
                if (behavior.target != "enemy" && behavior.target != "ally")
                    throw std::runtime_error("target must be enemy or ally");
                behavior.requireCasterCombat = value.at("require_caster_combat").get<bool>();
                behavior.requireTargetInCombat = value.at("require_target_in_combat").get<bool>();
                behavior.requireEngaged = value.at("require_engaged").get<bool>();
                behavior.requireAuraAbsent = value.at("require_aura_absent").get<bool>();
                if (behavior.target == "enemy" && behavior.requireTargetInCombat)
                    throw std::runtime_error("enemy require_target_in_combat is unsupported");
                if (behavior.target == "ally" && behavior.requireEngaged)
                    throw std::runtime_error("ally require_engaged is unsupported");
                if (!value.at("timer_mode").is_string())
                    throw std::runtime_error("timer_mode must be timer or event");
                std::string const timerMode = value.at("timer_mode").get<std::string>();
                if (timerMode != "timer" && timerMode != "event")
                    throw std::runtime_error("timer_mode must be timer or event");
                behavior.eventDriven = timerMode == "event";
                if (behavior.eventDriven &&
                    (behavior.initial.minimum != 0 || behavior.initial.maximum != 0 ||
                     behavior.repeat.minimum != 0 || behavior.repeat.maximum != 0 ||
                     behavior.chancePct != 100 || behavior.ticksOutOfCombat))
                    throw std::runtime_error("event timer_mode requires zero timers and 100 percent chance");
                if (!value.at("max_distance_yards").is_null())
                {
                    behavior.hasMaxDistance = true;
                    behavior.maxDistanceYards = ReadDistance(value.at("max_distance_yards"), "max_distance_yards");
                }
                if (!value.at("max_target_health_pct").is_null())
                {
                    behavior.hasMaxTargetHealthPct = true;
                    behavior.maxTargetHealthPct = ReadUInt(value.at("max_target_health_pct"),
                                                            "max_target_health_pct", 1, 100);
                }
                if (!approvedSpell(behavior.entry, behavior.spell, behavior.target, behavior.eventDriven))
                    throw std::runtime_error("spell does not match the curated capability catalog");
                uint32_t const spellId = behavior.spell;
                if (!actual.emplace(behavior.entry, spellId).second ||
                    !current.spells.emplace(behavior.spell, std::move(behavior)).second)
                    throw std::runtime_error("duplicate spell record");
                if (!allConfiguredSpells.insert(spellId).second)
                    throw std::runtime_error("spell ids must be unique across curated scenes");
            }
            if (actual != expectedCatalog(sceneId))
                throw std::runtime_error("scene spell catalog disagrees with its curated capabilities");

            json const& flee = scene.at("flee");
            RequireKeys(flee, {"max_health_pct", "once_per_epoch", "require_in_combat",
                "require_not_casting", "require_engaged", "max_distance_yards"}, "flee");
            current.flee.maxHealthPct = ReadUInt(flee.at("max_health_pct"), "flee.max_health_pct", 1, 100);
            if (!flee.at("once_per_epoch").is_boolean() || !flee.at("require_in_combat").is_boolean() ||
                !flee.at("require_not_casting").is_boolean() || !flee.at("require_engaged").is_boolean())
                throw std::runtime_error("flee predicate flags must be booleans");
            current.flee.oncePerEpoch = flee.at("once_per_epoch").get<bool>();
            current.flee.requireInCombat = flee.at("require_in_combat").get<bool>();
            current.flee.requireNotCasting = flee.at("require_not_casting").get<bool>();
            current.flee.requireEngaged = flee.at("require_engaged").get<bool>();
            current.flee.maxDistanceYards = ReadDistance(flee.at("max_distance_yards"),
                                                         "flee.max_distance_yards");
            json const& aggroTalk = scene.at("aggro_talk");
            RequireKeys(aggroTalk, {"enabled", "chance_pct", "text_group"}, "aggro_talk");
            if (!aggroTalk.at("enabled").is_boolean())
                throw std::runtime_error("aggro_talk.enabled must be boolean");
            current.aggroTalk.enabled = aggroTalk.at("enabled").get<bool>();
            current.aggroTalk.chancePct = ReadUInt(aggroTalk.at("chance_pct"),
                                                   "aggro_talk.chance_pct", 0, 100);
            current.aggroTalk.textGroup = ReadUInt(aggroTalk.at("text_group"),
                                                   "aggro_talk.text_group", 0, 0);
            bool const curatedRiverpawTalk = sceneId == "riverpaw" && current.aggroTalk.enabled &&
                current.aggroTalk.chancePct >= 1 && current.aggroTalk.chancePct <= 100 &&
                current.aggroTalk.textGroup == 0;
            bool const curatedDisabledTalk = sceneId == "murlocs" && !current.aggroTalk.enabled &&
                current.aggroTalk.chancePct == 0 && current.aggroTalk.textGroup == 0;
            if (!curatedRiverpawTalk && !curatedDisabledTalk)
                throw std::runtime_error("aggro_talk disagrees with curated scene policy");
            if (sceneId == activeScene)
            {
                selected = std::move(current);
                foundActive = true;
            }
        }
        if (!foundActive)
            throw std::runtime_error("active_scene has no scene record");
        WojBehaviorPolicy policy = std::move(selected);
        policy.revision = revision;
        policy.hasSceneRoster = true;
        policy.canonical = root.dump();
        return policy;
    }
}

WojConfig& WojConfig::Instance()
{
    static WojConfig instance;
    return instance;
}


char const* WojConfig::ModeName() const
{
    return Mode == WojMode::Original ? "original" : "ours";
}

void WojConfig::SetMode(WojMode mode)
{
    Mode = mode;
}

bool WojConfig::ArenaDestination(float& x, float& y) const
{
    // Калиброванная цель мурлоков побайтно совпадает со старыми константами
    // WojCombatLog. Riverpaw — центроид шести точных DB home
    // (x -9887.56..-9877.14, y 1540.43..1558.94): он лежит внутри лагеря и
    // не следует за временной позицией патруля.
    if (SceneId == "murlocs")
    {
        x = -11367.2f;
        y = 1806.3f;
        return true;
    }
    if (SceneId == "riverpaw")
    {
        x = -9881.915f;
        y = 1550.8583f;
        return true;
    }
    return false;
}

WojActorBinding const* WojConfig::FindActor(uint32_t spawnId) const
{
    auto const it = Behavior.actors.find(spawnId);
    return it == Behavior.actors.end() ? nullptr : &it->second;
}

WojControl WojConfig::EffectiveControl(uint32_t spawnId) const
{
    WojActorBinding const* actor = FindActor(spawnId);
    if (!actor)
        return WojControl::Original;
    // A disabled v3 group is absent from the gateway's policy selection.
    // Letting a transient group/actor overlay reactivate it locally would
    // create an AI that can only receive NO_DECISION, and violates the
    // atomic enabled gate on the other side of the protocol.
    if (!actor->groupEnabled)
        return WojControl::Original;
    if (auto const it = ActorOverlays.find(spawnId); it != ActorOverlays.end())
        return it->second;
    if (auto const it = GroupOverlays.find(actor->groupId); it != GroupOverlays.end())
        return it->second;
    if (actor->control != WojControl::Inherit)
        return actor->control;
    return actor->groupControl;
}

bool WojConfig::IsJevActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const
{
    if (Behavior.schemaVersion != 3)
        return Enabled && BehaviorPolicyValid && Mode == WojMode::Ours && mapId == SceneMapId && OwnedGuids.count(spawnId);
    WojActorBinding const* actor = FindActor(spawnId);
    return Enabled && BehaviorPolicyValid && actor && actor->entry == entry && actor->mapId == mapId &&
        EffectiveControl(spawnId) == WojControl::Jev;
}

bool WojConfig::IsRecordingOriginalActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const
{
    if (!RecordOriginal || !BehaviorPolicyValid)
        return false;
    if (Behavior.schemaVersion != 3)
        return Mode == WojMode::Original && mapId == SceneMapId && OwnedGuids.count(spawnId);
    WojActorBinding const* actor = FindActor(spawnId);
    return actor && actor->groupEnabled && actor->entry == entry && actor->mapId == mapId &&
        EffectiveControl(spawnId) == WojControl::Original;
}

bool WojConfig::IsBoundActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const
{
    if (Behavior.schemaVersion != 3)
        return IsJevActor(spawnId, entry, mapId);
    WojActorBinding const* actor = FindActor(spawnId);
    return BehaviorPolicyValid && actor && actor->groupEnabled && actor->entry == entry && actor->mapId == mapId;
}

bool WojConfig::SameGroup(uint32_t first, uint32_t second) const
{
    if (Behavior.schemaVersion != 3)
        return OwnedGuids.count(first) && OwnedGuids.count(second);
    WojActorBinding const* left = FindActor(first);
    WojActorBinding const* right = FindActor(second);
    return left && right && left->groupId == right->groupId;
}

std::string WojConfig::GroupId(uint32_t spawnId) const
{
    if (WojActorBinding const* actor = FindActor(spawnId))
        return actor->groupId;
    return SceneId;
}

WojSpellBehavior const* WojConfig::FindBehaviorSpellForActor(uint32_t spawnId, uint32_t spell) const
{
    if (Behavior.schemaVersion != 3)
        return FindBehaviorSpell(spell);
    auto const actor = Behavior.actorSpells.find(spawnId);
    if (actor == Behavior.actorSpells.end())
        return nullptr;
    auto const behavior = actor->second.find(spell);
    return behavior == actor->second.end() ? nullptr : &behavior->second;
}

WojActorBinding const* WojConfig::FindRuntimeSummonForCast(uint32_t sourceActorId, uint32_t spell) const
{
    return FindRuntimeSummonForCastIndex(sourceActorId, spell, 1);
}

WojActorBinding const* WojConfig::FindRuntimeSummonForCastIndex(uint32_t sourceActorId,
    uint32_t spell, uint32_t summonIndex) const
{
    WojActorBinding const* found = nullptr;
    for (auto const& [actorId, actor] : Behavior.actors)
    {
        (void)actorId;
        if (!actor.runtimeSummon || actor.sourceGameObject || actor.sourceActorId != sourceActorId ||
            actor.createdBySpell != spell || actor.summonIndex != summonIndex)
            continue;
        if (found)
            return nullptr;
        found = &actor;
    }
    return found;
}

uint32_t WojConfig::RuntimeSummonCountForCast(uint32_t sourceActorId, uint32_t spell) const
{
    uint32_t count = 0;
    for (auto const& [actorId, actor] : Behavior.actors)
    {
        (void)actorId;
        if (actor.runtimeSummon && !actor.sourceGameObject && actor.sourceActorId == sourceActorId &&
            actor.createdBySpell == spell)
            ++count;
    }
    return count;
}

std::vector<WojSpellBehavior const*> WojConfig::BehaviorSpellsForActor(uint32_t spawnId, uint32_t entry) const
{
    if (Behavior.schemaVersion != 3)
        return BehaviorSpellsForEntry(entry);
    std::vector<WojSpellBehavior const*> result;
    auto const found = Behavior.actorSpells.find(spawnId);
    if (found == Behavior.actorSpells.end())
        return result;
    for (auto const& [spell, behavior] : found->second)
    {
        (void)spell;
        if (behavior.entry == entry)
            result.push_back(&behavior);
    }
    std::sort(result.begin(), result.end(), [](WojSpellBehavior const* left, WojSpellBehavior const* right)
    {
        return left->spell < right->spell;
    });
    return result;
}

bool WojConfig::ActorAggroTalk(uint32_t spawnId) const
{
    if (Behavior.schemaVersion != 3)
        return Behavior.aggroTalk.enabled;
    WojActorBinding const* actor = FindActor(spawnId);
    return actor && actor->aggroTalk;
}

bool WojConfig::ActorAmbientTalk(uint32_t spawnId) const
{
    if (Behavior.schemaVersion != 3)
        return false;
    WojActorBinding const* actor = FindActor(spawnId);
    return actor && actor->ambientTalk;
}

WojActivationBehavior const& WojConfig::ActivationForActor(uint32_t spawnId) const
{
    static WojActivationBehavior const always;
    if (Behavior.schemaVersion != 3)
        return always;
    WojActorBinding const* actor = FindActor(spawnId);
    return actor ? actor->activation : always;
}

WojFleeBehavior const& WojConfig::FleeForActor(uint32_t spawnId) const
{
    if (auto const it = Behavior.actorFlee.find(spawnId); it != Behavior.actorFlee.end())
        return it->second;
    return Behavior.flee;
}

WojAggroTalkBehavior const& WojConfig::AggroTalkForActor(uint32_t spawnId) const
{
    if (auto const it = Behavior.actorAggroTalk.find(spawnId); it != Behavior.actorAggroTalk.end())
        return it->second;
    return Behavior.aggroTalk;
}

WojAmbientTalkBehavior const& WojConfig::AmbientTalkForActor(uint32_t spawnId) const
{
    static WojAmbientTalkBehavior const disabled;
    if (auto const it = Behavior.actorAmbientTalk.find(spawnId); it != Behavior.actorAmbientTalk.end())
        return it->second;
    return disabled;
}

std::vector<WojDeathEffectBehavior> const& WojConfig::DeathEffectsForActor(uint32_t spawnId) const
{
    static std::vector<WojDeathEffectBehavior> const empty;
    auto const it = Behavior.actorDeathEffects.find(spawnId);
    return it == Behavior.actorDeathEffects.end() ? empty : it->second;
}

std::vector<WojHealthPhaseBehavior> const& WojConfig::HealthPhasesForActor(uint32_t spawnId) const
{
    static std::vector<WojHealthPhaseBehavior> const empty;
    auto const it = Behavior.actorHealthPhases.find(spawnId);
    return it == Behavior.actorHealthPhases.end() ? empty : it->second;
}

std::vector<WojHealthEventBehavior> const& WojConfig::HealthEventsForActor(uint32_t spawnId) const
{
    static std::vector<WojHealthEventBehavior> const empty;
    auto const it = Behavior.actorHealthEvents.find(spawnId);
    return it == Behavior.actorHealthEvents.end() ? empty : it->second;
}

std::vector<WojAggroEventBehavior> const& WojConfig::AggroEventsForActor(uint32_t spawnId) const
{
    static std::vector<WojAggroEventBehavior> const empty;
    auto const it = Behavior.actorAggroEvents.find(spawnId);
    return it == Behavior.actorAggroEvents.end() ? empty : it->second;
}

bool WojConfig::GroupArenaAnchor(std::string const& groupId, WojPoint& out) const
{
    auto const it = Behavior.arenaAnchors.find(groupId);
    if (it == Behavior.arenaAnchors.end())
        return false;
    out = it->second;
    return true;
}

bool WojConfig::GroupMapId(std::string const& groupId, uint32_t& out) const
{
    for (auto const& [spawnId, actor] : Behavior.actors)
    {
        (void)spawnId;
        if (actor.groupId == groupId)
        {
            out = actor.mapId;
            return true;
        }
    }
    return false;
}

std::unordered_set<uint32_t> WojConfig::BindingActors() const
{
    std::unordered_set<uint32_t> result;
    if (Behavior.schemaVersion == 3)
        for (auto const& [spawnId, unused] : Behavior.actors)
        {
            (void)unused;
            result.insert(spawnId);
        }
    else
        result = Behavior.ownedGuids;
    return result;
}

std::unordered_set<uint32_t> WojConfig::ChangedBindingActors(WojBehaviorPolicy const& previous) const
{
    std::unordered_set<uint32_t> changed;
    if (previous.schemaVersion != 3 || Behavior.schemaVersion != 3)
    {
        if (previous.schemaVersion != Behavior.schemaVersion || previous.mapId != Behavior.mapId ||
            previous.ownedGuids != Behavior.ownedGuids)
        {
            changed = previous.ownedGuids;
            for (auto const& [spawnId, unused] : previous.actors)
            {
                (void)unused;
                changed.insert(spawnId);
            }
            std::unordered_set<uint32_t> const current = BindingActors();
            changed.insert(current.begin(), current.end());
        }
        return changed;
    }

    auto effective = [this](WojBehaviorPolicy const& policy, uint32_t spawnId)
    {
        auto const actor = policy.actors.find(spawnId);
        if (actor == policy.actors.end())
            return WojControl::Original;
        if (auto const it = ActorOverlays.find(spawnId); it != ActorOverlays.end())
            return it->second;
        if (auto const it = GroupOverlays.find(actor->second.groupId); it != GroupOverlays.end())
            return it->second;
        if (actor->second.control != WojControl::Inherit)
            return actor->second.control;
        return actor->second.groupControl;
    };

    changed = BindingActors();
    for (auto const& [spawnId, unused] : previous.actors)
    {
        (void)unused;
        changed.insert(spawnId);
    }
    for (auto const& [spawnId, oldActor] : previous.actors)
    {
        auto const current = Behavior.actors.find(spawnId);
        if (current == Behavior.actors.end() || oldActor.entry != current->second.entry ||
            oldActor.mapId != current->second.mapId || oldActor.groupId != current->second.groupId ||
            oldActor.runtimeSummon != current->second.runtimeSummon ||
            oldActor.sourceGameObject != current->second.sourceGameObject ||
            oldActor.sourceGameObjectReportUse != current->second.sourceGameObjectReportUse ||
            oldActor.sourceActorId != current->second.sourceActorId ||
            oldActor.sourceGameObjectEntry != current->second.sourceGameObjectEntry ||
            oldActor.createdBySpell != current->second.createdBySpell ||
            oldActor.summonIndex != current->second.summonIndex ||
            oldActor.sourceGameObjectSummonPosition.x != current->second.sourceGameObjectSummonPosition.x ||
            oldActor.sourceGameObjectSummonPosition.y != current->second.sourceGameObjectSummonPosition.y ||
            oldActor.sourceGameObjectSummonPosition.z != current->second.sourceGameObjectSummonPosition.z ||
            oldActor.hasFleeEnabledOverride != current->second.hasFleeEnabledOverride ||
            oldActor.fleeEnabledOverride != current->second.fleeEnabledOverride ||
            oldActor.hasPreferredCombatRange != current->second.hasPreferredCombatRange ||
            oldActor.preferredCombatRangeYards != current->second.preferredCombatRangeYards ||
            oldActor.hasExecutorRepeatContinuationSpell != current->second.hasExecutorRepeatContinuationSpell ||
            oldActor.executorRepeatContinuationSpell != current->second.executorRepeatContinuationSpell ||
            oldActor.preserveStockRoutine != current->second.preserveStockRoutine ||
            oldActor.groupEnabled != current->second.groupEnabled ||
            effective(previous, spawnId) != effective(Behavior, spawnId))
            continue;
        changed.erase(spawnId);
    }
    return changed;
}

std::unordered_set<uint32_t> WojConfig::GroupActors(std::string const& id) const
{
    std::unordered_set<uint32_t> result;
    for (auto const& [spawnId, actor] : Behavior.actors)
        if (actor.groupId == id)
            result.insert(spawnId);
    return result;
}

bool WojConfig::SetGroupOverlay(std::string const& id, WojControl mode)
{
    if (!Behavior.groupControls.count(id))
        return false;
    if (Behavior.schemaVersion == 3 && mode == WojControl::Jev)
    {
        bool enabled = false;
        for (auto const& [spawnId, actor] : Behavior.actors)
        {
            (void)spawnId;
            if (actor.groupId == id)
            {
                enabled = actor.groupEnabled;
                break;
            }
        }
        // The gateway refuses disabled groups before actor selection. Do not
        // report a successful local Jev switch that can never issue a
        // request; `original`/`inherit` remain meaningful no-op controls.
        if (!enabled)
            return false;
    }
    if (mode == WojControl::Inherit)
        GroupOverlays.erase(id);
    else
        GroupOverlays[id] = mode;
    if (Behavior.schemaVersion == 3)
    {
        OwnedGuids.clear();
        for (auto const& [spawnId, actor] : Behavior.actors)
        {
            (void)actor;
            if (EffectiveControl(spawnId) == WojControl::Jev)
                OwnedGuids.insert(spawnId);
        }
        for (auto& [spawnId, actor] : Behavior.actors)
            if (actor.groupId == id)
                actor.generation = NextBindingGeneration(actor.generation);
    }
    else
        ++BindingGeneration;
    return true;
}

bool WojConfig::SetActorOverlay(uint32_t spawnId, WojControl mode)
{
    WojActorBinding const* const binding = FindActor(spawnId);
    if (!binding)
        return false;
    if (Behavior.schemaVersion == 3 && mode == WojControl::Jev && !binding->groupEnabled)
        return false;
    if (mode == WojControl::Inherit)
        ActorOverlays.erase(spawnId);
    else
        ActorOverlays[spawnId] = mode;
    if (Behavior.schemaVersion == 3)
    {
        OwnedGuids.clear();
        for (auto const& [id, actor] : Behavior.actors)
        {
            (void)actor;
            if (EffectiveControl(id) == WojControl::Jev)
                OwnedGuids.insert(id);
        }
        if (auto actor = Behavior.actors.find(spawnId); actor != Behavior.actors.end())
            actor->second.generation = NextBindingGeneration(actor->second.generation);
    }
    else
        ++BindingGeneration;
    return true;
}

WojSpellBehavior const* WojConfig::FindBehaviorSpell(uint32_t spell) const
{
    auto const it = Behavior.spells.find(spell);
    return it == Behavior.spells.end() ? nullptr : &it->second;
}

std::vector<WojSpellBehavior const*> WojConfig::BehaviorSpellsForEntry(uint32_t entry) const
{
    std::vector<WojSpellBehavior const*> result;
    for (auto const& [spell, behavior] : Behavior.spells)
    {
        (void)spell;
        if (behavior.entry == entry)
            result.push_back(&behavior);
    }
    std::sort(result.begin(), result.end(), [](WojSpellBehavior const* left, WojSpellBehavior const* right)
    {
        return left->spell < right->spell;
    });
    return result;
}

void WojConfig::RebuildDerivedCatalog()
{
    OwnedGuids.clear();
    Spells.clear();

    // A v2/v3 roster takes ownership of these derived fields even while a
    // first-boot v3 DB preflight is pending. Falling through to old
    // WorldOfJevs.OwnedGuids here would make the selector fail closed while
    // keep-alive, arena and observer commands still act on a legacy roster.
    if (Behavior.hasSceneRoster)
    {
        SceneId = Behavior.sceneId;
        SceneMapId = Behavior.mapId;
        if (!BehaviorPolicyValid)
            return;

        OwnedGuids = Behavior.ownedGuids;
        if (Behavior.schemaVersion == 3)
            for (auto const& [spawnId, actor] : Behavior.actors)
            {
                (void)actor;
                if (EffectiveControl(spawnId) == WojControl::Jev)
                    OwnedGuids.insert(spawnId);
            }
        for (auto const& [spellId, behavior] : Behavior.spells)
        {
            (void)spellId;
            Spells[behavior.entry].push_back(behavior.spell);
        }
        if (Behavior.schemaVersion == 3)
            for (auto const& [spawnId, spells] : Behavior.actorSpells)
                for (auto const& [spellId, behavior] : spells)
                {
                    (void)spawnId;
                    (void)spellId;
                    Spells[behavior.entry].push_back(behavior.spell);
                }
        return;
    }

    // v1 remains supported for existing murloc deployments and frozen
    // replays. v2/v3 policies never reach this branch merely because a v3
    // initial DB preflight has not happened yet.
    SceneId = "murlocs";
    SceneMapId = 0;
    if (!BehaviorPolicyValid)
        return;
    std::string raw = sConfigMgr->GetOption<std::string>("WorldOfJevs.OwnedGuids", "");
    std::stringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        std::string const token = Trim(item);
        if (token.empty())
            continue;
        if (!IsDigitsOnly(token))
        {
            LOG_ERROR("module", "mod-world-of-jevs: bad guid in OwnedGuids: '{}'", item);
            continue;
        }
        try
        {
            unsigned long const parsed = std::stoul(token);
            if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max())
            {
                LOG_ERROR("module", "mod-world-of-jevs: bad guid in OwnedGuids: '{}'", item);
                continue;
            }
            OwnedGuids.insert(static_cast<uint32_t>(parsed));
        }
        catch (std::exception const&)
        {
            LOG_ERROR("module", "mod-world-of-jevs: bad guid in OwnedGuids: '{}'", item);
        }
    }
    for (auto const& [spellId, behavior] : Behavior.spells)
    {
        (void)spellId;
        Spells[behavior.entry].push_back(behavior.spell);
    }
}

bool WojConfig::ValidateBehaviorBindings()
{
    // Legacy catalogs have already been completely validated by Load(). A
    // failed initial parse leaves schemaVersion at its zero-value and must
    // remain closed; it is not a successful no-op DB preflight.
    if (Behavior.schemaVersion != 3)
        return BehaviorPolicyValid;

    try
    {
        ValidateV3BindingsAgainstDb(Behavior);
        BehaviorPolicyValid = true;
        RebuildDerivedCatalog();
        return true;
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("module", "mod-world-of-jevs: published v3 behavior bindings rejected: {}", error.what());
        if (Behavior.schemaVersion == 3)
        {
            // First boot has no last-good catalog to restore.  Do not let an
            // unverified binding reach the global AI selector or bridge.
            BehaviorPolicyValid = false;
            BehaviorPolicyChanged = false;
            Enabled = false;
            OwnedGuids.clear();
            Spells.clear();
            RebuildDerivedCatalog();
        }
        return false;
    }
}

void WojConfig::Load(bool bindingDataReady)
{
    BehaviorPolicyChanged = false;
    WojBehaviorPolicy const previousBehavior = Behavior;
    Enabled = sConfigMgr->GetOption<bool>("WorldOfJevs.Enable", false);
    RecordOriginal = sConfigMgr->GetOption<bool>("WorldOfJevs.RecordOriginal", false);
    GatewayUrl = sConfigMgr->GetOption<std::string>("WorldOfJevs.GatewayUrl", "http://host.docker.internal:8088");
    // Дефолт здесь обязан совпадать с тем, что лежит в
    // mod_world_of_jevs.conf.dist, иначе
    // непрочитанный или потерянный файл конфигурации молча меняет
    // TimeoutMs и выглядит как случайные обрывы связи с гейтвеем.
    TimeoutMs = sConfigMgr->GetOption<uint32_t>("WorldOfJevs.TimeoutMs", 3000);

    // N4: clamp instead of trust. See the constants' comment above for why
    // both ends of the range are actual failure modes in WojBridge, not
    // just unreasonable values.
    if (TimeoutMs < MIN_TIMEOUT_MS || TimeoutMs > MAX_TIMEOUT_MS)
    {
        uint32_t const configured = TimeoutMs;
        TimeoutMs = std::clamp(TimeoutMs, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS);
        LOG_ERROR("module",
                   "mod-world-of-jevs: WorldOfJevs.TimeoutMs {} out of range [{}, {}] - clamped to {}",
                   configured, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS, TimeoutMs);
    }

    TickMs = sConfigMgr->GetOption<uint32_t>("WorldOfJevs.TickMs", 2000);
    CombatTickMs = sConfigMgr->GetOption<uint32_t>("WorldOfJevs.CombatTickMs", 2000);
    CombatFreshnessMs = sConfigMgr->GetOption<uint32_t>("WorldOfJevs.CombatFreshnessMs", 4500);
    IdleFreshnessMs = sConfigMgr->GetOption<uint32_t>("WorldOfJevs.IdleFreshnessMs", 5000);
    auto clampFreshness = [](uint32_t configured, char const* option)
    {
        uint32_t const clamped = std::clamp(configured, MIN_FRESHNESS_MS, MAX_FRESHNESS_MS);
        if (clamped != configured)
            LOG_ERROR("module", "mod-world-of-jevs: {} {} out of range [{}, {}] - clamped to {}",
                      option, configured, MIN_FRESHNESS_MS, MAX_FRESHNESS_MS, clamped);
        return clamped;
    };
    CombatFreshnessMs = clampFreshness(CombatFreshnessMs, "WorldOfJevs.CombatFreshnessMs");
    IdleFreshnessMs = clampFreshness(IdleFreshnessMs, "WorldOfJevs.IdleFreshnessMs");
    BehaviorFile = Trim(sConfigMgr->GetOption<std::string>("WorldOfJevs.BehaviorFile",
        "/azerothcore/env/dist/etc/modules/woj_behavior.json"));

    try
    {
        std::ifstream input(BehaviorFile, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open file");
        input.seekg(0, std::ios::end);
        std::streamoff const fileSize = input.tellg();
        if (fileSize < 0 || fileSize > MAX_BEHAVIOR_FILE_BYTES)
            throw std::runtime_error("behavior policy file exceeds 256 KiB");
        input.seekg(0, std::ios::beg);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        WojBehaviorPolicy parsed = ParseBehaviorPolicy(buffer.str());
        // World::SetInitialWorldSettings invokes OnAfterConfigLoad before
        // ObjectMgr::LoadCreatures, so first boot must defer this until
        // OnStartup. Reloads can and must reject before replacing last-good.
        if (bindingDataReady)
            ValidateV3BindingsAgainstDb(parsed);
        if (BehaviorPolicyValid)
        {
            // Смена состава сцены требует вернуть AI прежним creature и
            // атомарно очистить bridge/help/cache. Этого протокол reload
            // пока не делает, поэтому не выдаём частичную передачу владения
            // за hot reload: правила активной сцены обновляются, выбор
            // другой сцены применяется только после перезапуска.
            if (parsed.schemaVersion != 3 && Behavior.schemaVersion != 3 &&
                parsed.sceneId != Behavior.sceneId)
                throw std::runtime_error("active_scene change requires a world restart");
            if (parsed.revision < Behavior.revision)
                throw std::runtime_error("revision moved backwards");
            if (parsed.revision == Behavior.revision && parsed.canonical != Behavior.canonical)
                throw std::runtime_error("content changed without increasing revision");
            if (parsed.revision == Behavior.revision)
                parsed = Behavior;
            else
                BehaviorPolicyChanged = true;
        }
        else
            BehaviorPolicyChanged = true;
        bool const bindingChanged = !BehaviorPolicyValid || parsed.canonical != Behavior.canonical;
        Behavior = std::move(parsed);
        // An overlay is not durable.  Do not accidentally retain a switch
        // for an actor/group that disappeared in the newly validated catalog.
        for (auto it = GroupOverlays.begin(); it != GroupOverlays.end(); )
            it = Behavior.groupControls.count(it->first) ? std::next(it) : GroupOverlays.erase(it);
        for (auto it = ActorOverlays.begin(); it != ActorOverlays.end(); )
            it = Behavior.actors.count(it->first) ? std::next(it) : ActorOverlays.erase(it);
        if (Behavior.schemaVersion == 3)
        {
            std::unordered_set<uint32_t> const changed = ChangedBindingActors(previousBehavior);
            for (auto& [spawnId, binding] : Behavior.actors)
            {
                auto const old = previousBehavior.actors.find(spawnId);
                if (old != previousBehavior.actors.end() && !changed.count(spawnId))
                    binding.generation = old->second.generation ? old->second.generation : 1u;
                else
                    binding.generation = NextBindingGeneration(old == previousBehavior.actors.end()
                        ? 0u : old->second.generation);
            }
        }
        else if (bindingChanged)
            ++BindingGeneration;
        // V3 bindings refer to live DB spawns. At initial config load the
        // ObjectMgr has not loaded those rows yet, therefore ownership stays
        // closed until OnStartup validates them. Reloads validate before this
        // publication point and can retain the previous last-good catalog.
        BehaviorPolicyValid = Behavior.schemaVersion != 3 || bindingDataReady;
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("module", "mod-world-of-jevs: behavior policy '{}' rejected: {}",
                  BehaviorFile, error.what());
        if (!BehaviorPolicyValid)
            BehaviorPolicyChanged = false;
    }

    std::string mode = Trim(sConfigMgr->GetOption<std::string>("WorldOfJevs.Mode", "ours"));
    std::transform(mode.begin(), mode.end(), mode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode == "ours")
        Mode = WojMode::Ours;
    else if (mode == "original")
        Mode = WojMode::Original;
    else
    {
        Mode = WojMode::Ours;
        LOG_ERROR("module", "mod-world-of-jevs: bad WorldOfJevs.Mode '{}', using ours", mode);
    }

    // Part B: single descriptor string, empty = off. "owned" is the only
    // value understood so far; see WojKeepAlive.cpp for the parse and why
    // its dispatch is written to grow by adding cases, not by rewriting.
    KeepAlive = Trim(sConfigMgr->GetOption<std::string>("WorldOfJevs.KeepAlive", ""));

    RebuildDerivedCatalog();

    // This line is the only evidence the config file was actually read: it
    // must look different when a value silently fell back to its default
    // than when it was set on purpose, or a bad file and a working one are
    // indistinguishable in the log.
    //
    // timeoutMs добавлен сюда же намеренно: код и .conf.dist договорились
    // об одном и том же дефолте, но это ничего не стоит проверить на
    // живом стенде, а не только по исходникам, - без этой строки узнать,
    // какое TimeoutMs реально подхватил именно этот запуск worldserver,
    // можно было бы только косвенно, по задержке до первого таймаута.
    LOG_INFO("module",
             "mod-world-of-jevs: config loaded: enabled={} mode={} scene={} map={} gatewayUrl={} timeoutMs={} tickMs={} combatTickMs={} combatFreshnessMs={} idleFreshnessMs={} owning {} npc behaviorRevision={} behaviorValid={} behaviorChanged={}",
             Enabled, ModeName(), SceneId, SceneMapId, GatewayUrl, TimeoutMs, TickMs, CombatTickMs,
             CombatFreshnessMs, IdleFreshnessMs, OwnedGuids.size(),
             Behavior.revision, BehaviorPolicyValid, BehaviorPolicyChanged);

    if (Enabled && BehaviorPolicyValid && OwnedGuids.empty())
        LOG_ERROR("module",
                   "mod-world-of-jevs: WorldOfJevs.Enable=1 but OwnedGuids is empty - "
                   "the module is enabled but owns nothing and can never act");
    if (Behavior.schemaVersion != 3 && OwnedGuids.size() > 14)
        LOG_ERROR("module",
                  "mod-world-of-jevs: {} OwnedGuids exceed protocol 1.3 heal domain capacity 14; "
                  "heal_allies will be deterministically limited", OwnedGuids.size());
}
