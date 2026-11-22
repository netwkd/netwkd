class_name NakamaTestSupport
extends RefCounted

const PEER_CLASS := &"NakamaRelayPeer"

const RUN_PREFIX_ENV := "NETW_NAKAMA_TEST_RUN"

static var prefix := ""


static func start_host(
		parent: Node,
		username: String = "host",
) -> Dictionary:
	var tree := make_tree(parent, "NakamaHost")
	if await sign_in(tree, username) != OK:
		tree.queue_free()
		return { }
	var settled := await NetwOrdinarySetup.connect_through(
		tree,
		PEER_CLASS,
		NetwConnection.TRANSPORT_MODE_HOST,
		"",
		{ },
		StringName(username),
	)
	if settled.error != OK:
		push_error(
			"NakamaTestSupport could not host (%s, %s)." % [
				error_string(settled.error),
				settled.detail,
			],
		)
		tree.queue_free()
		return { }
	return {
		tree = tree,
		room = Netw.session(tree).connection.join_address,
	}


static func join_client(
		client: MultiplayerViewport,
		room: String,
		username: String,
) -> Error:
	var signed_in := await sign_in(client, username)
	if signed_in != OK:
		return signed_in
	var settled := await NetwOrdinarySetup.connect_through(
		client,
		PEER_CLASS,
		NetwConnection.TRANSPORT_MODE_CLIENT,
		room,
		{ },
		StringName(username),
	)
	return settled.error


static func sign_in(tree: MultiplayerViewport, username: String) -> Error:
	var nakama := account(tree)
	if nakama.socket != null:
		return OK
	var made := await nakama.client.authenticate_device_async(
		"%s-%s" % [run_prefix(), username],
		null,
		true,
	)
	if made.is_exception():
		push_error(
			"NakamaTestSupport could not sign in %s (%s)."
			% [username, made.get_exception().message],
		)
		return ERR_UNAUTHORIZED
	nakama.use_session(made)
	var signing_in := nakama.sign_in()
	await signing_in.wait()
	return signing_in.code


static func make_client_tree(
		parent: Node,
		username: String,
) -> MultiplayerViewport:
	return make_tree(parent, "NakamaClient_%s" % username)


static func stop_tree(tree: MultiplayerViewport) -> void:
	if not is_instance_valid(tree):
		return
	var scene_tree := tree.get_tree()
	var dir := directory(tree)
	if dir != null:
		await dir._leave_lobby()
	tree.queue_free()
	if scene_tree:
		for i in 5:
			await scene_tree.process_frame


static func make_tree(parent: Node, tree_name: String) -> MultiplayerViewport:
	var tree := MultiplayerViewport.new()
	tree.name = tree_name
	var nakama := NakamaAccount.new()
	nakama.name = &"NakamaAccount"
	nakama.host = NakamaTestServer.host()
	nakama.port = NakamaTestServer.DEFAULT_PORT
	nakama.use_ssl = false
	tree.add_child(nakama)
	var dir := NakamaLobbyDirectory.new()
	dir.name = &"NakamaLobbyDirectory"
	tree.add_child(dir)
	parent.add_child(tree)
	return tree


static func account(tree: MultiplayerViewport) -> NakamaAccount:
	for child in tree.find_children("*", "NakamaAccount", true):
		return child as NakamaAccount
	return tree.get_node_or_null("NakamaAccount") as NakamaAccount


static func directory(tree: MultiplayerViewport) -> NakamaLobbyDirectory:
	for child in tree.find_children("*", "NakamaLobbyDirectory", true):
		return child as NakamaLobbyDirectory
	return tree.get_node_or_null("NakamaLobbyDirectory") \
			as NakamaLobbyDirectory


static func run_prefix() -> String:
	if not prefix.is_empty():
		return prefix
	var env_prefix := OS.get_environment(RUN_PREFIX_ENV)
	if not env_prefix.is_empty():
		prefix = env_prefix
	else:
		prefix = "netw-%d-%d" % [Time.get_unix_time_from_system(), randi()]
	return prefix
