class_name TestNakamaPeerAwareness
extends GdUnitTestSuite

const TIMEOUT := 10.0

var trees: Array[MultiplayerViewport] = []


@warning_ignore("unused_parameter")
func before(
		do_skip = NakamaTestServer.unavailable(),
		skip_reason = NakamaTestServer.SKIP_REASON,
) -> void:
	pass


func after_test() -> void:
	for tree in trees:
		if is_instance_valid(tree):
			await NakamaTestSupport.stop_tree(tree)
	trees.clear()
	await super.after_test()


func test_disconnect_propagates_to_remaining_peers() -> void:
	var host := await NakamaTestSupport.start_host(self, "host")
	var host_tree: MultiplayerViewport = host.tree
	var alice := NakamaTestSupport.make_client_tree(self, "alice")
	var bruno := NakamaTestSupport.make_client_tree(self, "bruno")
	trees.assign([host_tree, alice, bruno])
	assert_int(await NakamaTestSupport.join_client(alice, host.room, "alice")) \
			.is_equal(OK)
	assert_int(await NakamaTestSupport.join_client(bruno, host.room, "bruno")) \
			.is_equal(OK)
	await wait_for(
		func() -> bool: return trees.all(sees_everyone),
		"all Nakama peers to connect",
	)

	var alice_id := alice.multiplayer.get_unique_id()
	trees.erase(alice)
	await NakamaTestSupport.stop_tree(alice)
	await wait_for(
		func() -> bool:
			return not host_tree.multiplayer.get_peers().has(alice_id) \
					and not bruno.multiplayer.get_peers().has(alice_id) \
					and Netw.session(host_tree).players.size() == 2,
		"the remaining peers to drop the client that left",
	)

	var server_lost: Array[bool] = [false]
	bruno.multiplayer.server_disconnected.connect(
		func() -> void: server_lost[0] = true,
	)
	trees.erase(host_tree)
	await NakamaTestSupport.stop_tree(host_tree)
	await wait_for(
		func() -> bool:
			return server_lost[0] or not Netw.session(bruno).is_online,
		"the client to see the host leave",
	)


func sees_everyone(tree: MultiplayerViewport) -> bool:
	return Netw.session(tree).is_online \
			and tree.multiplayer.get_peers().size() == 2 \
			and Netw.session(tree).players.size() == 3


func wait_for(condition: Callable, label: String) -> void:
	var deadline := get_tree().create_timer(TIMEOUT)
	while deadline.time_left > 0.0 and not condition.call():
		await get_tree().process_frame
	assert_bool(condition.call()) \
			.override_failure_message("Timed out waiting for %s." % label) \
			.is_true()
