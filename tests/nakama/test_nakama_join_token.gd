class_name TestNakamaJoinToken
extends GdUnitTestSuite

const KEY := "res://tests/nakama/join_token/test_only_join_key.pub"
const AUDIENCE := "netw-test"
const PORT := 17350
const WAIT_MS := 15000


class Gate extends Node:
	var requests: Array[NetwJoinRequest] = []


	func admit(request: NetwJoinRequest) -> void:
		request.admit()
		if request.peer_id == 1:
			request.account = ""
		else:
			requests.append(request)


var host: Dictionary
var client: MultiplayerViewport
var host_gate: Gate
var client_gate: Gate


@warning_ignore("unused_parameter")
func before(
		do_skip = OS.get_environment("NAKAMA_JOIN_TOKEN_HOST").is_empty(),
		skip_reason = "Start tests/nakama/join_token/docker-compose.yml "
		+ "and set NAKAMA_JOIN_TOKEN_HOST.",
) -> void:
	pass


func before_test() -> void:
	host_gate = Gate.new()
	host = await EnetTestSupport.start_host(self, host_gate, WAIT_MS / 1000.0)
	client = EnetTestSupport.make_client_tree(self, host.port)
	client.api.auth_timeout = WAIT_MS / 1000.0
	client_gate = Gate.new()
	client.add_child(client_gate)


func after_test() -> void:
	await EnetTestSupport.stop_tree(client)
	await EnetTestSupport.stop_tree(host.tree)


func signed_in(parent: Node, device: String) -> NakamaAccount:
	var account := NakamaAccount.new()
	account.host = OS.get_environment("NAKAMA_JOIN_TOKEN_HOST")
	account.port = PORT
	parent.add_child(account)
	account.use_session(
		await account.client.authenticate_device_async(
			"%s-%d-%d" % [device, Time.get_unix_time_from_system(), randi()],
			null,
			true,
		),
	)
	await account.sign_in().wait()
	return account


func test_a_player_joins_as_the_first_nakama_account_in_the_process(
		timeout := WAIT_MS * 2,
) -> void:
	var account := await signed_in(client, "netw-join")
	var shown := "Jose %d" % randi()
	await account.client.update_account_async(account.session, null, shown)
	var other_tree: MultiplayerViewport = auto_free(MultiplayerViewport.new())
	add_child(other_tree)
	await signed_in(other_tree, "netw-join-other")
	var token := NakamaIdToken.new(KEY, AUDIENCE)
	Netw.configure_admission(host_gate.admit).accept(token)
	Netw.configure_admission(client_gate.admit).accept(token)

	var peer := ENetMultiplayerPeer.new()
	peer.create_client("127.0.0.1", host.port)
	Netw.prepare_join(client_gate, &"player")
	client.multiplayer.multiplayer_peer = peer
	var session := Netw.session(client)
	var started := Time.get_ticks_msec()
	while session.local_player == null \
			and Time.get_ticks_msec() - started < WAIT_MS:
		await get_tree().process_frame

	assert_object(session.local_player).is_not_null()
	assert_int(host_gate.requests.size()).is_equal(1)
	if session.local_player == null or host_gate.requests.is_empty():
		return
	var own := "nakama:" + account.user_id
	var request := host_gate.requests[0]
	assert_array(request.accounts) \
			.override_failure_message(
				"The join did not carry the first NakamaAccount's token.",
			).contains_exactly([own])
	assert_str(request.name_of(own)).is_equal(shown)
	assert_str(session.local_player.account).is_equal(own)
