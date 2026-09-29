# Why 8,000 Deadmines decisions did not call the model again

In a 56-minute Jev-only run through The Deadmines, the server asked for an NPC decision about **27,500 times**. The decision journal recorded **19,521 new Jev decisions** and about **8,000 requests answered from a recent-decision cache**. Those 8,000 were not refusals: the server received a previously authorized answer for the same creature and situation. The counts come from different log boundaries, so their rounded figures are not meant to add up exactly.

## The cache I use today

An NPC can ask again before anything relevant has changed. The gateway compares its current decision snapshot—including available choices, targets, behavior state and policy revision—with the snapshot that produced its recent Jev answer. A match for the **same NPC in the same world life** can reuse that answer for at most **15 seconds**. A new target, changed action eligibility, policy change, or a new creature life requires a new decision. The game server still checks that the chosen action can be executed in the current world.

That is a small, conservative cache of a *recent decision*, not a library of “what this species always does.” It avoids asking Jev an identical question twice in quick succession. In this run it accounted for roughly **29% of decision requests** (about 8,000 of 27,500), without making those requests failures or new paid model calls. Some new decisions themselves require more than one provider call, which is why **19,633 provider calls** is slightly higher than the new-decision count.

For now, the measured **$0.69** model spend for the complete run already includes the benefit of the short-lived cache. It is one observed run, not a price promise for every dungeon or server.
