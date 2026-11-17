# netwkd Nakama

Nakama relay matches, accounts and saves for a netwkd session. The host
needs no open port, so a web export can host.

## Install

Unzip `netwkd_nakama` from the netwkd release into your project's root,
beside `netwkd`. It holds `addons/netwkd_nakama` and
`addons/com.heroiclabs.nakama`. The second is the
[Nakama Godot SDK](https://github.com/heroiclabs/nakama-godot). Run a Nakama
server, such as `tests/nakama/docker-compose.yml` in the netwkd repository.

## Use

Add a `NakamaAccount` node with the server's address, and a `NakamaLobbyDirectory`.

```gdscript
@onready var session: NetwSession = Netw.session(self)

func _on_host_pressed() -> void:
    await session.connection.host(&"NakamaRelayPeer").settled
```

A server hosting over ENet or WebRTC proves Nakama accounts with
`NakamaIdToken`, and `NakamaParty` invites friends, both through
`server/netwkd.js` on the Nakama server. Its header says how to install it.
