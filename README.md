# GTrain

A server-side [DDNet](https://github.com/ddnet/ddnet) mod for training Gores maps: practice random parts of a map over and over, alone or against friends.

## How it works

- Every spawn puts you at a random spot between the start and the finish line, always above freeze. You hover frozen for one second, then play.
- A personal blue flag is placed a set number of path tiles ahead (100 by default). A particle trail shows the shortest path to it.
- Reach the flag to capture it. The capture message shows how many fails it took (and your time when training alone), then you get a new spot.
- The race timer restarts with every attempt, so it always shows the time of your current try.
- Kill for a new spot, or `/r` to retry the same spot and flag. Every retry counts as a fail.

## Commands

| Command | Description |
| --- | --- |
| Kill | New random spot and flag |
| `/r`, `/retry` | Retry the current spot and flag (`/tc`, `/telecursor` and `/invincible` work too) |
| `/fight <player>` | Join a player's fight; `/fight` alone leaves it |
| `/distance <1-500>` | Your goal distance in tiles; `0` uses the server default |
| `/nodj` | Toggle starting attempts without double jump; touching the ground gives it back |
| `/scorelimit <0-1000>` | Points needed to win a fight (default 10, `0` for no limit) |
| `/shotgun`, `/grenade`, `/laser`, `/ninja` | Toggle keeping that weapon in every attempt; ninja never runs out |
| `/freeplay` | Toggle training without flags, paths and captures |
| `/help` | List these commands |

`/distance` and `/scorelimit` show the current value without an argument. In a fight, only the leader can change these settings, and they apply to the whole group. Leaving a fight keeps its settings, and joining one adopts its settings, including removing weapons it has disabled.

## Fights

- Everyone in a fight trains at the same spot with the same flag. Joining a player who is already fighting merges both groups.
- Each capture scores a point and announces the standings. The first to reach the score limit wins the fight, and all scores reset.
- The leader's death starts a new attempt for everyone. Other members respawn at the current spot.
- `/r` only retries the player who used it, the leader included.
- Fights show up as teams in the scoreboard, with points as scores. The leader is shown as `*name*`; the server browser keeps the real name.
- Leaving a fight resets your score. A fight with one player left ends and that score resets too.
- Paused and `/spec` players can fight; players in the spectator team cannot.

## Pathfinding

The walkable tile graph is built once when the map loads. Paths never go through solid, freeze or death tiles, and diagonal steps can't cut corners. Teleporters count as path steps; checkpoint teleporters are skipped because they depend on each player's history. Every new flag stores its distance to all nearby tiles, so redrawing the path when you leave it costs no search. If you stray more than the goal distance plus 32 tiles behind your start, the path is hidden until you come back.

## Server setup

Build the server as described in the [DDNet building guide](docs/BUILDING.md) and start it with a Gores map that has a start and a finish line:

```sh
./DDNet-Server "sv_map <map>"
```

`sv_gametype` defaults to `gtrain`. The mod forces `sv_solo_server 1` and `sv_kill_delay 0`, and disables `/team` and `/practice` to keep fights intact.

| Setting | Default | Description |
| --- | --- | --- |
| `sv_gtrain_goal_distance` | 100 | Goal distance in tiles for players who didn't set `/distance` |
| `sv_gtrain_path_clear_radius` | 64 | Particle-free radius around the player in world units (32 per tile); 0 shows the whole path |
| `sv_gtrain_path_particle_spacing` | 192 | Distance between path particles in world units |
| `sv_require_chat_flag_to_chat` | 0 | Set to 1 so new players must show the chat bubble before chatting during their first 20 seconds; commands are unaffected (from [ddnet-insta](https://github.com/ddnet-insta/ddnet-insta/commit/9b7c415b19510dc04157dfaa05596c077ca02ec6)) |
