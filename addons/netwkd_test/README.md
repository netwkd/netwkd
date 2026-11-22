# netwkd Test

Tests for a netwkd game, with a server and its clients running in one
process.

## Install

Unzip `netwkd_test` from the netwkd release into your project's root,
beside `netwkd`. It holds `addons/netwkd_test` and needs no test framework.

## Use

Make a `NetwTestGame` from the game's main scene, add it to the tree, then
host and join.

```gdscript
extends GdUnitTestSuite

const MAIN := preload("res://main.tscn")

var game: NetwTestGame


func before_test() -> void:
    game = NetwTestGame.new(MAIN)
    game.failed.connect(fail)
    add_child(game)


func after_test() -> void:
    game.free()


func test_a_client_moves_and_the_host_sees_it() -> void:
    var ada := await game.host(&"ada")
    var bob := await game.join(&"bob")
    var seen: Node2D = await game.until(ada.player.bind(&"bob"))
    var start := seen.position.x

    bob.impair(NetwLinkConditions.wifi())
    bob.press(&"move_right")
    await game.run(12)
    bob.release(&"move_right")
    await game.run(4)

    assert_float(seen.position.x).is_greater(start)
```

A failure emits `failed` with a message, and a suite connects it to its
test framework's fail call. That call is `fail` in gdUnit4 and `fail_test`
in GUT. With nothing connected, the message is a `push_error`.

`press`, `release` and `input` reach a peer's `_input` and
`_unhandled_input`, and never the `Input` singleton. Every peer in the
process shares `Input`, so a game that polls it reads none of these
presses.

Press F1 on `NetwTestGame` or `NetwTestPeer` in the editor for every call.
