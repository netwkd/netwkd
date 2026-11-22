extends GdUnitTestSuite

const MAIN := preload("res://addons/netwkd/examples/racing/main.tscn")

var game: NetwTestGame


func before_test() -> void:
	ProjectSettings.set_setting(&"netwkd/display/smooth_when_headless", true)
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	ProjectSettings.set_setting(&"netwkd/display/smooth_when_headless", false)
	game.free()


func test_a_controller_drives_its_own_car_forward() -> void:
	var host := await game.host(&"mario")
	await game.until(host.session.find_scene.bind(&"Track"))
	var car: Node = await game.until(host.player.bind(&"mario"))
	assert_that(host.player()).is_equal(car)

	var start: Vector3 = car.sphere_position
	await game.run(4)
	host.press(&"forward")
	await game.run(90)
	host.release(&"forward")

	assert_float(car.sphere_position.distance_to(start)).is_greater(0.5)
	assert_float(absf(car.linear_speed)).is_greater(0.1)
	assert_int(car.entity.simulation.mode).override_failure_message(
		"the controller's car runs its step as the author",
	).is_equal(NetwSimulation.MODE_AUTHORITY)


func test_every_view_of_a_car_follows_its_controllers_relayed_pose() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	var watcher := await game.join(&"peach")
	await game.until(host.session.find_scene.bind(&"Track"))
	await game.until(client.session.find_scene.bind(&"Track"))
	await game.until(watcher.session.find_scene.bind(&"Track"))

	var own: Node = await game.until(client.player.bind(&"luigi"))
	var host_view: Node = await game.until(host.player.bind(&"luigi"))
	var watcher_view: Node = await game.until(watcher.player.bind(&"luigi"))
	assert_that(client.player()).is_equal(own)

	assert_bool(host_view.sphere.freeze).override_failure_message(
		"a car nobody here controls is a kinematic proxy on the relayed pose",
	).is_true()
	assert_bool(watcher_view.sphere.freeze).is_true()

	var start: Vector3 = own.sphere_position
	var host_start: Vector3 = host_view.display_position
	var watcher_start: Vector3 = watcher_view.display_position
	await game.run(4)
	client.press(&"forward")
	await game.run(90)
	client.release(&"forward")

	var travelled: float = own.sphere_position.distance_to(start)
	assert_float(travelled).is_greater(2.0)
	assert_float(
		host_view.display_position.distance_to(host_start),
	).override_failure_message(
		"the host view must follow the pose its controller relayed",
	).is_greater(travelled * 0.5)
	assert_float(
		watcher_view.display_position.distance_to(watcher_start),
	).override_failure_message(
		"a watcher view must follow the pose its controller relayed",
	).is_greater(travelled * 0.5)


func test_a_proxy_uses_relayed_turn_rate_for_its_tire_trails() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	var watcher := await game.join(&"peach")
	await game.until(host.session.find_scene.bind(&"Track"))
	await game.until(client.session.find_scene.bind(&"Track"))
	await game.until(watcher.session.find_scene.bind(&"Track"))

	var own: Node = await game.until(client.player.bind(&"luigi"))
	var watcher_view: Node = await game.until(watcher.player.bind(&"luigi"))
	own.entity.simulation.network_process = hold
	own.linear_speed = 1.0
	own.acceleration = 1.0
	own.angular_speed = 4.0
	await game.run(4)
	watcher_view.effect_body(0.2)
	watcher_view.effect_trails()

	assert_bool(watcher_view.sphere.freeze).is_true()
	assert_float(watcher_view.angular_speed).is_equal_approx(4.0, 0.01)
	assert_bool(watcher_view.trail_left.emitting).is_true()
	assert_bool(watcher_view.trail_right.emitting).is_true()


func test_a_car_drives_on_after_the_host_takes_its_control() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await game.until(host.session.find_scene.bind(&"Track"))
	await game.until(client.session.find_scene.bind(&"Track"))
	var own: Node = await game.until(client.player.bind(&"luigi"))
	var host_view: Node = await game.until(host.player.bind(&"luigi"))
	await game.run(4)
	assert_bool(own.sphere.freeze).is_false()
	assert_bool(host_view.sphere.freeze).is_true()

	host_view.entity.controller = Netw.session(host_view).local_player
	await game.run(10)
	assert_bool(host_view.entity.is_controlled_locally).is_true()
	assert_bool(host_view.sphere.freeze).override_failure_message(
		"the host controls the car now, so its sphere runs there",
	).is_false()
	assert_bool(own.sphere.freeze).override_failure_message(
		"the client gave the car up, so its sphere is a proxy there",
	).is_true()

	var start: Vector3 = host_view.sphere_position
	var client_start: Vector3 = own.display_position
	host.press(&"forward")
	await game.run(90)
	host.release(&"forward")

	var travelled: float = host_view.sphere_position.distance_to(start)
	assert_float(travelled).override_failure_message(
		"the new controller drives the car it was given",
	).is_greater(2.0)
	assert_float(own.display_position.distance_to(client_start)) \
			.override_failure_message(
				"the old controller's view follows the pose the host relays",
			).is_greater(travelled * 0.5)


func hold(_delta: float, _tick: int, _is_fresh: bool) -> void:
	pass
