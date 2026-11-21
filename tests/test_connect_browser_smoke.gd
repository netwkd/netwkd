@tool
class_name TestConnectBrowserSmoke
extends NetwTestSuite

const BROWSER := preload(
	"res://addons/netwkd/gdscript/connect/ui/connect_browser.tscn"
)

var tree: MultiplayerViewport
var session: NetwSession
var connection: NetwConnection
var browser: ConnectBrowser


class AnotherGame:
	extends Node

	func _init() -> void:
		Netw.configure_server_info(answer)


	func answer(info: NetwServerInfo) -> void:
		info.app_id = &"another-game"


class SmallServer:
	extends Node

	var max_players := 8


	func _init() -> void:
		Netw.configure_server_info(answer)


	func answer(info: NetwServerInfo) -> void:
		info.max_players = max_players


class TeamGame:
	extends Node

	var joined_team := 0


	func _init() -> void:
		Netw.configure_join(enter)


	func enter(_player: NetwPlayer, team: int) -> void:
		joined_team = team


func before_test() -> void:
	tree = MultiplayerViewport.new()
	add_child(tree)
	session = Netw.session(tree)
	connection = session.connection
	browser = BROWSER.instantiate()
	tree.add_child(browser)


func after_test() -> void:
	tree.queue_free()
	await get_tree().process_frame


func test_rows_follow_added_and_removed_endpoints() -> void:
	var endpoint := connection.add(
		&"ENetMultiplayerPeer",
		"203.0.113.1",
		"Alpha",
	)

	assert_int(browser._list_box.get_child_count()).is_equal(1)
	assert_str(browser._rows[endpoint]._name_label.text).is_equal("Alpha")
	assert_bool(browser._empty_state.visible).is_false()

	connection.remove(endpoint)
	await get_tree().process_frame

	assert_int(browser._list_box.get_child_count()).is_equal(0)
	assert_bool(browser._empty_state.visible).is_true()


func test_a_probe_answer_reaches_the_row_and_the_join_button() -> void:
	var host := await EnetTestSupport.start_host(self, AnotherGame.new())
	var endpoint := connection.add(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
	)
	browser._rows[endpoint].selected.emit()

	assert_bool(browser._details.visible).is_true()
	assert_bool(browser._join_button.disabled).is_false()

	await EnetTestSupport.await_until(
		self,
		func() -> bool: return endpoint.status != NetwEndpoint.STATUS_UNKNOWN,
	)

	assert_int(endpoint.status).is_equal(NetwEndpoint.STATUS_INCOMPATIBLE)
	assert_str(browser._rows[endpoint]._status_dot.tooltip_text) \
			.is_equal("Incompatible")
	assert_bool(browser._join_button.disabled).is_true()
	await EnetTestSupport.stop_tree(host.tree)


func test_the_details_panel_follows_the_selected_endpoint() -> void:
	var server := SmallServer.new()
	var host := await EnetTestSupport.start_host(self, server)
	var endpoint := connection.add(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
	)
	await EnetTestSupport.await_until(
		self,
		func() -> bool: return endpoint.status == NetwEndpoint.STATUS_ONLINE,
	)
	browser._rows[endpoint].selected.emit()

	assert_str(browser._details_status_label.text).is_equal("Online")
	assert_str(browser._details_ping_label.text).ends_with(" ms")
	assert_str(browser._details_players_label.text).is_equal("1/8")

	server.max_players = 4
	endpoint.probe()
	await EnetTestSupport.await_until(
		self,
		func() -> bool: return endpoint.info.max_players == 4,
	)

	assert_str(browser._details_players_label.text).is_equal("1/4")
	await EnetTestSupport.stop_tree(host.tree)


func test_browsing_follows_the_server_list_visibility() -> void:
	assert_bool(connection.browsing).is_true()

	browser.hide()
	assert_bool(connection.browsing).is_false()

	browser.show()
	assert_bool(connection.browsing).is_true()

	session.state_changed.emit(NetwSession.STATE_ONLINE)
	assert_bool(browser.visible).is_true()
	assert_bool(connection.browsing).is_false()

	session.state_changed.emit(NetwSession.STATE_OFFLINE)
	assert_bool(connection.browsing).is_true()


func test_the_connecting_popup_follows_state_and_progress() -> void:
	session.state_changed.emit(NetwSession.STATE_CREATING)
	assert_bool(browser._connecting_popup.visible).is_true()

	connection.progressed.emit("Asking the tracker", 0.5)
	assert_str(browser._connecting_popup._title_label.text) \
			.is_equal("Asking the tracker")
	assert_float(browser._connecting_popup._progress_bar.value).is_equal(0.5)

	session.state_changed.emit(NetwSession.STATE_ONLINE)
	assert_bool(browser._connecting_popup.visible).is_false()


func test_a_failure_reaches_the_banner_and_the_popup() -> void:
	session.state_changed.emit(NetwSession.STATE_CONNECTING)
	connection.failed.emit(ERR_CANT_CONNECT, "The server could not be reached.")
	session.state_changed.emit(NetwSession.STATE_OFFLINE)
	await get_tree().process_frame

	assert_bool(browser._banner.visible).is_true()
	assert_str(browser._banner_label.text) \
			.is_equal("The server could not be reached.")
	assert_bool(browser._connecting_popup.visible).is_true()
	assert_str(browser._connecting_popup._cancel_button.text).is_equal("Close")

	session.state_changed.emit(NetwSession.STATE_CREATING)
	assert_bool(browser._banner.visible).is_false()


func test_a_host_refused_its_own_join_reaches_the_banner() -> void:
	browser.host(&"LocalMultiplayerPeer", { }, NetwServerInfo.new(), [])
	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)

	session.join_failed.emit(ERR_TIMEOUT, "The join ran out of time.")
	await get_tree().process_frame

	assert_bool(browser._banner.visible).is_true()
	assert_str(browser._banner_label.text).is_equal("The join ran out of time.")


func test_the_room_bar_shows_the_code_while_hosting() -> void:
	var room_bar := browser.get_node(^"RoomBar")
	assert_bool(room_bar._panel.visible).is_false()

	browser.host(&"LocalMultiplayerPeer", { }, NetwServerInfo.new(), [])

	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)
	assert_bool(room_bar._panel.visible).is_true()
	assert_str(room_bar._code_edit.text).is_equal("local")


func test_a_hidden_room_bar_stays_hidden_while_hosting() -> void:
	var room_bar: Control = browser.get_node(^"RoomBar")
	room_bar.hide()

	browser.host(&"LocalMultiplayerPeer", { }, NetwServerInfo.new(), [])

	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)
	assert_bool(room_bar.visible).is_false()


func test_transport_defaults_reach_the_connection() -> void:
	var config := ConnectENetConfig.new()
	config.port = 34567
	config.max_players = 12
	var configured: ConnectBrowser = BROWSER.instantiate()
	configured.transport_defaults = [config]
	tree.add_child(configured)

	var enet := connection.transport(&"ENetMultiplayerPeer")
	assert_int(enet.client_settings.port).is_equal(34567)
	assert_bool(enet.client_settings.has("max_players")).is_false()
	assert_int(enet.host_settings.port).is_equal(34567)
	assert_int(enet.host_settings.max_players).is_equal(12)


func test_a_join_through_the_browser_goes_online_and_hides_the_list() -> void:
	var host := await EnetTestSupport.start_host(self)
	var endpoint := connection.add(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
	)

	browser.join(endpoint, { }, [])

	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)
	assert_int(session.state).is_equal(NetwSession.STATE_ONLINE)
	assert_bool(browser._server_list.visible).is_false()
	assert_bool(browser._connecting_popup.visible).is_false()
	await EnetTestSupport.stop_tree(host.tree)


func test_an_invite_joins_with_the_browser_player_name_and_defaults() -> void:
	var host := await EnetTestSupport.start_host(self)
	browser.username = "InvitedPlayer"
	connection.set_defaults(&"ENetMultiplayerPeer", { port = host.port })

	connection.invite(&"ENetMultiplayerPeer", "127.0.0.1", "Host")

	assert_int(
		await EnetTestSupport.await_until(
			tree,
			func() -> bool: return session.local_player != null,
		),
	).is_equal(OK)
	assert_str(String(session.local_player.username)) \
			.is_equal("InvitedPlayer")
	assert_bool(browser._server_list.visible).is_false()
	await EnetTestSupport.stop_tree(host.tree)


func test_an_invite_requests_declared_join_inputs() -> void:
	var server := TeamGame.new()
	var host := await EnetTestSupport.start_host(self, server)
	tree.add_child(TeamGame.new())
	await get_tree().process_frame

	var endpoint := connection.invite(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
		"Host",
	)

	assert_object(browser._selected).is_same(endpoint)
	assert_bool(browser._join_popup.visible).is_true()
	assert_int(session.state).is_equal(NetwSession.STATE_OFFLINE)
	var inputs := browser._join_popup._args_container.find_children(
		"*",
		"SpinBox",
		true,
		false,
	)
	assert_int(inputs.size()).is_equal(1)
	(inputs[0] as SpinBox).value = 2
	browser._join_popup._confirm_button.pressed.emit()

	assert_int(
		await EnetTestSupport.await_until(
			tree,
			func() -> bool: return session.local_player != null,
		),
	).is_equal(OK)
	assert_int(server.joined_team).is_equal(2)
	await EnetTestSupport.stop_tree(host.tree)


func test_an_invite_leaves_the_current_lobby_only_after_confirmation() -> void:
	browser.host(&"LocalMultiplayerPeer", { }, NetwServerInfo.new(), [])
	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)
	var original := tree.api.multiplayer_peer
	var host := await EnetTestSupport.start_host(self)

	connection.invite(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
		"Host",
	)

	assert_bool(browser._join_popup.visible).is_true()
	assert_bool(browser._join_popup._leave_warning.visible).is_true()
	assert_object(tree.api.multiplayer_peer).is_same(original)
	browser._join_popup._cancel_button.pressed.emit()
	assert_bool(browser._join_popup.visible).is_false()
	assert_object(tree.api.multiplayer_peer).is_same(original)

	connection.invite(
		&"ENetMultiplayerPeer",
		"127.0.0.1:%d" % host.port,
		"Host",
	)
	browser._join_popup._confirm_button.pressed.emit()

	assert_int(
		await EnetTestSupport.await_until(
			tree,
			func() -> bool:
				return tree.api.multiplayer_peer is ENetMultiplayerPeer \
						and session.local_player != null,
		),
	).is_equal(OK)
	assert_int(session.state).is_equal(NetwSession.STATE_ONLINE)
	assert_bool(browser._server_list.visible).is_false()
	await EnetTestSupport.stop_tree(host.tree)


func test_an_invite_waits_for_the_game_while_join_invites_is_off() -> void:
	browser.join_invites = false

	connection.invite(&"ENetMultiplayerPeer", "127.0.0.1", "Host")

	assert_int(session.state).is_equal(NetwSession.STATE_OFFLINE)
	assert_object(browser._selected).is_null()
	assert_bool(browser._join_popup.visible).is_false()

	browser.host(&"LocalMultiplayerPeer", { }, NetwServerInfo.new(), [])
	assert_int(await EnetTestSupport.await_online(tree)).is_equal(OK)
	connection.invite(&"ENetMultiplayerPeer", "127.0.0.1", "Host")

	assert_bool(browser._join_popup.visible).is_false()

	browser.join_invites = true
	connection.invite(&"ENetMultiplayerPeer", "127.0.0.1", "Host")

	assert_bool(browser._join_popup.visible).is_true()
