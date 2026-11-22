extends GdUnitTestSuite

const GAME := "res://addons/netwkd/examples/multiple_levels/"
const MAIN := preload(GAME + "main.tscn")
const SAVES := preload(GAME + "game/save_database.tres")
const TPLayer := preload(GAME + "game/tp_layer.gd")
const TPComponent := preload(GAME + "game/tp_component.gd")
const LEVEL_1_PATH := GAME + "game/level1.tscn"
const LEVEL_2_PATH := GAME + "game/level2.tscn"
const TP_MARKER := ^"%Teleporter/Marker2D"

var game: NetwTestGame


func before_test() -> void:
	var saves: FileSystemDatabase = SAVES
	saves.root = create_temp_dir("multiple_levels_saves")
	saves.delete_slot(null, &"campaign")
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	game.free()


func test_a_host_named_player_keeps_a_readable_name() -> void:
	var host := await game.host(&"Player")
	await game.until(settled.bind(host))

	var body: Node2D = await game.until(host.player)
	var seated := String(body.name)
	assert_str(seated) \
			.override_failure_message(
				"the host's body was seated as '%s', which no spawner accepts"
				% seated,
			) \
			.is_equal(seated.validate_node_name())
	assert_that(NetwEntity.of(body).name).is_equal(&"Player")


func test_host_input_replicates_to_client() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	await game.until(all_settled.bind(game.peers))

	var valeria_on_jose: Node2D = await game.until(
		jose.player.bind(&"valeria"),
	)
	var start := valeria_on_jose.position.x

	await game.run(16)
	valeria.press(&"move_right")
	await game.run(16)
	valeria.release(&"move_right")
	await game.run(16)

	assert_that(valeria_on_jose.position.x).is_greater(start)
	assert_that(Input.is_action_pressed(&"move_right")).is_false()


func test_host_scene_change_keeps_camera_current() -> void:
	var valeria := await game.host(&"valeria")
	await game.until(settled.bind(valeria))

	var player: Node2D = await game.until(valeria.player)
	var camera := player.get_node("Camera2D") as Camera2D
	assert_that(camera).is_not_null()
	assert_that(player.get_viewport().get_camera_2d()).is_equal(camera)

	await change_level_2([valeria])
	await game.until(valeria.session.find_scene.bind(&"Level2"))

	var arrived: Node2D = await game.until(
		body_in.bind(valeria, &"valeria", &"Level2"),
		180,
	)
	var arrived_camera := arrived.get_node("Camera2D") as Camera2D
	assert_that(arrived_camera).is_not_null()
	assert_that(arrived.get_viewport().get_camera_2d()).is_equal(arrived_camera)


func test_clients_still_see_each_other_after_scene_change() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	var maria := await game.join(&"maria")
	await game.until(all_settled.bind(game.peers))
	var maria_on_jose_start: Node2D = await game.until(
		jose.player.bind(&"maria"),
	)
	var base_x := maria_on_jose_start.position.x
	maria.press(&"move_right")
	await game.run(16)
	maria.release(&"move_right")
	await game.run(8)
	assert_that(maria_on_jose_start.position.x).is_greater(base_x)

	await change_level_2([maria, jose, valeria])
	for peer: NetwTestPeer in [valeria, jose, maria]:
		await game.until(peer.session.find_scene.bind(&"Level2"))

	var maria_local: Node2D = await game.until(
		body_in.bind(maria, &"maria", &"Level2"),
		180,
	)
	var maria_on_valeria: Node2D = await game.until(
		body_in.bind(valeria, &"maria", &"Level2"),
		180,
	)
	var maria_on_jose: Node2D = await game.until(
		body_in.bind(jose, &"maria", &"Level2"),
		180,
	)
	await game.run(8)

	var local_start := maria_local.position.x
	var host_start := maria_on_valeria.position.x
	var jose_start := maria_on_jose.position.x

	maria.press(&"move_right")
	await game.run(16)
	maria.release(&"move_right")
	await game.run(8)

	assert_that(maria_local.position.x) \
			.override_failure_message("maria's own local player did not move") \
			.is_greater(local_start)
	assert_that(maria_on_valeria.position.x) \
			.override_failure_message("host did not see maria move") \
			.is_greater(host_start)
	assert_that(maria_on_jose.position.x) \
			.override_failure_message("jose (client) did not see maria move") \
			.is_greater(jose_start)


func test_client_round_trip_teleport_stays_functional() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	var maria := await game.join(&"maria")
	await game.until(all_settled.bind(game.peers))
	var rough_link := OS.get_environment("NETW_RELOCATION_LINK") == "rough"
	if rough_link:
		var rough := NetwLinkConditions.new()
		rough.latency_ms = 125.0
		rough.loss_chance = 0.5
		jose.impair(rough)
		maria.impair(rough)

	await teleport_without_bridge(jose, valeria, LEVEL_1_PATH, &"Level1")
	assert_arrived_at_marker(jose, "Level1")

	await teleport_without_bridge(jose, valeria, LEVEL_2_PATH, &"Level2")
	await game.until(jose.session.find_scene.bind(&"Level2"))
	assert_arrived_at_marker(jose, "Level2")
	assert_that(jose.session.local_player.scene.label) \
			.override_failure_message(
				"jose is not in Level2 after moving into it",
			) \
			.is_equal(&"Level2")

	await teleport_without_bridge(jose, maria, LEVEL_1_PATH, &"Level1")
	await game.run(20)
	assert_arrived_at_marker(jose, "Level1")

	var jose_local := jose.player() as Node2D
	assert_that(jose_local) \
			.override_failure_message("jose has no local player after return") \
			.is_not_null()
	var jose_scene: NetwScene = NetwEntity.of(jose_local).scene
	assert_that(jose_scene) \
			.override_failure_message("jose's player belongs to no scene") \
			.is_not_null()
	assert_int(jose.session.scenes.size()) \
			.override_failure_message("jose still holds the level he left") \
			.is_equal(1)
	assert_bool(jose_local.is_visible_in_tree()) \
			.override_failure_message("jose's own body came back invisible") \
			.is_true()
	var jose_level: Node = jose_scene.root
	assert_that(StringName(jose_level.name)) \
			.override_failure_message(
				"jose's player did not return to Level1",
			) \
			.is_equal(&"Level1")
	assert_that(jose.session.local_player.scene.label) \
			.override_failure_message("jose is not in Level1 after returning") \
			.is_equal(&"Level1")

	var maria_on_jose: Node2D = await game.until(jose.player.bind(&"maria"))
	var jose_on_maria: Node2D = await game.until(maria.player.bind(&"jose"))

	var maria_x := maria_on_jose.position.x
	maria.press(&"move_right")
	await game.run(16)
	maria.release(&"move_right")
	await game.run(8)
	assert_that(maria_on_jose.position.x) \
			.override_failure_message(
				"jose (returned) stopped seeing maria move",
			) \
			.is_greater(maria_x)

	var jose_local_x := jose_local.position.x
	var jose_remote_x := jose_on_maria.position.x
	jose.press(&"move_right")
	await game.run(16)
	jose.release(&"move_right")
	await game.run(8)
	if rough_link:
		await game.run(64)
	assert_that(jose_local.position.x) \
			.override_failure_message("jose lost control of his own player") \
			.is_greater(jose_local_x)
	assert_that(jose_on_maria.position.x) \
			.override_failure_message("maria stopped seeing jose move") \
			.is_greater(jose_remote_x)


func test_a_reconnecting_player_is_restored_where_it_saved() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	await game.until(all_settled.bind(game.peers))

	await teleport_without_bridge(jose, valeria, LEVEL_2_PATH, &"Level2")
	assert_arrived_at_marker(jose, "Level2")

	await jose.drop()
	await game.run(16)

	var again := await game.join(&"jose")
	await game.until(settled.bind(again))
	assert_arrived_at_marker(again, "Level2")


func test_a_player_saved_in_level_2_never_appears_in_level_1() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	var maria := await game.join(&"maria")
	await game.until(all_settled.bind(game.peers))

	await teleport_without_bridge(jose, valeria, LEVEL_2_PATH, &"Level2")
	await jose.drop()
	await game.run(16)

	var parents: Array[StringName] = []
	var watch := func(node: Node) -> void:
		if node.name == &"jose" and maria.viewport.is_ancestor_of(node):
			parents.append(StringName(node.get_parent().name))
	get_tree().node_added.connect(watch)
	var again := await game.join(&"jose")
	await game.until(settled.bind(again))
	await game.run(16)
	get_tree().node_added.disconnect(watch)

	assert_arrived_at_marker(again, "Level2")
	assert_array(parents) \
			.override_failure_message("maria saw jose under %s" % [parents]) \
			.not_contains([&"Level1"])


func test_teleport_to_a_missing_marker_rejects_and_leaves_the_player_home() -> void:
	var valeria := await game.host(&"valeria")
	await game.until(settled.bind(valeria))

	var tp: TPComponent = await game.until(ready_tp.bind(valeria), 240)
	var before: Vector2 = (valeria.player() as Node2D).global_position

	var promise := tp.teleport(LEVEL_2_PATH, ^"%Teleporter/NoSuchMarker")
	await game.until(func() -> bool: return promise.is_settled, 300)
	assert_int(promise.code) \
			.override_failure_message(
				"a missing marker did not reject the teleport",
			) \
			.is_equal(ERR_DOES_NOT_EXIST)

	var player := valeria.player() as Node2D
	assert_that(player).is_not_null()
	var level: Node = NetwEntity.of(player).scene.root
	assert_that(String(level.name)) \
			.override_failure_message(
				"a rejected teleport moved valeria anyway",
			) \
			.is_equal("Level1")
	assert_float(player.global_position.distance_to(before)).is_less(1.0)


func test_completion_before_local_placement_waits_for_the_marker() -> void:
	await game.host(&"valeria")
	var jose := await game.join(&"jose")
	await game.until(all_settled.bind(game.peers))

	var layer := layer_of(jose)
	var tp: TPComponent = await game.until(ready_tp.bind(jose), 240)

	var promise := tp.teleport(LEVEL_2_PATH, TP_MARKER)
	layer.answer(OK, "")

	var level: Node = NetwEntity.of(jose.player()).scene.root
	assert_that(String(level.name)) \
			.override_failure_message(
				"the mover left Level1 before the server moved it",
			) \
			.is_equal("Level1")
	assert_bool(layer.revealing) \
			.override_failure_message(
				"the mover revealed the destination before it arrived",
			) \
			.is_false()
	assert_bool(promise.is_settled) \
			.override_failure_message(
				"a completion settled the teleport before it applied",
			) \
			.is_false()

	await game.until(func() -> bool: return promise.is_settled, 300)
	assert_int(promise.code).is_equal(OK)
	assert_arrived_at_marker(jose, "Level2")


func test_a_second_teleport_while_one_is_in_flight_is_one_operation() -> void:
	var valeria := await game.host(&"valeria")
	await game.until(settled.bind(valeria))

	var layer := layer_of(valeria)
	var tp: TPComponent = await game.until(ready_tp.bind(valeria), 240)

	var first := tp.teleport(LEVEL_2_PATH, TP_MARKER)
	var second := tp.teleport(LEVEL_1_PATH, TP_MARKER)

	assert_that(second) \
			.override_failure_message(
				"a second teleport opened a second operation",
			) \
			.is_same(first)
	assert_that(layer.pending) \
			.override_failure_message(
				"a second teleport replaced the pending request",
			) \
			.is_same(first)

	await game.until(func() -> bool: return first.is_settled, 300)
	assert_int(first.code).is_equal(OK)

	var level: Node = NetwEntity.of(valeria.player()).scene.root
	assert_that(String(level.name)) \
			.override_failure_message(
				"the overlapping request chose the destination",
			) \
			.is_equal("Level2")


func test_a_mover_leaving_mid_teleport_leaves_the_others_playable() -> void:
	var valeria := await game.host(&"valeria")
	var jose := await game.join(&"jose")
	await game.until(all_settled.bind(game.peers))

	var tp: TPComponent = await game.until(ready_tp.bind(jose), 240)
	tp.teleport(LEVEL_2_PATH, TP_MARKER)
	await game.run(2)
	await jose.drop()
	await game.run(30)

	var player := valeria.player() as Node2D
	assert_that(player) \
			.override_failure_message(
				"valeria lost her player when jose left",
			) \
			.is_not_null()
	var level: Node = NetwEntity.of(player).scene.root
	assert_that(String(level.name)).is_equal("Level1")

	var start := player.position.x
	valeria.press(&"move_right")
	await game.run(16)
	valeria.release(&"move_right")
	await game.run(8)
	assert_that(player.position.x) \
			.override_failure_message(
				"valeria could not move after jose left mid teleport",
			) \
			.is_greater(start)


func teleport_without_bridge(
		participant: NetwTestPeer,
		observer: NetwTestPeer,
		scene_path: String,
		scene_name: StringName,
) -> void:
	var layer := layer_of(participant)
	var tp: TPComponent = await game.until(ready_tp.bind(participant), 240)
	participant.press(&"move_right")
	var promise := tp.teleport(scene_path, TP_MARKER)
	var moving_frames := 0
	var destination_frames := 0
	var destination_arrived := false
	var input_held := true
	for tick in 300:
		await game.run(1)
		if layer.is_moving():
			moving_frames += 1
			var body := participant.player() as CharacterBody2D
			if body:
				assert_vector(body.velocity).is_equal(Vector2.ZERO)
			if moving_frames == 4:
				participant.release(&"move_right")
				input_held = false
		var remote := observer.player(participant.username) as Node2D
		if remote:
			var scene: NetwScene = NetwEntity.of(remote).scene
			if scene and StringName(scene.root.name) == scene_name:
				var marker := scene.root.get_node(TP_MARKER) as Node2D
				var visual := remote.get_node("Icon") as Node2D
				destination_arrived = destination_arrived or (
						remote.global_position.distance_to(
							marker.global_position,
						) < 1.0
				)
				if destination_arrived:
					destination_frames += 1
					assert_float(
						visual.global_position.distance_to(
							marker.global_position,
						),
					) \
							.override_failure_message(
								"%s rendered %s between teleport endpoints"
								% [observer.username, visual.global_position],
							) \
							.is_less(1.0)
		if promise.is_settled:
			break
	if input_held:
		participant.release(&"move_right")
	assert_int(moving_frames).is_greater(0)
	assert_int(destination_frames).is_greater(0)
	assert_bool(promise.is_completed) \
			.override_failure_message(
				"teleport to %s never completed" % scene_path,
			) \
			.is_true()


func body_in(
		peer: NetwTestPeer,
		player_username: StringName,
		label: StringName,
) -> Node2D:
	var body := peer.player(player_username) as Node2D
	var scene: NetwScene = Netw.scene(body) if body != null else null
	return body if scene != null and scene.label == label else null


func settled(peer: NetwTestPeer) -> bool:
	var layer := layer_of(peer)
	return peer.player() != null \
			and (layer == null or not layer.transition_anim.is_playing())


func all_settled(peers: Array[NetwTestPeer]) -> bool:
	return peers.all(settled)


func assert_arrived_at_marker(peer: NetwTestPeer, level_name: String) -> void:
	var player := peer.player() as Node2D
	assert_that(player) \
			.override_failure_message(
				"%s has no local player to place" % peer.username,
			) \
			.is_not_null()
	var level: Node = NetwEntity.of(player).scene.root
	assert_that(String(level.name)) \
			.override_failure_message(
				"%s is not standing in %s" % [peer.username, level_name],
			) \
			.is_equal(level_name)
	var marker := level.get_node_or_null(TP_MARKER) as Node2D
	assert_that(marker) \
			.override_failure_message(
				"%s has no %s to arrive on" % [level_name, TP_MARKER],
			) \
			.is_not_null()
	assert_float(player.global_position.distance_to(marker.global_position)) \
			.override_failure_message(
				"%s arrived at %s instead of %s's marker at %s"
				% [
					peer.username,
					player.global_position,
					level_name,
					marker.global_position,
				],
			) \
			.is_less(1.0)


func layer_of(peer: NetwTestPeer) -> TPLayer:
	return peer.session.service(TPLayer) as TPLayer


func ready_tp(peer: NetwTestPeer) -> TPComponent:
	var player := peer.player()
	if player == null or layer_of(peer).is_moving():
		return null
	return player.get_node_or_null("%TPComponent") as TPComponent


func change_level_2(peers: Array[NetwTestPeer]) -> void:
	for peer in peers:
		var tp: TPComponent = await game.until(ready_tp.bind(peer), 240)
		var promise := tp.teleport(LEVEL_2_PATH, TP_MARKER)
		await game.until(func() -> bool: return promise.is_settled, 300)
		assert_int(promise.code).is_equal(OK)
	await game.until(all_settled.bind(peers))
