# World of Jevs: AzerothCore integration and recording modes

Status: experimental integration. The [public package](../INSTALL.md) has been
built and cold-booted against the pinned AzerothCore revision, without the
private dungeon harness. A same-process operator command cycle confirmed
`original → jev → original` and config reload acknowledgements. No active NPC
or player was present in that package smoke test, so live handoff, physical
actions and a full client playthrough are not established by it.

## Recommended integration

Keep AzerothCore as the authority for maps, activation, movement, combat,
spells, damage, loot and persistence. Add World of Jevs as a module; do not
patch the pinned `core/` source. Keep the model gateway separate from the
worldserver and keep its API key out of the module, policy, logs and examples.
Build the module into the worldserver once. Subsequent creature ownership,
brief and rule changes should be data reloads, not C++ rebuilds.

Put each creature's readable brief and abilities in a sheet, and bind actual
spawns or verified runtime summons to a policy group. Start one group at a
time. Keep the same physical server logs in both modes so that comparisons
are about casts, damage, movement and encounter progression—not only a model
answer. Apply a bounded provider budget and surface unavailable decisions;
never silently substitute scripted choices while claiming Jev control.

For OpenRouter, keep provider-specific HTTP handling in the gateway, not in
AzerothCore. Supply the API key through a private secret file, pin the Jev
model used for a comparison, set explicit timeouts and a spending cap, and
record reported usage, latency and failures separately from the game
server's action-delivery result. A provider response is not a physical game
event. If model fallback is ever enabled, every substituted model must be
identified in the decision record; otherwise the comparison is not
Jev-only.

## Mode 1: original AI + recording

Set `WorldOfJevs.RecordOriginal = 1` and reload the module config. Leave a
configured actor or group in `original` mode. Its normal AzerothCore AI stays
installed; World of Jevs does not send it to Jev or replace its actions.
For active configured original-AI creatures, the module builds the full
versioned request through the same snapshot serializer used for Jev-controlled
creatures. It writes an `original_jev_request` JSON event at the configured
decision cadence (currently about two seconds), including the character's
policy binding, observed world state, abilities, targets and triggers. The
native AI still makes and executes every decision; the recorder never calls
the model. `RecordOriginal = 0` disables this extra recording.

Run the bundled `scripts/woj-export-original-prompts --log <WojCombat.log> --policy
<matching-policy.json> --out <new-file.jsonl>` to pass those recorded requests
through the gateway's **same** request validator and prompt builder used for
live Jev calls. Each exported row contains the resulting model-facing
`state/questions` payloads, without a model answer or provider charge. An
Original-AI run does not reveal SmartAI's private choice merely by producing
a prompt. Physical casts, attacks, speech and movement may be analysed
separately, but ambiguous or unobserved decisions are not training labels.

## Mode 2: Jev owns the decisions

Switch the same group or a single actor to `jev` through the existing
operator overlay. The module's AI selector installs `WojAI` for that actor;
the gateway receives compact character context, current observable state and
eligible typed choices. Jev selects an intent, and the executor revalidates
it against the live world before asking AzerothCore to act. An unavailable
or unusable model response is logged as a failed decision, not replaced by
the original AI. Switching back to `original` restores native AI ownership.

Use the first mode to collect Jev-format inputs from original-AI gameplay and
physical outcomes. Use the second to compare the same authored actors under
Jev. Neither an observation nor a successful model response alone proves
that an ability executed or that the dungeon is accepted.

## Validation boundary

The public module compiled and cold-booted against the pinned AzerothCore
revision with no internal harness source. Operator commands changed a group's
mode `original → jev → original` and reloaded config on the same worldserver
process. The public gateway's isolated checks rejected unavailable Jev and
exhausted-budget decisions without heuristic substitution. A bundled exporter
test produced model-facing questions from a recorded-format original-AI input
without calling the provider.

An active NPC was not present in that package smoke test. Physical action
delivery, a changed policy taking effect during combat, runtime-summon
identity and a real-client encounter still need independent verification on
the packaged build. Do not interpret a command acknowledgement or model
response as proof that an NPC acted.

## How the AzerothCore integration works technically

The existing `AllCreatureScript::GetCreatureAI` hook can return a `WojAI`
for a Jev-owned creature. Returning `nullptr` for an original-owned creature
lets AzerothCore continue to its normal native AI factory. No `core/` source
edit is required. `AllCreatureScript::OnAllCreatureUpdate` observes original
creatures after their normal update without supplying a replacement AI.
`WojRuntimeBinding` resolves the exact actor identity, and the versioned
policy determines effective control. `WojCombatLog` records shadow Jev-format
requests and physical events; the bridge sends HTTP only for Jev-owned
decision requests. The current module is statically linked, so
adding the integration initially needs a worldserver build; ordinary policy
and ownership changes are intended to reload on the running server.
