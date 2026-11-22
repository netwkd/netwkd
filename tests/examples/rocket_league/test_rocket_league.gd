extends GdUnitTestSuite

const GAME := "res://addons/netwkd/examples/"
const MAIN := preload(GAME + "rocket_league/main.tscn")
const RocketBall := preload(GAME + "rocket_league/scripts/ball.gd")
const RocketCar := preload(GAME + "rocket_league/scripts/player_rb.gd")
const RocketGame := preload(GAME + "rocket_league/scripts/game.gd")
const InLobby := preload(GAME + "shared/in_lobby.gd")

var game: NetwTestGame


func before_test() -> void:
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	game.free()


func test_a_host_car_spawns_on_its_marker_and_drives_after_kickoff() -> void:
	var host := await game.host(&"mario")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	assert_that(host.player()).is_equal(car)
	quiet_ai(host)

	await game.run(4)
	assert_float(pose_of(car).origin.distance_to(car.marker.global_position)) \
			.override_failure_message(
				"a car must sit on its own kickoff marker before kickoff",
			).is_less(1.0)

	await await_kickoff(car)
	quiet_ai(host)
	var start: Vector3 = pose_of(car).origin
	var facing: Vector3 = pose_of(car).basis.z
	host.press(&"forward")
	await game.run(90)
	host.release(&"forward")
	assert_float(pose_of(car).origin.distance_to(start)).is_greater(0.5)

	assert_float((pose_of(car).origin - start).dot(facing)) \
			.override_failure_message(
				"the accelerate action must move a car along its own facing",
			).is_greater(0.5)


func test_every_peer_holds_the_other_peers_car() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))

	var own: RocketCar = await game.until(client.player.bind(&"luigi"))
	var host_view: RocketCar = await game.until(host.player.bind(&"luigi"))
	var mario_on_client: Node = await game.until(client.player.bind(&"mario"))
	assert_that(client.player()).is_equal(own)
	assert_bool(own.entity.simulation.physics_driven).is_true()

	await await_kickoff(own)
	quiet_ai(host)
	quiet_ai(client)
	client.press(&"forward")
	await game.run(90)
	client.release(&"forward")

	assert_float(pose_of(own).origin.distance_to(pose_of(host_view).origin)) \
			.override_failure_message(
				"the host's view of a client car must track the client's own",
			).is_less(3.5)
	assert_bool(is_instance_valid(mario_on_client)).is_true()


func test_a_goal_bumps_the_score_on_every_peer_and_queues_a_kickoff() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var server_game := rocket_game(host)
	var client_game := rocket_game(client)
	var ball := arena_ball(host)
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	await game.run(4)

	await await_kickoff(car)
	quiet_ai(host)
	quiet_ai(client)
	var before: int = server_game.score_red
	place(ball, goal_position(host, "Goal0"))
	ball.state.linear_velocity = Vector3.ZERO
	await game.until(func() -> bool: return server_game.score_red > before)

	assert_int(server_game.score_red).override_failure_message(
		"the ball resting in a goal must be ruled a goal exactly once",
	).is_equal(before + 1)
	assert_int(server_game.kickoff_tick).is_greater(0)
	await game.run(20)
	assert_int(client_game.score_red).override_failure_message(
		"every peer reads the score off the same column",
	).is_equal(server_game.score_red)


func test_cars_hold_their_markers_and_the_ball_returns_to_its_spot() -> void:
	var host := await game.host(&"mario")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	var ball := arena_ball(host)
	var server_game := rocket_game(host)
	quiet_ai(host)
	await game.run(4)

	assert_int(server_game.kickoff_tick).override_failure_message(
		"a join queues a kickoff",
	).is_greater(0)
	host.press(&"forward")
	await game.run(30)
	host.release(&"forward")
	assert_float(pose_of(car).origin.distance_to(car.marker.global_position)) \
			.override_failure_message(
				"input must not move a car that is held for the kickoff",
			).is_less(1.0)

	await await_kickoff(car)
	var spot := RocketBall.STARTING_POSITION
	assert_float(pose_of(ball).origin.distance_to(spot)) \
			.override_failure_message(
				"the reset must put the ball back on its spot before play",
			).is_less(1.0)


func test_the_ball_holds_one_route_on_every_peer_and_keeps_its_own_body() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var host_ball := arena_ball(host)
	var client_ball := arena_ball(client)
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	await await_kickoff(car)
	quiet_ai(host)
	quiet_ai(client)
	await game.run(10)

	assert_str(String(host_ball.entity.name)).is_equal("ball")
	assert_str(String(client_ball.entity.name)).is_equal("ball")

	assert_bool(host_ball.entity.simulation.physics_driven) \
			.override_failure_message(
				"the ball keeps its own prediction body on every peer",
			).is_true()
	assert_bool(client_ball.entity.simulation.physics_driven).is_true()
	assert_that(client_ball.entity.scene.root).override_failure_message(
		"the client's ball answers the arena its route was published under",
	).is_equal(arena_level(client))


func test_a_car_drives_itself_toward_the_ball_with_no_local_input() -> void:
	var host := await game.host(&"mario")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	assert_bool(car.ai_enabled).override_failure_message(
		"a car drives itself until its controller takes the wheel",
	).is_true()

	await await_kickoff(car)
	var ball := arena_ball(host)
	var before: float = pose_of(car).origin.distance_to(pose_of(ball).origin)
	await game.run(120)
	var after: float = pose_of(car).origin.distance_to(pose_of(ball).origin)
	assert_float(after).override_failure_message(
		"the AI closed %.2fm to %.2fm on the ball" % [before, after],
	).is_less(before)


func test_a_clean_client_drive_opens_no_correction() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var own: RocketCar = await game.until(client.player.bind(&"luigi"))
	await game.until(host.player.bind(&"luigi"))

	await await_kickoff(own)
	quiet_ai(host)
	quiet_ai(client)
	var at_kickoff: int = own.entity.prediction.correction_count
	await game.run(16)
	var corrections: int = own.entity.prediction.correction_count
	client.press(&"forward")
	await game.run(120)
	client.release(&"forward")

	var opened: int = own.entity.prediction.correction_count
	assert_int(opened) \
			.override_failure_message(
				"a straight drive with no contact must reconcile clean: "
				+ "%d at kickoff, %d after the settle, %d after the drive"
				% [at_kickoff, corrections, opened],
			).is_equal(corrections)


func test_the_lobby_lists_every_waiting_player_before_the_match_opens() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await game.until(in_lobby(host).is_visible)
	await game.until(in_lobby(client).is_visible)
	await game.run(4)

	var roster := in_lobby(host).member_list
	assert_int(roster.item_count).override_failure_message(
		"the lobby roster carries every seated player",
	).is_equal(2)
	assert_bool(in_lobby(client).start_btn.visible).override_failure_message(
		"only the host starts the match",
	).is_false()

	in_lobby(host).start_btn.pressed.emit()
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	assert_that(await game.until(client.player.bind(&"luigi"))).is_not_null()
	assert_bool(in_lobby(client).visible).override_failure_message(
		"the lobby hides once the match is live",
	).is_false()


func test_a_clients_ball_responds_in_the_frame_its_car_touches_it() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var own: RocketCar = await game.until(client.player.bind(&"luigi"))
	var host_ball := arena_ball(host)
	var client_ball := arena_ball(client)
	own.contact_monitor = true
	await await_kickoff(own)
	quiet_ai(host)
	quiet_ai(client)
	await game.run(4)
	assert_int(client_ball.entity.simulation.mode).override_failure_message(
		"the client's car selects the ball, so the client runs it",
	).is_equal(NetwSimulation.MODE_ACTIVE)

	var ahead: Vector3 = pose_of(own).origin + pose_of(own).basis.z * 5.0
	ahead.y = RocketBall.STARTING_POSITION.y
	place(host_ball, ahead)
	host_ball.state.linear_velocity = Vector3.ZERO
	var still := 0
	for tick in 400:
		await game.run(1)
		var resting: bool = host_ball.state.linear_velocity.length() < 0.05
		still = still + 1 if resting else 0
		if still >= 10:
			break
	client.impair(slow_link())
	await game.run(20)

	client.press(&"forward")
	var touched := -1
	var moved := -1
	for tick in 240:
		await game.run(1)
		if touched < 0 and own.get_colliding_bodies().has(client_ball):
			touched = tick
		var drift := client_ball.state.linear_velocity * Vector3(1.0, 0.0, 1.0)
		if touched >= 0 and drift.length() > 2.0:
			moved = tick
			break
	client.release(&"forward")

	assert_int(touched).override_failure_message(
		"the client's car never reached the ball",
	).is_greater_equal(0)
	assert_int(moved - touched).override_failure_message(
		"the client's ball moved %d ticks after its car touched it"
		% (moved - touched),
	).is_between(0, 2)


func test_a_clients_hit_on_a_bouncing_ball_takes_effect_at_once() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var own: RocketCar = await game.until(client.player.bind(&"luigi"))
	var host_ball := arena_ball(host)
	var client_ball := arena_ball(client)
	own.contact_monitor = true
	await await_kickoff(own)
	quiet_ai(host)
	quiet_ai(client)
	await game.run(4)
	assert_int(client_ball.entity.simulation.mode).override_failure_message(
		"the client's car selects the ball, so the client runs it",
	).is_equal(NetwSimulation.MODE_ACTIVE)

	var ahead: Vector3 = pose_of(own).origin + pose_of(own).basis.z * 5.0
	ahead.y = RocketBall.STARTING_POSITION.y
	place(host_ball, ahead)
	host_ball.state.linear_velocity = Vector3.ZERO
	client.impair(slow_link())
	for tick in 20:
		await game.run(1)
		bounce(host_ball)

	client.press(&"forward")
	var touched := -1
	var moved := -1
	var server_speed := 0.0
	for tick in 240:
		if touched < 0:
			bounce(host_ball)
			var speed := host_ball.state.linear_velocity.length()
			server_speed = maxf(server_speed, speed)
		await game.run(1)
		if touched < 0 and own.get_colliding_bodies().has(client_ball):
			touched = tick
		var drift := client_ball.state.linear_velocity * Vector3(1.0, 0.0, 1.0)
		if touched >= 0 and drift.length() > 2.0:
			moved = tick
			break
	client.release(&"forward")

	assert_int(touched).override_failure_message(
		"the client's car never reached the ball",
	).is_greater_equal(0)
	assert_float(server_speed).override_failure_message(
		"the server's ball rested while the client's car drove at it",
	).is_greater(0.5)
	assert_int(moved - touched).override_failure_message(
		"the client's ball moved %d ticks after its car touched it"
		% (moved - touched),
	).is_between(0, 2)


func test_a_clients_ball_visibly_rotates_with_its_state() -> void:
	var host := await game.host(&"mario")
	var client := await game.join(&"luigi")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	await game.until(client.session.find_scene.bind(&"Arena"))
	var host_ball := arena_ball(host)
	var client_ball := arena_ball(client)
	var car: RocketCar = await game.until(host.player.bind(&"mario"))
	await await_kickoff(car)
	quiet_ai(host)

	host_ball.state.angular_velocity = Vector3(0.0, 6.0, 0.0)
	var before := client_ball.transform.basis.get_rotation_quaternion()
	await game.run(60)
	var after := client_ball.transform.basis.get_rotation_quaternion()
	assert_float(after.angle_to(before)) \
			.override_failure_message(
				"a client's replicated ball spun but its drawn transform "
				+ "did not follow",
			).is_greater(0.05)


func test_a_late_join_gets_a_car_on_the_other_team() -> void:
	var host := await game.host(&"mario")
	await begin_match(host)
	await game.until(host.session.find_scene.bind(&"Arena"))
	var first: RocketCar = await game.until(host.player.bind(&"mario"))
	assert_int(first.team).is_equal(0)

	var client := await game.join(&"luigi")
	await game.until(client.session.find_scene.bind(&"Arena"))
	var late: RocketCar = await game.until(client.player.bind(&"luigi"))
	assert_that(client.player()).override_failure_message(
		"a peer joining a running match is given a car of its own",
	).is_equal(late)
	assert_int(late.team).override_failure_message(
		"a late join takes the next grid slot, so it lands on the other team",
	).is_equal(1)
	assert_that(await game.until(host.player.bind(&"luigi"))).is_not_null()


func slow_link() -> NetwLinkConditions:
	var slow := NetwLinkConditions.new()
	slow.latency_ms = 300.0
	return slow


func bounce(ball: RocketBall) -> void:
	if pose_of(ball).origin.y < 1.1 and ball.state.linear_velocity.y <= 0.0:
		ball.state.linear_velocity = Vector3(0.0, 2.0, 0.0)


func pose_of(body: RigidBody3D) -> Transform3D:
	if body.freeze:
		return body.global_transform
	return PhysicsServer3D.body_get_direct_state(body.get_rid()).transform


func place(ball: RocketBall, at: Vector3) -> void:
	ball.state.transform = Transform3D(ball.state.transform.basis, at)


func begin_match(host: NetwTestPeer) -> void:
	await game.until(in_lobby(host).is_visible)
	in_lobby(host).start_btn.pressed.emit()


func in_lobby(peer: NetwTestPeer) -> InLobby:
	return peer.scene.find_child("InLobby", true, false) as InLobby


func quiet_ai(peer: NetwTestPeer) -> void:
	for child in arena_level(peer).get_node(^"Players").get_children():
		if child is RocketCar:
			(child as RocketCar).ai_enabled = false


func await_kickoff(car: RocketCar) -> void:
	await game.until(
		func() -> bool:
			return car.game.kickoff_tick > 0 \
					and car.session.clock.tick >= car.game.kickoff_tick,
		400,
	)


func arena_level(peer: NetwTestPeer) -> Node:
	return peer.session.find_scene(&"Arena").root


func arena_ball(peer: NetwTestPeer) -> RocketBall:
	return arena_level(peer).get_node(^"ball")


func rocket_game(peer: NetwTestPeer) -> RocketGame:
	return arena_level(peer).get_node(^"game")


func goal_position(peer: NetwTestPeer, named: String) -> Vector3:
	var goal := arena_level(peer).get_node(NodePath("field/%s" % named))
	return (goal as Node3D).global_position
