# Rocket League

A multiplayer car-and-ball example.

Open `main.tscn` and press F6. For more than one player, enable multiple
instances in Debug > Customize Run Instances, press Host in one window and
Join in the others. The host starts the match from the lobby.

```text
arrows / WASD   drive
Space           jump
```

## Attribution

Ported from [godot-rocket-league](https://github.com/albertok/godot-rocket-league)
by Alberto Klocker, MIT licensed. `LICENSE` in this directory is that project's
licence, and the models, materials and vehicle arithmetic are its work.

Replication and rollback use `Netw.configure_property` declarations.
The session runs at 60 ticks per second.
