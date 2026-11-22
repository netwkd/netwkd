extends GdUnitTestSuite

const MAIN := preload("res://addons/netwkd/examples/fps/main.tscn")

var game: NetwTestGame


func before_test() -> void:
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	Input.action_release("move_forward")
	Input.action_release("shoot")
	game.free()


func test_the_host_sees_a_client_player_move() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await game.until(host.session.find_scene.bind(&"Main"))
	await game.until(client.session.find_scene.bind(&"Main"))

	var own: Node3D = await game.until(client.player.bind(&"luigi"))
	var host_view: Node3D = await game.until(host.player.bind(&"luigi"))
	assert_that(client.player()).is_equal(own)
	assert_bool(host_view.entity.is_controlled_locally).is_false()

	await game.run(30)
	var start: Vector3 = own.position
	var host_start: Vector3 = host_view.position
	Input.action_press("move_forward")
	await game.run(60)
	Input.action_release("move_forward")
	await game.run(10)

	var travelled: float = own.position.distance_to(start)
	assert_float(travelled).is_greater(1.0)
	assert_float(host_view.position.distance_to(host_start)) \
			.override_failure_message(
				"the host view must follow the move its controller relayed",
			).is_greater(travelled * 0.5)


func test_a_client_shot_lands_on_an_enemy_every_peer_sees() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	var host_scene: NetwScene = await game.until(
		host.session.find_scene.bind(&"Main"),
	)
	var client_scene: NetwScene = await game.until(
		client.session.find_scene.bind(&"Main"),
	)
	var own: Node3D = await game.until(client.player.bind(&"luigi"))
	var host_view: Node3D = await game.until(host.player.bind(&"luigi"))
	var mario: Node3D = await game.until(host.player.bind(&"mario"))
	var host_enemy: Node3D = host_scene.root.get_node(^"Enemies/enemy-flying")
	var client_enemy: Node3D = client_scene.root.get_node(
		^"Enemies/enemy-flying",
	)
	await game.run(30)

	mario.camera.rotation.x = deg_to_rad(90)
	var aim: Vector3 = client_enemy.global_position + Vector3(0, 0.25, 0) \
			- own.camera.global_position
	own.rotation.y = atan2(-aim.x, -aim.z)
	own.camera.rotation.x = atan2(aim.y, Vector2(aim.x, aim.z).length())
	Input.action_press("shoot")
	await game.run(2)
	Input.action_release("shoot")
	await game.run(30)

	assert_int(host_enemy.health).override_failure_message(
		"the server must take the hit the client reported",
	).is_less(100)
	assert_int(client_enemy.health).override_failure_message(
		"the client must see the hit the server took",
	).is_equal(host_enemy.health)
	assert_int(own.shots).is_greater(0)
	assert_int(host_view.shots).override_failure_message(
		"the host must see the shot counter the client raised",
	).is_equal(own.shots)
