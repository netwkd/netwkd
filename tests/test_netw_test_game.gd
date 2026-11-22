extends GdUnitTestSuite

const GAME := "res://addons/netwkd/examples/"
const MAIN := preload(GAME + "bomber/main.tscn")
const BomberGamestate := preload(GAME + "bomber/game/gamestate.gd")
const InLobby := preload(GAME + "shared/in_lobby.gd")

var game: NetwTestGame


func before_test() -> void:
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	game.free()


func test_input_reaches_only_the_peer_that_sent_it() -> void:
	var hana := await game.host(&"hana")
	var ada := await game.join(&"ada")
	var lobby := hana.scene.find_child("InLobby", true, false) as InLobby
	await game.until(lobby.is_visible)
	(hana.session.service(BomberGamestate) as BomberGamestate).begin_game()
	var mine: Node2D = await game.until(ada.player)
	var theirs: Node2D = await game.until(hana.player)
	await game.run(8)
	var start := mine.position
	var their_start := theirs.position

	ada.press(&"move_right")
	await game.run(8)
	ada.release(&"move_right")

	assert_float(mine.position.x).is_greater(start.x)
	assert_vector(theirs.position).is_equal(their_start)


func test_run_advances_every_clock_by_exactly_the_ticks_asked() -> void:
	var hana := await game.host(&"hana")
	var ada := await game.join(&"ada")
	var host_start := hana.session.clock.tick
	var client_start := ada.session.clock.tick

	await game.run(10)

	assert_int(hana.session.clock.tick - host_start).is_equal(10)
	assert_int(ada.session.clock.tick - client_start).is_equal(10)


func test_an_impaired_client_hears_of_a_join_later_until_healed() -> void:
	await game.host(&"hana")
	var ada := await game.join(&"ada")
	var slow := NetwLinkConditions.new()
	slow.latency_ms = 1000.0

	ada.impair(slow)
	var start := ada.session.clock.tick
	await game.join(&"bob")
	await game.until(func() -> bool: return ada.session.players.size() == 3)
	var impaired := ada.session.clock.tick - start

	ada.heal()
	start = ada.session.clock.tick
	await game.join(&"cleo")
	await game.until(func() -> bool: return ada.session.players.size() == 4)
	var healed := ada.session.clock.tick - start

	assert_int(impaired).is_greater(healed + 5)


func test_drop_takes_the_player_off_the_server_roster() -> void:
	var hana := await game.host(&"hana")
	var ada := await game.join(&"ada")

	await ada.drop()

	assert_array(hana.session.players).has_size(1)


func test_until_answers_its_condition_and_fails_once_it_gives_up() -> void:
	var hana := await game.host(&"hana")
	var messages: Array[String] = []
	game.failed.disconnect(fail)
	game.failed.connect(messages.append)

	var seated: Variant = await game.until(hana.session.player_of.bind(1))
	var missing: Variant = await game.until(hana.player.bind(&"bob"), 5)

	assert_object(seated).is_same(hana.session.local_player)
	assert_that(missing).is_null()
	assert_array(messages).contains_exactly(
		[
			'Waited 5 ticks for player(&"bob"). Pass a larger tick count or '
			+ "check the condition.",
		],
	)


func test_serve_hosts_with_no_player_and_a_client_joins() -> void:
	var server := await game.serve()
	var ada := await game.join(&"ada")

	assert_that(server.session.local_player).is_null()
	assert_array(server.session.players).has_size(1)
	assert_that(ada.session.local_player).is_not_null()


func test_show_lays_every_peer_out_side_by_side() -> void:
	game.show()
	var hana := await game.host(&"hana")
	var ada := await game.join(&"ada")
	await game.run(1)

	assert_vector(ada.viewport.get_parent().position) \
			.is_not_equal(hana.viewport.get_parent().position)
