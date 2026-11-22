extends GdUnitTestSuite

const GAME := "res://addons/netwkd/examples/"
const MAIN := preload(GAME + "bomber/main.tscn")
const BomberGamestate := preload(GAME + "bomber/game/gamestate.gd")
const Bomb := preload(GAME + "bomber/game/bomb.gd")
const Score := preload(GAME + "bomber/game/score.gd")
const InLobby := preload(GAME + "shared/in_lobby.gd")

var game: NetwTestGame


func before_test() -> void:
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	game.free()


func test_an_accepted_invite_joins_the_host() -> void:
	var host := await game.host(&"host")
	var guest := MultiplayerViewport.new()
	guest.add_child(MAIN.instantiate())
	add_child(guest)
	var session: NetwSession = Netw.session(guest)
	await get_tree().process_frame

	session.connection.invite(&"LocalMultiplayerPeer", "local", "host")
	await game.until(func() -> bool: return session.local_player != null)

	assert_bool(session.is_online).is_true()
	assert_object(session.local_player).is_not_null()
	assert_int(host.session.players.size()).is_equal(2)

	await session.leave().wait()
	guest.queue_free()
	await get_tree().process_frame


func test_client_moves_and_drops_a_bomb_every_peer_sees() -> void:
	var host := await game.host(&"host")
	var client := await game.join(&"client")
	await start_match(host)
	var host_level: NetwScene = await game.until(
		host.session.find_scene.bind(&"World"),
	)
	var client_level: NetwScene = await game.until(
		client.session.find_scene.bind(&"World"),
	)
	var player: Node2D = await game.until(client.player.bind(&"client"))
	var host_player: Node2D = await game.until(host.player.bind(&"host"))
	await game.run(8)
	var start := player.position.x
	var host_start := host_player.position.x

	client.press(&"move_right")
	client.press(&"set_bomb")
	await game.run(12)
	client.release(&"move_right")
	client.release(&"set_bomb")
	await game.run(4)

	assert_float(player.position.x).is_greater(start)
	assert_float(host_player.position.x).is_equal(host_start)
	assert_int(bombs(host_level)).is_greater(0)
	assert_int(bombs(client_level)).is_equal(bombs(host_level))


func test_explosion_scores_rocks_and_stuns_players() -> void:
	var host := await game.host(&"host")
	var client := await game.join(&"client")
	await start_match(host)
	var level: NetwScene = await game.until(
		host.session.find_scene.bind(&"World"),
	)
	var client_view: Node2D = await game.until(client.player.bind(&"client"))
	var client_on_host: Node2D = await game.until(host.player.bind(&"client"))
	await game.run(8)

	host.press(&"set_bomb")
	await game.run(6)
	host.release(&"set_bomb")
	await game.run(2)
	var bomb: Bomb = level.root.get_node(^"Bombs").get_child(0)
	var score: Score = level.root.get_node(^"Score")
	var before: int = score.get_score(host.session.local_player)

	bomb.from_player = host.session.local_player.peer_id
	bomb.in_area = [level.root.get_node(^"Rocks").get_child(0), client_on_host]
	bomb.explode()
	await game.run(6)

	assert_int(score.get_score(host.session.local_player)).is_greater(before)
	assert_bool(client_on_host.get(&"stunned")).is_true()
	assert_bool(client_view.get(&"stunned")).is_true()


func test_exit_returns_every_peer_to_the_lobby() -> void:
	var host := await game.host(&"host")
	var client := await game.join(&"client")
	await start_match(host)
	await game.until(client.session.find_scene.bind(&"World"))

	var level: NetwScene = host.session.find_scene(&"World")
	(level.root.get_node(^"Winner/ExitGame") as Button).pressed.emit()
	await game.until(
		func() -> bool: return lobby(host).visible and lobby(client).visible,
	)

	assert_bool(lobby(host).visible).is_true()
	assert_bool(lobby(client).visible).is_true()
	assert_that(client.player(&"client")).is_null()


func test_a_host_leaving_mid_match_hides_the_client_lobby() -> void:
	var host := await game.host(&"host")
	var client := await game.join(&"client")
	await start_match(host)
	await game.until(client.session.find_scene.bind(&"World"))

	host.session.leave()
	await game.until(func() -> bool: return not client.session.is_online)
	await game.run(4)

	assert_bool(client.session.is_online).is_false()
	assert_bool(lobby(client).visible).is_false()


func start_match(host: NetwTestPeer) -> void:
	await game.until(lobby(host).is_visible)
	(host.session.service(BomberGamestate) as BomberGamestate).begin_game()


func lobby(peer: NetwTestPeer) -> InLobby:
	return peer.scene.find_child("InLobby", true, false) as InLobby


func bombs(level: NetwScene) -> int:
	return level.root.get_node(^"Bombs").get_child_count()
