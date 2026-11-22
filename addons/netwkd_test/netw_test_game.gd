## A server and its clients for one game, running in one process for a test.
##
## Each peer runs its own instance of the game's [PackedScene]. Only one
## game runs at a time. [method run] and [method until] count ticks, and
## nothing in the game counts seconds. Freeing the game is the whole
## teardown.
## [codeblock]
## var game := NetwTestGame.new(MAIN)
## game.failed.connect(fail)
## add_child(game)
##
## var ada := await game.host(&"ada")
## var bob := await game.join(&"bob")
## var seen: Node2D = await game.until(ada.player.bind(&"bob"))
## bob.press(&"move_right")
## await game.run(12)
##
## game.free()
## [/codeblock]
## A test hosts or serves once, joins its clients, and reads what each
## [NetwTestPeer] sees through [member NetwTestPeer.session].
class_name NetwTestGame
extends Node

## Emitted with [param message] when a call fails. A failed [method serve],
## [method host], [method join] or [method until] answers [code]null[/code].
signal failed(message: String)

## The transport every peer connects through.
const TRANSPORT := &"LocalMultiplayerPeer"

## The size of each peer's view in the window [method show] opens.
const VIEW_SIZE := Vector2(576, 324)

## Every peer that served, hosted or joined, in that order, so the server is
## first.
var peers: Array[NetwTestPeer] = []

var _scene: PackedScene
var _window := Window.new()
var _grid := GridContainer.new()


## Makes a game whose peers each run their own instance of [param scene].
## A [param scene] holds at most one [MultiplayerViewport], and one with none
## is wrapped in one.
func _init(scene: PackedScene) -> void:
	_scene = scene
	failed.connect(_on_failed)
	_window.title = "NetwTestGame"
	_window.visible = false
	_window.wrap_controls = true
	_window.add_child(_grid)
	add_child(_window)


## Adds a dedicated server with no player.
func serve() -> NetwTestPeer:
	if not is_inside_tree():
		failed.emit(
			"NetwTestGame is not in the scene tree. Add it with add_child "
			+ "before calling serve.",
		)
		return null
	if not peers.is_empty():
		failed.emit(
			"This game already has a server. Use join to add a client.",
		)
		return null
	if LocalLoopbackSession.get_shared_session().has_live_server():
		failed.emit(
			"Another NetwTestGame is running. Free it before adding this one.",
		)
		return null

	var peer := NetwTestPeer.new(self, _scene, _grid, &"server")
	if peer.viewport == null:
		return null

	var hosted: NetwPromise = peer.session.connection.host(
		TRANSPORT,
		{ role = NetwSession.ROLE_DEDICATED_SERVER },
	)
	await hosted.wait()
	if hosted.is_failed:
		failed.emit("'server' could not host. %s" % hosted.detail)
		return null

	peers.append(peer)
	return peer


## Adds a listen server whose own player is [param username], and returns
## once the server has seated that player. The arguments after
## [param username] reach the game's [method Netw.configure_join] handler.
func host(username: StringName, ...args: Array) -> NetwTestPeer:
	if not is_inside_tree():
		failed.emit(
			"NetwTestGame is not in the scene tree. Add it with add_child "
			+ "before calling host.",
		)
		return null
	if not peers.is_empty():
		failed.emit(
			"This game already has a server. Use join to add a client.",
		)
		return null
	if LocalLoopbackSession.get_shared_session().has_live_server():
		failed.emit(
			"Another NetwTestGame is running. Free it before adding this one.",
		)
		return null

	var peer := NetwTestPeer.new(self, _scene, _grid, username)
	if peer.viewport == null:
		return null

	Netw.prepare_join.callv([peer.viewport, username] + args)
	var hosted: NetwPromise = peer.session.connection.host(TRANSPORT)
	await hosted.wait()
	if hosted.is_failed:
		failed.emit("'%s' could not host. %s" % [username, hosted.detail])
		return null

	peers.append(peer)
	var seated: Variant = await until(peer.session.player_of.bind(1))
	return peer if seated != null else null


## Adds a client whose player is [param username], and returns once the
## server has seated that player. The arguments after [param username] reach
## the game's [method Netw.configure_join] handler.
func join(username: StringName, ...args: Array) -> NetwTestPeer:
	if not is_inside_tree():
		failed.emit(
			"NetwTestGame is not in the scene tree. Add it with add_child "
			+ "before calling join.",
		)
		return null
	if peers.is_empty():
		failed.emit("This game has no server. Call host or serve before join.")
		return null

	var peer := NetwTestPeer.new(self, _scene, _grid, username)
	if peer.viewport == null:
		return null

	Netw.prepare_join.callv([peer.viewport, username] + args)
	var connection := peer.session.connection
	var joined: NetwPromise = connection.join(connection.add(TRANSPORT, ""))
	await joined.wait()
	if joined.is_failed:
		failed.emit("'%s' could not join. %s" % [username, joined.detail])
		return null

	peers.append(peer)
	var id := peer.viewport.multiplayer.get_unique_id()
	var seated: Variant = await until(peers[0].session.player_of.bind(id))
	return peer if seated != null else null


## Advances every peer by [param ticks] ticks of its [NetwClock], or by
## [param ticks] frames when no peer has a clock configured.
func run(ticks: int) -> void:
	var clocks: Array[NetwClock] = []
	for peer in peers:
		if peer.session.clock.is_configured:
			clocks.append(peer.session.clock)

	if clocks.is_empty():
		for tick in ticks:
			await get_tree().process_frame
		return

	var frames_per_tick: Array[int] = []
	var longest := 1
	for clock in clocks:
		frames_per_tick.append(maxi(1, roundi(clock.physics_factor)))
		longest = maxi(longest, frames_per_tick.back())
		clock.manual_tick = true

	var stepped: Array[int] = []
	stepped.resize(clocks.size())
	stepped.fill(0)
	for frame in range(1, ticks * longest + 1):
		await get_tree().physics_frame
		for i in clocks.size():
			var due := stepped[i] < ticks and frame % frames_per_tick[i] == 0
			clocks[i].step(int(due))
			stepped[i] += int(due)

	for clock in clocks:
		clock.manual_tick = false


## Runs one tick at a time until [param condition] answers a value that is
## not [code]null[/code] or [code]false[/code], and returns that value.
## After [param ticks] ticks it emits [signal failed].
func until(condition: Callable, ticks := 120) -> Variant:
	for tick in ticks + 1:
		var answer: Variant = condition.call()
		if answer != null and (answer is not bool or answer):
			return answer
		if tick < ticks:
			await run(1)

	var bound: Array = condition.get_bound_arguments().map(var_to_str)
	failed.emit(
		"Waited %d ticks for %s(%s). Pass a larger tick count or check the "
		% [ticks, condition.get_method(), ", ".join(bound)]
		+ "condition.",
	)
	return null


## Opens a window that draws every peer's view in a grid.
func show() -> Window:
	_window.show()
	_window.grab_focus()
	return _window


func _on_failed(message: String) -> void:
	if failed.get_connections().size() == 1:
		push_error(message)
