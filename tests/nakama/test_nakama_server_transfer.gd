class_name TestNakamaServerTransfer
extends GdUnitTestSuite

const TIMEOUT := 15.0
const DEADLINE_TICKS := 30
const DEADLINE_WATCH_SECONDS := 3.0
const CRATE_STRIDE := 20.0
const MOVED_BEFORE := Vector2(200, 100)
const MOVED_AFTER := Vector2(400, 300)
const DEADLINE_WARNING := "did not report its world to the new server"

var trees: Array[MultiplayerViewport] = []
var warnings: WarningLog


class WarningLog:
	extends Logger

	var lines: Array[String] = []
	var mutex := Mutex.new()


	func _log_error(
			_function: String,
			_file: String,
			_line: int,
			code: String,
			rationale: String,
			_editor_notify: bool,
			_error_type: int,
			_script_backtraces: Array[ScriptBacktrace],
	) -> void:
		mutex.lock()
		lines.append(code if rationale.is_empty() else rationale)
		mutex.unlock()


	func count(fragment: String) -> int:
		mutex.lock()
		var found := lines.filter(
			func(line: String) -> bool: return line.contains(fragment)
		).size()
		mutex.unlock()
		return found


class TransferWorld:
	extends Node

	func _init() -> void:
		name = "World"
		Netw.configure_session(self).transfer_deadline(DEADLINE_TICKS)
		Netw.configure_clock(self)
		Netw.configure_spawn(spawn_arena)
		Netw.configure_join(enter)


	func spawn_arena() -> Node:
		return Arena.new()


	func enter(player: NetwPlayer) -> void:
		var arena := get_node_or_null(^"Arena") as Arena
		if arena == null:
			arena = Netw.spawn(spawn_arena)
			add_child(arena)
			arena.add_child(Netw.spawn(arena.spawn_crate))
		arena.add_child(Netw.spawn_player(player, arena.spawn_avatar))


class Arena:
	extends Node2D

	func _init() -> void:
		name = "Arena"
		Netw.configure_scene(self).labeled(&"Arena")
		Netw.configure_spawn(spawn_avatar)
		Netw.configure_spawn(spawn_crate)


	func spawn_avatar() -> Node:
		return Avatar.new()


	func spawn_crate() -> Node:
		return Crate.new()


class Avatar:
	extends Node2D

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").broadcast()


class Crate:
	extends Node2D

	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


	func _physics_process(_delta: float) -> void:
		if is_multiplayer_authority():
			position.x += 1.0


class Watch:
	extends RefCounted

	var transfers: Array = []
	var server_lost := false


	func _init(tree: MultiplayerViewport) -> void:
		Netw.session(tree).server_transferred.connect(on_server_transferred)
		tree.multiplayer.server_disconnected.connect(on_server_disconnected)


	func on_server_transferred(successor_was: int, former_now: int) -> void:
		transfers.append([successor_was, former_now])


	func on_server_disconnected() -> void:
		server_lost = true


@warning_ignore("unused_parameter")
func before(
		do_skip = NakamaTestServer.unavailable(),
		skip_reason = NakamaTestServer.SKIP_REASON,
) -> void:
	pass


func before_test() -> void:
	warnings = WarningLog.new()
	OS.add_logger(warnings)


func after_test() -> void:
	OS.remove_logger(warnings)
	for tree in trees:
		if is_instance_valid(tree):
			await NakamaTestSupport.stop_tree(tree)
	trees.clear()
	await super.after_test()


func test_a_lost_host_continues_on_a_survivor() -> void:
	var host := await open_world("host")
	var room := Netw.session(host).connection.join_address
	var ana := await join_world(room, "ana")
	var ben := await join_world(room, "ben")
	var survivors: Array[MultiplayerViewport] = [ana, ben]
	if not await wait_until(world_whole.bind([host, ana, ben]), "the world"):
		return
	await judge_state_flows(host, ana, MOVED_BEFORE)
	var ana_id := ana.multiplayer.get_unique_id()
	var ben_id := ben.multiplayer.get_unique_id()
	var successor_was := mini(ana_id, ben_id)
	var watches: Array[Watch] = [Watch.new(ana), Watch.new(ben)]

	trees.erase(host)
	await NakamaTestSupport.stop_tree(host)

	if not await wait_until(world_whole.bind(survivors), "the survivors"):
		return
	for watch in watches:
		assert_array(watch.transfers).is_equal([[successor_was, 0]])
		assert_bool(watch.server_lost).is_false()
	var successor := ana if ana_id == successor_was else ben
	var other := ben if successor == ana else ana
	assert_int(successor.multiplayer.get_unique_id()).is_equal(1)
	assert_bool(successor.multiplayer.is_server()).is_true()
	assert_int(other.multiplayer.get_unique_id()).is_equal(maxi(ana_id, ben_id))
	for tree in survivors:
		assert_object(body_of(tree, "host")).is_null()
	await judge_state_flows(successor, other, MOVED_AFTER)
	await judge_deadline_holds(survivors)

	var dana := await join_world(room, "dana")
	await wait_until(
		world_whole.bind([successor, other, dana]),
		"a late joiner",
	)


func test_a_handed_off_server_continues_on_its_successor() -> void:
	var host := await open_world("host")
	var room := Netw.session(host).connection.join_address
	var ana := await join_world(room, "ana")
	var ben := await join_world(room, "ben")
	var everyone: Array[MultiplayerViewport] = [host, ana, ben]
	if not await wait_until(world_whole.bind(everyone), "the world"):
		return
	await judge_state_flows(host, ana, MOVED_BEFORE)
	var ana_id := ana.multiplayer.get_unique_id()
	var watches: Array[Watch] = [
		Watch.new(host),
		Watch.new(ana),
		Watch.new(ben),
	]

	var session: NetwSession = Netw.session(host)
	assert_int(session.transfer_server(session.player_of(ana_id))).is_equal(OK)

	var handed := func() -> bool:
		return watches.all(
			func(watch: Watch) -> bool: return not watch.transfers.is_empty()
		)
	if not await wait_until(handed, "every peer to install the hand off"):
		return
	if not await wait_until(world_whole.bind(everyone), "the handed off world"):
		return
	var former_now: int = watches[0].transfers[0][1]
	assert_int(former_now).is_not_equal(0)
	for watch in watches:
		assert_array(watch.transfers).is_equal([[ana_id, former_now]])
		assert_bool(watch.server_lost).is_false()
	assert_int(ana.multiplayer.get_unique_id()).is_equal(1)
	assert_int(host.multiplayer.get_unique_id()).is_equal(former_now)
	assert_bool(host.multiplayer.is_server()).is_false()
	for player: NetwPlayer in Netw.session(ana).players:
		assert_str(player.account) \
				.override_failure_message(
					"The successor holds no Nakama account for peer %d."
					% player.peer_id,
				).starts_with("nakama:")
	for player: NetwPlayer in Netw.session(host).players:
		if player.peer_id != former_now:
			assert_str(player.account) \
					.override_failure_message(
						"The demoted server kept the account of peer %d."
						% player.peer_id,
					).is_empty()
	await judge_state_flows(ana, host, MOVED_AFTER)
	await judge_deadline_holds(everyone)


func judge_state_flows(
		server: MultiplayerViewport,
		client: MultiplayerViewport,
		to: Vector2,
) -> void:
	var start := crate_of(client).position.x
	await wait_until(
		func() -> bool:
			return crate_of(client).position.x > start + CRATE_STRIDE,
		"the crate to move under the server",
	)
	var client_name := String(Netw.session(client).local_player.username)
	(body_of(client, client_name) as Node2D).position = to
	await wait_until(
		func() -> bool: return body_of(server, client_name).position == to,
		"the client's avatar to reach the server",
	)
	var server_name := String(Netw.session(server).local_player.username)
	(body_of(server, server_name) as Node2D).position = to
	await wait_until(
		func() -> bool: return body_of(client, server_name).position == to,
		"the server's avatar to reach the client",
	)


func judge_deadline_holds(survivors: Array[MultiplayerViewport]) -> void:
	await get_tree().create_timer(DEADLINE_WATCH_SECONDS).timeout
	assert_int(warnings.count(DEADLINE_WARNING)).is_equal(0)
	for tree in survivors:
		assert_bool(Netw.session(tree).is_online).is_true()
		assert_int(tree.multiplayer.get_peers().size()) \
				.is_equal(survivors.size() - 1)


func open_world(username: String) -> MultiplayerViewport:
	var tree := NakamaTestSupport.make_client_tree(self, username)
	tree.add_child(TransferWorld.new())
	trees.append(tree)
	await get_tree().process_frame
	assert_int(await NakamaTestSupport.sign_in(tree, username)).is_equal(OK)
	var settled := await NetwOrdinarySetup.connect_through(
		tree,
		NakamaTestSupport.PEER_CLASS,
		NetwConnection.TRANSPORT_MODE_HOST,
		"",
		{ },
		StringName(username),
	)
	assert_int(settled.error).is_equal(OK)
	NakamaTestSupport.directory(tree).migrate_on_host_loss = true
	return tree


func join_world(room: String, username: String) -> MultiplayerViewport:
	var tree := NakamaTestSupport.make_client_tree(self, username)
	tree.add_child(TransferWorld.new())
	trees.append(tree)
	await get_tree().process_frame
	var error := await NakamaTestSupport.join_client(tree, room, username)
	assert_int(error).is_equal(OK)
	NakamaTestSupport.directory(tree).migrate_on_host_loss = true
	return tree


func world_whole(group: Array) -> bool:
	for tree: MultiplayerViewport in group:
		if not Netw.session(tree).is_online or crate_of(tree) == null:
			return false
		if tree.multiplayer.get_peers().size() != group.size() - 1:
			return false
		for other: MultiplayerViewport in group:
			var player := Netw.session(other).local_player
			if player == null or body_of(tree, player.username) == null:
				return false
		if player_entities_of(tree).size() != group.size():
			return false
	return true


func player_entities_of(tree: MultiplayerViewport) -> Array[NetwEntity]:
	var bodies: Array[NetwEntity] = []
	for scene: NetwScene in Netw.session(tree).scenes:
		bodies.append_array(scene.player_entities)
	return bodies


func body_of(tree: MultiplayerViewport, username: String) -> Node2D:
	var wanted := StringName(username)
	for body: NetwEntity in player_entities_of(tree):
		if body.player != null and body.player.username == wanted:
			return body.root as Node2D
	return null


func crate_of(tree: MultiplayerViewport) -> Crate:
	var arena := tree.get_node_or_null(^"World/Arena")
	if arena == null:
		return null
	for child in arena.get_children():
		if child is Crate:
			return child
	return null


func wait_until(
		condition: Callable,
		label: String,
		timeout: float = TIMEOUT,
) -> bool:
	var deadline := Time.get_ticks_msec() + int(timeout * 1000.0)
	while not condition.call():
		if Time.get_ticks_msec() > deadline:
			assert_bool(false) \
					.override_failure_message(
						"Timed out waiting for %s." % label,
					).is_true()
			return false
		await get_tree().process_frame
	return true
