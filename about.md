# Path Builder

Playtest your level in the editor, then tap **AUTO** to build blocks and spikes around the path you took.

- Every editor playtest records the player's path, gamemode, gravity, ground contact and clicks.
- **AUTO** places floor/ceiling blocks, jump spikes and ship/wave/UFO corridors, checking every frame of your hitbox so nothing placed would kill you on that exact path.
- **SET** opens the settings (style, gamemodes, spike count, clearance, corridor margins, object IDs).
- **BOT** (editor playtest only) replays the clicks of your last hand-played playtest on the exact same physics steps, so you can check a tight layout without having to hit it yourself.
- **CHK** moves each saved click one physics step earlier/later and replays it, listing every click that is not frame perfect.
- **Showcase Bot** (setting) replays the saved clicks in real play for showcase videos. Safe mode is forced while it is on: no progress, completions, stats or verification are saved.
- Everything placed is one undo step and is selected afterwards.

Supported: cube, robot, ship, swing, wave, ball, spider, UFO.
