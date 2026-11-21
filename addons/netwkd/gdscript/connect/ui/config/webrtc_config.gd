## WebRTC defaults for a [ConnectBrowser].
class_name ConnectWebRTCConfig
extends ConnectTransportConfig

## The tracker namespace rooms are found in. Empty uses
## [member NetwSessionConfig.app_id].
@export var signaling_namespace: String = ""

## The WebTorrent trackers peers use to find each other.
@export var trackers: PackedStringArray = [
	"wss://tracker.openwebtorrent.com",
	"wss://tracker.webtorrent.dev",
]

## The STUN and TURN servers each peer connection uses.
@export var ice_servers: Array[ConnectIceServerConfig] = [
	ConnectIceServerConfig.new(),
]

## Seconds between attempts to connect to the host.
@export_range(0.01, 60.0, 0.01) var connect_retry: float = 8.0

## Attempts to connect to the host before the join fails.
@export_range(1, 100) var max_connect_attempts: int = 3

## Seconds to gather ICE candidates before sending the last of them.
@export_range(0.01, 60.0, 0.01) var gather_timeout: float = 6.0


## Returns [code]&"WebRTCMultiplayerPeer"[/code].
func peer_class() -> StringName:
	return &"WebRTCMultiplayerPeer"


## Returns the exported properties as WebRTC settings.
func settings() -> Dictionary:
	var servers: Array[Dictionary] = []
	for server in ice_servers:
		if server != null:
			servers.append(server.settings())
	return {
		signaling_namespace = signaling_namespace,
		trackers = trackers.duplicate(),
		ice_servers = servers,
		connect_retry = connect_retry,
		max_connect_attempts = max_connect_attempts,
		gather_timeout = gather_timeout,
	}
