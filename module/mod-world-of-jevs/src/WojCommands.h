#ifndef WOJ_COMMANDS_H
#define WOJ_COMMANDS_H

#include <cstdint>
#include <unordered_set>
#include "WojConfig.h"

uint32_t WojReinitializeOwned(char const* reason = "mode");
uint32_t WojReinitializeTransition(WojBehaviorPolicy const& previous, char const* reason);
uint32_t WojReinitializeActors(WojBehaviorPolicy const& previous,
                               std::unordered_set<uint32_t> const& candidates, char const* reason);
uint32_t WojReloadBehaviorOwned();
void AddWojCommandScripts();

#endif
