#pragma once

namespace netw_test::gdsrc {

constexpr const char *TRANSPORT_THAT_HOLDS = R"(extends NetwTransport

func _peer_class() -> StringName:
	return &"HeldMultiplayerPeer"

func _display_name() -> String:
	return "Held"

func _timeout_hint() -> float:
	return -1.0

func _make_peer(
	_ticket: RID,
	_mode: NetwConnection.TransportMode,
	_address: String,
	_settings: Dictionary,
) -> void:
	pass
)";

constexpr const char *TRANSPORT_THAT_SETTLES = R"(extends NetwTransport

func _peer_class() -> StringName:
	return &"PromptMultiplayerPeer"

func _display_name() -> String:
	return "Prompt"

func _make_peer(
	ticket: RID,
	_mode: NetwConnection.TransportMode,
	_address: String,
	_settings: Dictionary,
) -> void:
	var peer := LocalMultiplayerPeer.new()
	peer.create_client(9)
	deliver(ticket, peer)
)";

constexpr const char *TRANSPORT_CLAIMING_THE_SAME_CLASS
    = R"(extends NetwTransport

func _peer_class() -> StringName:
	return &"HeldMultiplayerPeer"

func _display_name() -> String:
	return "Rival"

func _make_peer(
	_ticket: RID,
	_mode: NetwConnection.TransportMode,
	_address: String,
	_settings: Dictionary,
) -> void:
	pass
)";

constexpr const char *NATIVE_CLASS_HANDLE = R"(extends RefCounted

static func directory() -> Variant:
	return LobbyDirectory
)";

constexpr const char *JOIN_HANDLERS = R"(extends RefCounted

signal released(value: Variant)

var calls := 0

func immediate(_player: NetwPlayer, scene: Node) -> Node:
	calls += 1
	return scene

func suspended(_player: NetwPlayer, _scene: Node) -> Node:
	calls += 1
	return await released
)";

} // namespace netw_test::gdsrc
