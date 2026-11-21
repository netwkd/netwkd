## ICE server addresses and credentials for [ConnectWebRTCConfig].
class_name ConnectIceServerConfig
extends Resource

## The STUN or TURN URLs of this server.
@export var urls: PackedStringArray = ["stun:stun.l.google.com:19302"]

## The TURN user name. Left out when empty.
@export var username: String = ""

## The TURN password. Left out when empty.
@export var credential: String = ""


## Returns this server as one entry of the [code]ice_servers[/code] setting.
func settings() -> Dictionary:
	var result := { urls = Array(urls) }
	if not username.is_empty():
		result.username = username
	if not credential.is_empty():
		result.credential = credential
	return result
