class_name TestNakamaDatabaseLive
extends GdUnitTestSuite

const ERROR_SCRUBBER := preload(
	"res://tests/support/gdunit_error_scrubber.gd"
)

var tree: MultiplayerViewport
var session: NetwSession
var app := ""


@warning_ignore("unused_parameter")
func before(
		do_skip = NakamaTestServer.unavailable(),
		skip_reason = NakamaTestServer.SKIP_REASON,
) -> void:
	pass


func before_test() -> void:
	app = "netwlive%d%d" % [Time.get_ticks_usec(), randi() % 1000]
	tree = auto_free(NakamaTestSupport.make_client_tree(self, "db"))
	session = Netw.session(tree)
	var signed_in := await NakamaTestSupport.sign_in(
		tree,
		"db-%d-%d" % [Time.get_ticks_usec(), randi()],
	)
	assert_int(signed_in) \
			.override_failure_message("Nakama sign in failed.").is_equal(OK)


func after_test() -> void:
	if NakamaTestSupport.account(tree).session != null:
		var database := backend()
		var listing := database._list_slots(session)
		await listing.wait()
		if listing.get_is_completed():
			for slot in listing.get_result() as PackedStringArray:
				await database._delete_slot(session, StringName(slot)).wait()
	await super.after_test()


func backend() -> NakamaDatabase:
	var made := NakamaDatabase.new()
	made.app = app
	return made


func envelope(gold: int) -> Dictionary:
	return {
		"format_version": 1,
		"kind": 0,
		"schema_name": "players",
		"schema_version": 1,
		"payload": { "gold": gold, "where": Vector2(3, 4) },
	}


func address(kind: int, key: String) -> Dictionary:
	return { "kind": kind, "schema_name": "players", "key": key }


func open_slot(
		database: NakamaDatabase,
		slot: String,
) -> NetwDatabaseConnection:
	var promise := database._open(session, StringName(slot))
	await promise.wait()
	assert_bool(promise.get_is_completed()) \
			.override_failure_message(
				"Opening the slot failed: %s" % promise.get_detail(),
			).is_true()
	return promise.get_result() as NetwDatabaseConnection


func test_a_record_survives_the_connection_that_wrote_it() -> void:
	var connection := await open_slot(backend(), "slot1")

	var wrote := connection._write_batch(
		[
			{
				"kind": "replace",
				"address": address(0, "hero"),
				"envelope": envelope(42),
			},
		],
	)
	await wrote.wait()
	var errors: PackedInt32Array = wrote.get_result()["errors"]
	assert_int(errors[0]) \
			.override_failure_message("Nakama refused the write.").is_equal(OK)

	var fresh := await open_slot(backend(), "slot1")
	var read := fresh._read(address(0, "hero"))
	await read.wait()
	var reply: Dictionary = read.get_result()
	assert_int(int(reply["error"])).is_equal(OK)
	assert_bool(bool(reply["found"])) \
			.override_failure_message(
				"A record Nakama acknowledged was not there on a new "
				+ "connection.",
			).is_true()
	var held: Dictionary = reply["envelope"]
	assert_int(int(held["payload"]["gold"])).is_equal(42)
	assert_vector(held["payload"]["where"] as Vector2).is_equal(Vector2(3, 4))

	var missing := fresh._read(address(0, "ghost"))
	await missing.wait()
	assert_int(int(missing.get_result()["error"])).is_equal(OK)
	assert_bool(bool(missing.get_result()["found"])).is_false()


func test_a_foreign_value_is_found_and_unrecognized() -> void:
	var database := backend()
	var connection := await open_slot(database, "slot1")
	var account := NakamaTestSupport.account(tree)
	var legacy := JSON.stringify({ "gold": 4, "name": "hero" })
	await account.client.write_storage_objects_async(
		account.session,
		[
			NakamaWriteStorageObject.new(
				database.collection_for("slot1"),
				NakamaDatabase.address_key(address(0, "hero")),
				1,
				1,
				legacy,
				"",
			),
		],
	)

	var read := connection._read(address(0, "hero"))
	await read.wait()
	var reply: Dictionary = read.get_result()
	assert_int(int(reply["error"])).is_equal(OK)
	assert_bool(bool(reply["found"])) \
			.override_failure_message(
				"A stored value was read as an empty save.",
			).is_true()
	assert_dict(reply["envelope"]).is_empty()


func test_a_refused_session_fails_without_reading_as_absence() -> void:
	var database := backend()
	var signed_in := await open_slot(database, "slot1")
	var wrote := signed_in._write_batch(
		[
			{
				"kind": "replace",
				"address": address(0, "hero"),
				"envelope": envelope(1),
			},
		],
	)
	await wrote.wait()

	var forged_tree: MultiplayerViewport = auto_free(
		NakamaTestSupport.make_client_tree(self, "forged"),
	)
	var forged := NakamaTestSupport.account(forged_tree)
	var claims := Marshalls.utf8_to_base64(
		JSON.stringify(
			{
				"exp": int(Time.get_unix_time_from_system()) + 3600,
				"uid": NakamaTestSupport.account(tree).user_id,
				"usn": "forged",
			},
		),
	)
	forged.use_session(NakamaSession.new("e30.%s.c2ln" % claims))
	await forged.sign_in().wait()
	await ERROR_SCRUBBER.erase_matching(["Invalid status code"])
	await ERROR_SCRUBBER.erase_matching(["Invalid response headers"])
	var refused := NakamaDatabase.Connection.new(
		forged,
		database.collection_for("slot1"),
	)

	var read := refused._read(address(0, "hero"))
	await read.wait()
	var reply: Dictionary = read.get_result()
	assert_int(int(reply["error"])) \
			.override_failure_message(
				"A read Nakama refused did not carry its error.",
			).is_equal(ERR_UNAUTHORIZED)
	assert_bool(bool(reply["found"])).is_false()

	var batch := refused._write_batch(
		[
			{
				"kind": "replace",
				"address": address(0, "hero"),
				"envelope": envelope(2),
			},
			{ "kind": "ponder", "address": address(0, "two") },
		],
	)
	await batch.wait()
	var answer: Dictionary = batch.get_result()
	assert_int(int(answer["error"])).is_equal(OK)
	assert_array(Array(answer["errors"])) \
			.is_equal([ERR_UNAUTHORIZED, ERR_INVALID_DATA])
	assert_array(Array(answer["uncertain"])).is_equal([0, 0])


func test_a_scan_pages_real_storage_and_its_cursor_continues() -> void:
	var connection := await open_slot(backend(), "slot1")

	var operations: Array[Dictionary] = []
	for at in 2:
		var other := { "kind": 0, "schema_name": "others", "key": "o%d" % at }
		operations.append(
			{ "kind": "replace", "address": other, "envelope": envelope(at) },
		)
	for at in 5:
		operations.append(
			{
				"kind": "replace",
				"address": address(0, "p%d" % at),
				"envelope": envelope(at),
			},
		)
	var wrote := connection._write_batch(operations)
	await wrote.wait()

	var seen := PackedStringArray()
	var cursor := ""
	var pages := 0
	while pages < 10:
		pages += 1
		var scan := connection._scan(
			{
				"schema_name": "players",
				"kind": 0,
				"cursor": cursor,
				"limit": 2,
			},
		)
		await scan.wait()
		var page: Dictionary = scan.get_result()
		assert_int(int(page["error"])).is_equal(OK)
		for row: Dictionary in page["records"]:
			seen.append(String(row["key"]))
		cursor = String(page["cursor"])
		if cursor.is_empty():
			break
	assert_bool(cursor.is_empty()) \
			.override_failure_message("The scan never exhausted.").is_true()
	assert_int(pages) \
			.override_failure_message(
				"A limit of 2 over 5 records took one page.",
			).is_greater(1)
	seen.sort()
	assert_array(Array(seen)).is_equal(["p0", "p1", "p2", "p3", "p4"])


func test_a_slot_is_listed_then_deleted_with_every_schema_under_it() -> void:
	var database := backend()
	var connection := await open_slot(database, "slot1")
	await open_slot(database, "slot2")

	var world := { "kind": 0, "schema_name": "worlds", "key": "overworld" }
	var wrote := connection._write_batch(
		[
			{
				"kind": "replace",
				"address": address(0, "hero"),
				"envelope": envelope(1),
			},
			{ "kind": "replace", "address": world, "envelope": envelope(2) },
			{
				"kind": "replace",
				"address": address(1, "hero"),
				"envelope": envelope(3),
			},
		],
	)
	await wrote.wait()

	var listing := database._list_slots(session)
	await listing.wait()
	var names: PackedStringArray = listing.get_result()
	assert_bool(names.has("slot1")).is_true()
	assert_bool(names.has("slot2")).is_true()

	var removed := database._delete_slot(session, &"slot1")
	await removed.wait()
	assert_int(int(removed.get_result())).is_equal(OK)

	var after := database._list_slots(session)
	await after.wait()
	var left: PackedStringArray = after.get_result()
	assert_bool(left.has("slot1")) \
			.override_failure_message("A deleted slot is still listed.") \
			.is_false()
	assert_bool(left.has("slot2")).is_true()

	var reopened := await open_slot(backend(), "slot1")
	for stored: Dictionary in [address(0, "hero"), world, address(1, "hero")]:
		var read := reopened._read(stored)
		await read.wait()
		assert_bool(bool(read.get_result()["found"])) \
				.override_failure_message(
					"Deleting the slot left %s behind." % [stored],
				).is_false()
