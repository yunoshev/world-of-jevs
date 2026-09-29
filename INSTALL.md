# World of Jevs runtime integration (experimental)

Supported source revision: AzerothCore `4b5e842b6626b2587c19bc515a860f64ac1c5d32`
from `https://github.com/azerothcore/azerothcore-wotlk.git`. The C++ module is
statically linked; adding it needs a worldserver build. Keep AzerothCore's
database, maps, movement, combat, spells and persistence in charge.

1. Check out that revision using AzerothCore's normal setup instructions. Copy
   `module/mod-world-of-jevs` into its `modules/` directory. Configure and
   build `worldserver` as usual. The CMake output must contain
   `mod-world-of-jevs: public runtime hook applied`. This package has no
   headless dungeon or test-player source.
2. Set the installed `mod_world_of_jevs.conf` values for your deployment.
   `GatewayUrl` must be reachable from the worldserver and use `http://`.
   `BehaviorFile` must be an absolute path visible to the worldserver process.
   The sample starts in original-AI mode and uses placeholder spawn ID
   `123456789`; replace it with the real spawn ID of an entry-127 creature
   in your AzerothCore database before loading the policy. Other creature
   entries require matching sheets and policy rules.
3. Install Python 3.12 or newer and run `python -m pip install ./gateway`.
   Set `WOJ_BEHAVIOR_POLICY` to the same policy JSON, `WOJ_JEV_KEY` to a private
   file containing your OpenRouter key, and `WOJ_JOURNAL`/`WOJ_ACTIVITY` to
   writable files. Set `WOJ_BUDGET_SESSION_USD` to your chosen cap. This
   public gateway always fails closed when Jev cannot answer. Start
   `python -m uvicorn woj_gateway.app:app --host
   127.0.0.1 --port 8088` where the worldserver can reach it. Check
   `/healthz`: `behavior_policy.available` and `decision_available` must be
   true before switching a creature to Jev.
4. Start the worldserver. `.woj group murlocs mode jev` switches the sample
   group; `.woj group murlocs mode original` returns native AI ownership.
   `.reload config` reloads policy and most timing fields. A changed gateway
   URL or keep-alive setting still requires a worldserver restart. Inspect
   decisions and physical server events separately when checking results.

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

The gateway defaults to private development paths when the `WOJ_*` file-path
variables are unset; set all four paths explicitly for this package. The
sample has no API key. Original World of Jevs runtime source is offered under
GPL-2.0-or-later; `LICENSE` contains GPL version 2. Vendored
cpp-httplib and nlohmann/json have their MIT notices in `module/.../include/`.
