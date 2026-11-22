## One peer of a [NetwTestGame], from [method NetwTestGame.serve],
## [method NetwTestGame.host] or [method NetwTestGame.join].
##
## A test reads what this peer sees through [member session], and its input
## and link changes reach this peer and no other.
## [codeblock]
## var bob := await game.join(&"bob")
## var car: Node3D = await game.until(bob.player)
## bob.impair(NetwLinkConditions.poor_3g())
## bob.press(&"accelerate")
## await game.run(30)
## bob.release(&"accelerate")
## bob.heal()
## [/codeblock]
class_name NetwTestPeer
extends RefCounted

## The name this peer served, hosted or joined with.
var username: StringName

## What [method Netw.session] answers for this peer.
var session: NetwSession

## The [MultiplayerViewport] this peer's session runs in.
var viewport: MultiplayerViewport

## This peer's instance of the game's [PackedScene].
var scene: Node

var _game: NetwTestGame


## Made by the [NetwTestGame] that holds this peer.
func _init(
		game: NetwTestGame,
		packed: PackedScene,
		grid: GridContainer,
		player_name: StringName,
) -> void:
	_game = game
	username = player_name
	scene = packed.instantiate()

	var found := scene.find_children("*", "MultiplayerViewport", true, false)
	if scene is MultiplayerViewport:
		found.append(scene)
	if found.size() > 1:
		game.failed.emit(
			"The scene has %d MultiplayerViewport nodes. Use a scene with one "
			% found.size()
			+ "or none.",
		)
		scene.free()
		return

	var top := scene
	viewport = found.pop_back() as MultiplayerViewport
	if viewport == null:
		viewport = MultiplayerViewport.new()
		viewport.name = username
		viewport.add_child(scene)
		top = viewport

	var view := StretchViewportContainer.new()
	view.name = "%sView" % username
	view.custom_minimum_size = NetwTestGame.VIEW_SIZE
	view.add_child(top)
	grid.add_child(view)
	grid.columns = ceili(sqrt(grid.get_child_count()))
	session = Netw.session(viewport)


## Answers the root of [param player_name]'s player as this peer sees it, or
## this peer's own player when [param player_name] is empty, and
## [code]null[/code] while there is none.
func player(player_name := &"") -> Node:
	if player_name.is_empty():
		var local := session.local_player
		var own: Array = local.entities if local != null else []
		return own[0].root if own.size() == 1 else null

	var named := func(entity: NetwEntity) -> bool:
		return entity.name == player_name
	var found: Array[NetwEntity] = []
	for netw_scene: NetwScene in session.scenes:
		found.append_array(netw_scene.player_entities.filter(named))
	return found[0].root if found.size() == 1 else null


## Sends [param event] to this peer at the end of the frame.
func input(event: InputEvent) -> void:
	viewport.push_input.call_deferred(event, true)


## Presses the input action [param action] on this peer.
func press(action: StringName) -> void:
	var event := InputEventAction.new()
	event.action = action
	event.pressed = true
	input(event)


## Releases the input action [param action] on this peer.
func release(action: StringName) -> void:
	var event := InputEventAction.new()
	event.action = action
	input(event)


## Applies [param conditions] to both directions of this client's link to the
## server.
func impair(conditions: NetwLinkConditions) -> void:
	if session.role != NetwSession.ROLE_CLIENT:
		_game.failed.emit(
			"'%s' is the server. Call impair on a client." % username,
		)
		return

	var loopback := LocalLoopbackSession.get_shared_session()
	var id := viewport.multiplayer.get_unique_id()
	var peer := viewport.multiplayer.multiplayer_peer as LocalMultiplayerPeer
	loopback.set_link_conditions(peer, conditions, 1)
	loopback.set_link_conditions(loopback.get_server_peer(), conditions, id)


## Clears the conditions [method impair] applied.
func heal() -> void:
	var loopback := LocalLoopbackSession.get_shared_session()
	var id := viewport.multiplayer.get_unique_id()
	var peer := viewport.multiplayer.multiplayer_peer as LocalMultiplayerPeer
	loopback.clear_link_conditions(peer, 1)
	loopback.clear_link_conditions(loopback.get_server_peer(), id)


## Closes this client's connection as a lost link does, and returns once the
## server has let it go. [method NetwSession.leave] is a clean leave.
func drop() -> void:
	var server := _game.peers[0].session
	var id := viewport.multiplayer.get_unique_id()
	var peer := viewport.multiplayer.multiplayer_peer as LocalMultiplayerPeer
	LocalLoopbackSession.get_shared_session().release_inbound_packets(peer)
	peer.close()
	for tick in 120:
		if server.player_of(id) == null:
			return
		await _game.run(1)

	if server.player_of(id) != null:
		_game.failed.emit(
			"'%s' is still on the server's roster after 120 ticks." % username,
		)
