#ifndef WOJ_RUNTIME_IDENTITY_H
#define WOJ_RUNTIME_IDENTITY_H

#include "Unit.h"
#include "WojTypes.h"

// A low/raw ObjectGuid alone is not a process-wide actor identity: map 36 can
// have several live instance maps with the same static spawn.  This helper is
// intentionally usable for Creature and other Units participating in help.
inline WojActorKey WojRuntimeKey(Unit const* unit)
{
    return unit ? WojActorKey{unit->GetMapId(), unit->GetInstanceId(), unit->GetGUID().GetRawValue()}
                : WojActorKey{};
}

#endif
