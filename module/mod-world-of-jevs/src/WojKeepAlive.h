#ifndef WOJ_KEEP_ALIVE_H
#define WOJ_KEEP_ALIVE_H
#include <cstdint>

// Wakes up parts of the world with nobody watching, per WorldOfJevs.KeepAlive.
// Knows nothing about the gateway or decisions - the only thing it does is
// load a grid and flip setActive(true) so Map::UpdateNonPlayerObjects picks
// the creature up every tick regardless of players nearby. See
// docs/world-liveness.md for why both steps are needed and in that order.
//
// Must run on the game thread. It reads WojConfig directly, so - like
// WojConfig itself - it must never be called from any other thread.
namespace WojKeepAlive
{
    // Full pass: descriptor resolution, the safety cap
    // (MAX_KEEP_ALIVE_CREATURES), LoadGrid, find, setActive, and the one
    // summary log line. Called exactly once, from OnStartup - see
    // WojScriptLoader.cpp.
    void Apply();

    // Round 2 (NEW-1/NEW-3/NEW-4/NEW-5): the self-heal this used to be
    // (WojAI::ShouldKeepAlive, called from WojAI::UpdateAI) lived in the
    // wrong place - WojAI does not exist at all when WorldOfJevs.Enable=0
    // (NEW-3), does not know about the safety cap (NEW-1), runs under a
    // 250us tick budget that a one-time setActive() has no business being
    // charged against (NEW-5), and needed its own copy of the descriptor
    // dispatch to do any of this (NEW-4). Keep-alive is a world-level
    // mechanism, not an attachment to our AI, so it moves to
    // WorldScript::OnUpdate, roughly every 10s - see the call site for why
    // not every tick.
    //
    // No LoadGrid here: grids never unload in this core, so Apply() already
    // did the only loading that will ever be needed (docs/world-liveness.md).
    // Sweep() just finds each guid Apply() resolved and flips setActive(true)
    // if a dynamic-mode respawn reset it to false. Deliberately silent when
    // a guid is not found: between death and respawn the Creature genuinely
    // does not exist yet (spawntimesecs - five minutes for guid 89965), and
    // reporting that as an error would turn one routine death into roughly
    // spawntimesecs/10 panic lines. Only Apply(), which runs right after
    // LoadGrid and therefore expects to find what it just loaded, treats a
    // miss as an error. Sweep() only ever logs when it actually reactivates
    // something.
    //
    // Iterates the exact list Apply() resolved, capped the exact same way:
    // Apply() only keeps that list when the request was at or under
    // MAX_KEEP_ALIVE_CREATURES, so a request refused for exceeding the cap
    // leaves nothing here for Sweep() to find either - the same predicate
    // governs both ends without being checked twice. This is also the fix
    // for the old duplicate-dispatch problem (NEW-4): "owned" vs a future
    // "guids:"/"area:" is decided exactly once, in Apply(); Sweep() never
    // touches WorldOfJevs.KeepAlive again.
    //
    // Does not check WorldOfJevs.Enable, on purpose, same as Apply(): this
    // is about whether the world ticks the spawn at all, not about who is
    // deciding its actions - the comment on the OnStartup call site already
    // promises exactly this independence (NEW-3).
    void Sweep();
}
#endif
