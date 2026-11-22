extends GdUnitTestSuite

const GAME := "res://addons/netwkd/examples/playground/"
const MAIN := preload(GAME + "main.tscn")
const PlayPlayer := preload(GAME + "game/player.gd")

var game: NetwTestGame


func before_test() -> void:
	game = NetwTestGame.new(MAIN)
	game.failed.connect(fail)
	add_child(game)


func after_test() -> void:
	game.free()


func test_a_host_and_a_client_share_the_playground() -> void:
	var host := await game.host(&"hana")
	var ada := await game.join(&"ada")
	await game.until(host.player)
	await game.until(ada.player)
	await game.run(30)

	for peer: NetwTestPeer in [host, ada]:
		var root: Node = peer.session.find_scene(&"Playground").root
		var player := peer.player() as PlayPlayer
		assert_int(root.get_node(^"Cubes").get_child_count()).is_equal(900)
		assert_int(root.get_node(^"Players").get_child_count()).is_equal(2)
		assert_bool(player.entity.is_controlled_locally).is_true()
