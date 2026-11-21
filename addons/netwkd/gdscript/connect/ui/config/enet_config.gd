## ENet defaults for a [ConnectBrowser].
class_name ConnectENetConfig
extends ConnectTransportConfig

## The port the host listens on.
@export_range(1, 65535) var port: int = 21253

## The most clients the host accepts.
@export_range(1, 4095) var max_players: int = 32


## Returns [code]&"ENetMultiplayerPeer"[/code].
func peer_class() -> StringName:
	return &"ENetMultiplayerPeer"


## Returns [member port] and [member max_players].
func settings() -> Dictionary:
	return { port = port, max_players = max_players }
