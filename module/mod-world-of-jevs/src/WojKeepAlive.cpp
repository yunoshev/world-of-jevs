#include "WojKeepAlive.h"
#include "WojConfig.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // Small on purpose, for this phase: one murloc is free, a scene is
    // tolerable, a zone is hundreds of always-ticking AI updates, and a
    // continent would put the stand on its knees (docs/world-liveness.md).
    // Raise it deliberately later, alongside whatever descriptor needs it
    // (guids:, area:), not as a side effect of adding one.
    constexpr size_t MAX_KEEP_ALIVE_CREATURES = 32;

    // Round 2: the one guid list both Apply() and Sweep() work from.
    // Populated exactly once, by Apply(), and only when the request was at
    // or under MAX_KEEP_ALIVE_CREATURES - see the comment on Sweep() in the
    // header for why an empty list here is the entire fix for NEW-1.
    std::vector<uint32_t> gKeepAliveGuids;

    std::string JoinGuids(std::vector<uint32_t> const& guids)
    {
        std::string out;
        for (size_t i = 0; i < guids.size(); ++i)
        {
            if (i)
                out += ", ";
            out += std::to_string(guids[i]);
        }
        return out;
    }

    // M1: identifies one grid, so activating several guids that land in the
    // same grid is counted once, not once per guid. Ordered so it can live
    // in a std::set - dedup is the only thing this needs to do.
    struct GridKey
    {
        uint32_t mapId;
        uint32_t gridX;
        uint32_t gridY;

        bool operator<(GridKey const& other) const
        {
            if (mapId != other.mapId)
                return mapId < other.mapId;
            if (gridX != other.gridX)
                return gridX < other.gridX;
            return gridY < other.gridY;
        }
    };

    // Loads this guid's grid and flips setActive(true) on the Creature it
    // produces. Order matters and is not stylistic: WorldObject::setActive
    // (Object.cpp:1091-1109) returns at line ~1101 on `!IsInWorld()`, and
    // before the grid is loaded the creature has not been created at all -
    // activating first would silently set a flag on nothing.
    //
    // M1: also reports which grid it loaded, via outGrid, so the caller can
    // name the real cost (a whole grid, not "one creature") instead of
    // guessing at it.
    //
    // Apply()-only: this is the one-shot, right-after-LoadGrid call, where a
    // guid not being found really does mean the spawn is not where
    // creature_data says it is - see Sweep()/ReactivateOne() below for the
    // periodic counterpart, which must not repeat this LOG_ERROR.
    bool ActivateOne(uint32_t guid, GridKey& outGrid)
    {
        CreatureData const* data = sObjectMgr->GetCreatureData(guid);
        if (!data)
        {
            LOG_ERROR("module",
                       "mod-world-of-jevs: keep-alive guid {} has no creature_data in the world "
                       "database - skipped", guid);
            return false;
        }

        Map* map = sMapMgr->CreateBaseMap(data->mapid);
        // CreateBaseMap never returns null for a real continent id - map 0
        // always exists - but the guard costs nothing and the alternative
        // (dereferencing null two lines down) would take the whole world
        // thread down with it, not just this feature.
        if (!map)
        {
            LOG_ERROR("module", "mod-world-of-jevs: keep-alive guid {} - CreateBaseMap({}) returned null",
                       guid, data->mapid);
            return false;
        }

        // Instance maps are activated by real Player presence in the
        // player_path harness.  A MapInstanced base is only a factory and
        // cannot stand in for a concrete dungeon copy; trying to keep a DB
        // spawn alive there would both fail and blur the acceptance mode.
        if (map->Instanceable())
        {
            LOG_ERROR("module", "mod-world-of-jevs: keep-alive guid {} belongs to instanceable map {} - skipped",
                      guid, data->mapid);
            return false;
        }

        // One-shot and permanent: this core never unloads a grid outside of
        // Map::UnloadAll() at map shutdown (docs/world-liveness.md), so
        // there is no matching unload to pair this with, and no harm in
        // calling it unconditionally on every server start.
        map->LoadGrid(data->posX, data->posY);

        GridCoord const gc = Acore::ComputeGridCoord(data->posX, data->posY);
        outGrid = GridKey{data->mapid, gc.x_coord, gc.y_coord};

        // A multimap because multispawn (id2/id3) can back more than one
        // live Creature with the same spawn id; activate every one found.
        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
        if (bounds.first == bounds.second)
        {
            LOG_ERROR("module",
                       "mod-world-of-jevs: keep-alive guid {} not found on map {} after loading its "
                       "grid at ({:.1f}, {:.1f}) - the spawn is not where creature_data says it is",
                       guid, data->mapid, data->posX, data->posY);
            return false;
        }

        for (auto it = bounds.first; it != bounds.second; ++it)
            it->second->setActive(true);

        return true;
    }

    // Sweep()'s counterpart to ActivateOne() above, built for a periodic
    // call against a live world instead of a one-shot call right after
    // LoadGrid: no LoadGrid (nothing left to load, see the header comment
    // on Sweep()), and - the point of this function existing separately -
    // no LOG_ERROR when the guid is not found. Absence here has an ordinary
    // cause ActivateOne never has to consider: the corpse is still lying
    // there (spawntimesecs) and the new Creature simply does not exist yet.
    // Returns whether it actually flipped anything, so the caller only logs
    // when there is something real to report.
    bool ReactivateOne(uint32_t guid)
    {
        CreatureData const* data = sObjectMgr->GetCreatureData(guid);
        if (!data)
            return false; // gone from the DB entirely; Apply() already said so at startup if that was already true then

        Map* map = sMapMgr->CreateBaseMap(data->mapid);
        if (!map || map->Instanceable())
            return false;

        bool reactivatedAny = false;
        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            if (!it->second->isActiveObject())
            {
                it->second->setActive(true);
                reactivatedAny = true;
            }
        }
        return reactivatedAny;
    }

    // M6 (round 3): the previous version of this comment claimed "every
    // spawn-id-tracked creature in the grid, activated or not, now ticks
    // regardless of what we asked for" - checked against the engine and
    // wrong. Map::UpdateNonPlayerObjects keeps an object in the update list
    // only while Creature::IsUpdateNeeded() is true
    // (Creature.cpp:3991-4019): active objects (the ones we activated),
    // cell-marked, in combat, visible to a player, temp summons,
    // WAYPOINT-motion, evade, and non-leader formation members. Ordinary
    // RANDOM wandering (MovementType = 1, what our neighbouring murlocs
    // actually are) is none of those, so it does NOT tick just from sharing
    // a grid. docs/world-liveness.md already said this correctly ("a
    // creature in a loaded chunk with no player nearby simply does not
    // tick") - only this file's comment and log line disagreed with it.
    //
    // Counts both numbers live off the map's own store and the real
    // predicate, not an estimate: outLoaded is every spawn-id-tracked
    // creature resident in the grid (the honest total LoadGrid pulled in);
    // outTicking is how many of those IsUpdateNeeded() actually returns
    // true for right now. On guid 89965's grid that is 55 loaded and 5
    // ticking: the one we activated plus four WAYPOINT movers. 55 was
    // always right and is still reported; what a previous version got
    // wrong was claiming all 55 tick. (An earlier draft of this very
    // comment then said "1 and 12", which is wrong twice over - ticking
    // cannot be smaller than the creature we activated, and 12 is the
    // WAYPOINT count within 350 yards, which spans four grids while only
    // one is loaded.)
    void CountResidents(std::set<GridKey> const& grids, uint32_t& outLoaded, uint32_t& outTicking)
    {
        outLoaded = 0;
        outTicking = 0;
        for (GridKey const& grid : grids)
        {
            Map* map = sMapMgr->CreateBaseMap(grid.mapId);
            if (!map)
                continue; // cannot happen for a grid we just loaded, but see the null check above
            for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            {
                GridCoord const gc = Acore::ComputeGridCoord(creature->GetPositionX(), creature->GetPositionY());
                if (gc.x_coord == grid.gridX && gc.y_coord == grid.gridY)
                {
                    ++outLoaded;
                    if (creature->IsUpdateNeeded())
                        ++outTicking;
                }
            }
        }
    }

    // The only place WorldOfJevs.KeepAlive's descriptor is ever dispatched,
    // round 2 onward (NEW-4): Sweep() no longer has its own copy - it just
    // reads gKeepAliveGuids, which Apply() below populates from exactly
    // this function's result. Only "owned" exists in this phase; a future
    // "guids:1,2,3" or "area:map,x,y,radius" is a new branch added here,
    // once - everything downstream of `requested` in Apply() is already
    // description-agnostic.
    bool ResolveRequested(std::string const& spec, std::vector<uint32_t>& out)
    {
        WojConfig const& config = WojConfig::Instance();
        if (spec == "owned")
        {
            out.assign(config.OwnedGuids.begin(), config.OwnedGuids.end());
            return true;
        }

        constexpr char const* prefix = "groups:";
        if (spec.rfind(prefix, 0) != 0)
            return false;

        std::unordered_set<uint32_t> selected;
        std::string const groups = spec.substr(std::char_traits<char>::length(prefix));
        size_t begin = 0;
        while (begin <= groups.size())
        {
            size_t const comma = groups.find(',', begin);
            std::string const id = groups.substr(begin,
                comma == std::string::npos ? std::string::npos : comma - begin);
            uint32_t unusedMapId = 0;
            if (id.empty() || !config.GroupMapId(id, unusedMapId))
                return false;
            std::unordered_set<uint32_t> const actors = config.GroupActors(id);
            selected.insert(actors.begin(), actors.end());
            if (comma == std::string::npos)
                break;
            begin = comma + 1;
        }
        out.assign(selected.begin(), selected.end());
        std::sort(out.begin(), out.end());
        return !out.empty();
    }
}

void WojKeepAlive::Apply()
{
    std::string const& spec = WojConfig::Instance().KeepAlive;

    if (spec.empty())
    {
        LOG_INFO("module", "mod-world-of-jevs: keep-alive off (WorldOfJevs.KeepAlive is empty)");
        return;
    }

    // `owned` is retained for compatibility. `groups:a,b` lets world-map
    // soak actors stay active while dungeon groups remain player-driven.
    // Everything below works from the resulting explicit spawn list.
    std::vector<uint32_t> requested;
    if (!ResolveRequested(spec, requested))
    {
        LOG_ERROR("module",
                   "mod-world-of-jevs: WorldOfJevs.KeepAlive '{}' not understood - keep-alive NOT applied",
                   spec);
        return;
    }

    if (requested.size() > MAX_KEEP_ALIVE_CREATURES)
    {
        // Refuse all of it, not just the excess: a half-alive world looks
        // like a working one, which is worse than an inert one, because
        // nothing about it says "something was cut short". gKeepAliveGuids
        // is deliberately left untouched (empty) here - that is what makes
        // Sweep() refuse the exact same way later, for every guid, forever,
        // without re-checking this cap itself (NEW-1).
        LOG_ERROR("module",
                   "mod-world-of-jevs: keep-alive '{}' would need to activate {} creatures, over the "
                   "cap of {} - refusing to activate any of them",
                   spec, requested.size(), MAX_KEEP_ALIVE_CREATURES);
        return;
    }

    // Round 2: the list Sweep() will keep working from for the rest of this
    // worldserver's life, every guid the descriptor resolved to - not only
    // the ones ActivateOne() below happens to succeed on right now. A guid
    // that fails here because its corpse is already down when the server
    // boots is exactly the case Sweep() exists to pick back up later.
    gKeepAliveGuids = requested;

    std::vector<uint32_t> activated;
    std::set<GridKey> gridsLoaded;
    for (uint32_t guid : requested)
    {
        GridKey grid;
        if (ActivateOne(guid, grid))
        {
            activated.push_back(guid);
            gridsLoaded.insert(grid);
        }
    }

    // The log line must tell "enabled and it worked" apart from "enabled
    // and it did not", or the second - the dangerous one, because it looks
    // exactly like startup succeeding - would be indistinguishable from
    // keep-alive being off in the first place.
    if (activated.empty())
        LOG_ERROR("module",
                   "mod-world-of-jevs: keep-alive enabled ('{}') but activated 0 of {} requested creature(s)",
                   spec, requested.size());
    else
    {
        // M6: name the real cost, computed, not eyeballed - loaded is every
        // spawn-id-tracked creature LoadGrid pulled into the grid; ticking
        // is how many of them Creature::IsUpdateNeeded() actually returns
        // true for right now (see CountResidents above). These are usually
        // different numbers, and the gap is exactly the point: it is what
        // separates "resident" from "costs a tick".
        uint32_t loaded = 0, ticking = 0;
        CountResidents(gridsLoaded, loaded, ticking);
        LOG_INFO("module",
                  "mod-world-of-jevs: keep-alive activated {} creature(s) on {} grid(s), {} creature(s) "
                  "loaded there, {} of them actually ticking (Creature::IsUpdateNeeded): {}",
                  activated.size(), gridsLoaded.size(), loaded, ticking, JoinGuids(activated));
    }
}

void WojKeepAlive::Sweep()
{
    for (uint32_t guid : gKeepAliveGuids)
    {
        if (ReactivateOne(guid))
            LOG_INFO("module", "mod-world-of-jevs: keep-alive guid {} re-activated after respawn", guid);
    }
}
