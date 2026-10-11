# Creating a World Partition Map and Verifying Server Streaming

World Partition maps (`.umap` and One-File-Per-Actor packages) are binary files, so they are not included in this repository.
After creating the map in the editor, you can run the `Tools/PopulateTestWorld.py` script to place test content automatically. (Based on the UE 5.8 UI.)

## 1. Creating the Map and Placing Test Content (about 5 minutes)

1. Launch the editor → **File → New Level → Open World** (choose a template with World Partition enabled).

2. The terrain and sample actors included in the template are not needed, so **remove them in the Outliner**. (Alternatively, create a *Basic* level and convert it by checking **World Settings → World Partition Setup → Enable World Partition**.)

3. Save the map at `/Game/Maps/L_MassBubbleWorld`. The name and path must match `GameDefaultMap`/`ServerDefaultMap` in `Config/DefaultEngine.ini`.

4. Check the **World Settings**:

   * Leave *Game Mode Override* empty (`AMassBubbleGameMode` is set as the project default).

   * *World Partition → Runtime Settings*: set the default `MainGrid` to **Cell Size = 12800 (128 m)** and Loading Range = 25600 (256 m).
     (Keeping the cell size identical to `UCrowdSettings::RegionSizeCm` (12800) is a prerequisite of this project.)

   * *Server streaming*: make sure server streaming is enabled. The project default is `wp.Runtime.EnableServerStreaming=1` in `DefaultEngine.ini`.

5. Select Tools → Execute Python Script… and run `Tools/PopulateTestWorld.py`.

   * **Key features of the script (`PopulateTestWorld.py`):**

     * **Background tick execution (`tick` mode):** The work is split into per-frame chunks so the editor does not freeze, and you can follow the progress in real time in the Output Log.

     * **Resume support:** Actors that already exist (matched by label) are skipped, so if the script is interrupted you can continue without creating duplicates.

     * **Checkpoint saving:** The package is saved automatically at a set interval (`SAVE_EVERY = 1000`) for added stability. (Running the script again while it is running cancels the existing job.)

     * **Result:** One large floor (`OptFloor`, `Is Spatially Loaded = false`), about 2,600 blocks to be streamed (`OptBlock_x_y`, covering roughly 2 km × 2 km), and one `OptPlayerStart` at the origin are created automatically.

6. The default bot orbit radius is 600 m (`BotOrbitRadiusCm`), so the whole map must be at least 1.2 km wide. If you are using a smaller map, reduce the radius in *Project Settings → Game → MassBubble Crowd → Bots*.

> **Note:** The crowd is not placed directly in the level. Mass entities are managed inside the server subsystem; the map only provides the ground and the actors that get streamed.

## 2. Verifying That Server Streaming Is Enabled

1. Verify in a **Dedicated Server** environment (see "Starting a Dedicated Server directly" in `README.md`, or use PIE *Play As Client*).

   * In a Listen Server environment the server doubles as the host client, so the difference between server streaming on and off is hard to see.

2. On the server console (Dedicated Server log window) or via the `-ExecCmds` command line, enter:

   ```
   wp.Runtime.EnableServerStreaming        # prints the current value (should be 1)
   wp.Runtime.ToggleDrawRuntimeHash2D      # visualizes the runtime hash cells (the command may differ depending on the version)

   ```

3. Move the player beyond a cell boundary (`AMassBubbleCharacter` can fly with WASD/EQ/Space, up to 30 m/s).

   * Check that the blocks in cells that are now far away are **unloaded correctly** in the server world. If server streaming is off, all cells stay in memory.

## 3. Verifying the Crowd and Streaming Source Integration

Enter the following commands on the server console to test:

```
opt.crowd.SpawnBots 8
opt.crowd.Stats

```

| Check | Expected result |
| ----- | ----- |
| **Bot orbit** | The World Partition cells around the bot positions are loaded on the server (`UWorldPartitionStreamingSourceComponent` is registered automatically as a server streaming source). |
| **Region info in `Stats`** (`regions(... active=…)`) | Up to 25 per player plus up to 25 per bot (summed when regions overlap). |
| **LOD tiers** (`LOD tiers`) | Agents near bots and players are High/Medium; distant ones switch to Off. |
| **Bot removal test** (wait 10+ seconds after `opt.crowd.ClearBots`) | Regions go from `despawning` → `inactive` and the `alive` count decreases. |
| **Revisit test** | When a bot returns to a place it passed before, the agents are restored with the same NetIds and near their previous positions (check the `Saved` path). |

> **Failure case check:** If you deliberately set `wp.Runtime.EnableServerStreaming 0` and restart, you can see the server force-load all cells, which increases memory usage and loading time and renders the streaming source anchors ineffective.

## 4. Placed Streaming Anchors (Optional)

If you place an `AMassBubbleStreamingAnchor` actor directly in the level, you can keep the population and cells of an important location (e.g., a town square) permanently loaded.

* You must change **World Partition → Is Spatially Loaded = false** in the Details panel. If it is left on, the anchor itself ends up inside the cell it is supposed to keep loaded, creating a circular dependency, and it may not work as intended.

* In client copies, the streaming source is automatically disabled at `BeginPlay`. (A client should only stream based on its own player's position.)

## 5. Troubleshooting

| Symptom | Possible causes and fixes |
| ----- | ----- |
| **Map not found at editor startup** | The map may not be saved at `/Game/Maps/L_MassBubbleWorld`. Check the save location or fix the map settings in `DefaultEngine.ini`. |
| **Crowd is not visible on screen** | Run `opt.crowd.RenderStats 1` in the client console. If it shows `bubble=0`, the bubble has not arrived (check for `CrowdBubble … created` in the server log). If `bubble>0` but nothing is visible, the agents were spawned below the floor or there is a problem with the mesh settings. |
| **`SpawnBots` command is ignored** | You most likely ran it in a client window console. It must be run on the **server world** console (the Listen Server window or the Dedicated Server console). |
| **Crowd subsystem not visible in the server log** | Check whether this is a Game/PIE world, or whether the session is in the `NM_Client` state. |
| **Blocks are not unloaded on the server** | Either server streaming is off, or a bot or anchor is keeping that cell within its streaming source range. |
