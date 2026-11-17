# Starter Kit FPS

A multiplayer first person shooter.

Open `main.tscn` and press F6. For more than one player, enable multiple
instances in Debug > Customize Run Instances, press Host in one window and
Join in the others.

```text
WASD          move
Space         jump
left click    shoot
E             switch weapon
Escape        release the mouse
```

Each player moves its own body and every peer receives its `position` and
`rotation`. A shot that hits is sent to the server, which casts the ray again
and applies the damage on every peer. Enemies run on the server.

## Attribution

Ported from [Starter Kit FPS](https://github.com/KenneyNL/Starter-Kit-FPS) by
Kenney, MIT licensed. `LICENSE` in this directory is that project's licence,
and the scenes, scripts, sprites, 3D models and sound effects are its work. The
assets are [CC0 licensed](https://creativecommons.org/publicdomain/zero/1.0/).
