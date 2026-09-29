# Add Jev to your AzerothCore server

You need an existing AzerothCore server, Python 3.12+ and **your own OpenRouter
key with access to Jev**. This repository includes the module, gateway and
the complete configured The Deadmines data set. No project key, test player
or route automation is needed.

## 1. Add the module and build once

Clone this repository. Copy `module/mod-world-of-jevs` into your AzerothCore
`modules/` directory, reconfigure CMake and rebuild/install `worldserver` as
usual ([standard module installation](https://www.azerothcore.org/wiki/installing-a-module)).
CMake must report `mod-world-of-jevs: public runtime hook applied`.
No core source edits or module-specific SQL migration are required.

The checked core revision is `4b5e842b6626b2587c19bc515a860f64ac1c5d32`
([AzerothCore WotLK](https://github.com/azerothcore/azerothcore-wotlk)).
The supplied spawn/gameobject bindings use its standard world database.
On a custom database with changed spawn IDs, remap those IDs first. Other
core revisions need a compatibility check; do not downgrade a live database.

## 2. Point the module at the dungeon data

Copy `examples/woj_behavior.json` to a writable deployment location, such as
`/srv/world-of-jevs/woj_behavior.json`. It contains the Deadmines groups,
spells, boss phases, summons and door-event bindings, initially on original AI.
Edit the installed `etc/modules/mod_world_of_jevs.conf`:

```ini
[worldserver]
WorldOfJevs.Enable = 1
WorldOfJevs.RecordOriginal = 0
WorldOfJevs.GatewayUrl = "http://127.0.0.1:8088"
WorldOfJevs.BehaviorFile = "/srv/world-of-jevs/woj_behavior.json"
WorldOfJevs.KeepAlive = ""
```

Keep the other shipped timing defaults. Keep native instance scripts enabled.
If the gateway is in another container/host, use its private reachable address
instead of `127.0.0.1`. Do not expose the gateway directly to the Internet.

## 3. Start the gateway with your key

From the cloned repository:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install ./gateway
```

Save **your** OpenRouter key in a private file outside the repository, readable
only by the gateway user. Create a writable log directory. Set all four file
paths explicitly; gateway and worldserver must read the same policy content:

```sh
export WOJ_BEHAVIOR_POLICY=/srv/world-of-jevs/woj_behavior.json
export WOJ_JEV_KEY=/srv/world-of-jevs/secrets/openrouter-key
export WOJ_JOURNAL=/srv/world-of-jevs/logs/decisions.jsonl
export WOJ_ACTIVITY=/srv/world-of-jevs/logs/activity.jsonl
export WOJ_BUDGET_SESSION_USD=1.00
python -m woj_gateway.policy "$WOJ_BEHAVIOR_POLICY"
python -m uvicorn woj_gateway.app:app --host 127.0.0.1 --port 8088
```

Choose your own spending cap. Check `curl http://127.0.0.1:8088/healthz` in
another terminal: `behavior_policy.available` and `decision_available` must
be true. This is local readiness, not a provider credit/key check. Provider
errors are logged; the gateway does not replace missing Jev decisions with
heuristics.

## 4. Enable The Deadmines

In an activated Python environment, from the cloned repository:

```sh
python scripts/woj-deadmines-mode /srv/world-of-jevs/woj_behavior.json jev
```

Start worldserver, or run `.reload config` in GM chat if it is already running.
The gateway reloads the same policy automatically. Enter The Deadmines with
your character/group: creatures activate around players normally. This does
not change player health, equipment or dungeon balance settings.

To switch the entire dungeon back, run the helper with `original` instead of
`jev`, then `.reload config`. No rebuild is required. A single group can be
overridden with `.woj group deadmines_rhahk_boss mode original` or `mode jev`;
use `mode inherit` to clear an override and follow the file again. Clear any
group/NPC overrides before expecting a whole-policy switch to affect them.
Worldserver console commands omit the leading dot.

## Cards and request examples

The [NPC atlas](https://yunoshev.github.io/world-of-jevs/) contains all unique
cards, each with a link to a saved model-facing request. For example,
[this Defias Blackguard JSON](https://yunoshev.github.io/world-of-jevs/data/examples/defias-blackguard-vancleefs-static-guards.json)
contains `model`, `state` and `questions`: character context, an observed
situation and eligible choices. It is one example, not a fixed request reused
throughout combat. Runtime briefs/abilities live in
`gateway/woj_gateway/sheets/`; bindings and rules live in the policy JSON.

## Optional: record inputs with original AI

For original-AI recording, leave the group in `original`, set
`WorldOfJevs.RecordOriginal = 1`, reload the module config, and collect its
`WojCombat` JSONL log. Add `examples/worldserver.woj-logging.conf` to the
`[worldserver]` section of `worldserver.conf` so that
`Logger.module.woj.combat` writes `WojCombat.log` in the server log directory.
Convert the captured input with
`python scripts/woj-export-original-prompts --log <WojCombat.log> --policy
<matching-woj_behavior.json> --out <new-file.jsonl>`. The exporter uses the
gateway's request validator and prompt builder without calling a provider.
Rows remain unlabelled observations; physical events need separate
correlation before they can serve as original-AI decision labels.

## How it connects

The statically linked C++ module uses the existing creature-AI selection hook,
collects observations and asynchronously sends them to the Python gateway.
The gateway builds the model questions and returns a typed choice. The module
revalidates it; AzerothCore executes movement, attacks and spells. No game
thread waits synchronously for the model.

Policy edits need a higher `revision` and `.reload config`. The helper handles
the revision when switching modes. Gateway URL/keep-alive changes still need
a worldserver restart. Reinstall the gateway after changing its packaged
creature sheets, or use an editable Python installation.

The public runtime was compiled and booted on the pinned core; the full data
export is validated offline. Active-NPC testing of this exact public install
is a separate check. Original World of Jevs runtime source is GPL-2.0-or-later;
`LICENSE` contains GPL version 2. Vendored cpp-httplib and nlohmann/json retain
their MIT notices in `module/mod-world-of-jevs/include/`.
