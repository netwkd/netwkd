@tool
class_name TestConnectFieldList
extends GdUnitTestSuite

const ERROR_SCRUBBER := preload(
	"res://tests/support/gdunit_error_scrubber.gd"
)


func test_settings_render_one_input_per_editable_value() -> void:
	var container := VBoxContainer.new()
	add_child(container)
	var fields := ConnectFieldList.new()

	fields.render_settings(
		container,
		{
			port = 21253,
			relay = true,
			region = "eu",
			hook = Callable(),
			handle = RID(),
			peer = null,
		},
	)

	assert_int(container.get_child_count()).is_equal(3)
	assert_dict(fields.as_dictionary()).is_equal(
		{ port = 21253, relay = true, region = "eu" },
	)
	container.free()


func test_a_string_list_has_one_editable_row_per_entry() -> void:
	var authored := PackedStringArray(
		["wss://one.example", "wss://two.example"],
	)
	var control := ConnectFieldList.make_value_control(authored)
	add_child(control)
	await get_tree().process_frame

	assert_that(
		ConnectFieldList.value_from_control(control, TYPE_PACKED_STRING_ARRAY),
	).is_equal(authored)

	(control.get_node("Entry0/Edit") as LineEdit).text = "wss://edited.example"
	(control.get_node("AddEntryButton") as Button).pressed.emit()
	(control.get_node("Entry2/Edit") as LineEdit).text = "wss://three.example"
	(control.get_node("Entry1").get_child(1) as Button).pressed.emit()
	await get_tree().process_frame

	assert_that(
		ConnectFieldList.value_from_control(control, TYPE_PACKED_STRING_ARRAY),
	).is_equal(
		PackedStringArray(["wss://edited.example", "wss://three.example"]),
	)
	control.queue_free()


func test_a_structured_value_round_trips_as_json() -> void:
	var authored: Array = [{ "urls": ["stun:stun.example:19302"] }]
	var control := ConnectFieldList.make_value_control(authored)

	assert_that(
		ConnectFieldList.value_from_control(control, TYPE_ARRAY),
	).is_equal(authored)

	(control as LineEdit).text = "not json"

	assert_that(
		ConnectFieldList.value_from_control(control, TYPE_ARRAY),
	).is_null()
	assert_int(
		await ERROR_SCRUBBER.erase_matching(["'not json' is not a JSON array"]),
	).is_equal(1)
	control.free()


func test_ice_servers_have_one_editable_card_per_server() -> void:
	var control := ConnectFieldList.make_value_control(
		[
			{ "urls": ["stun:stun.example:19302"] },
			{
				"urls": ["turn:turn.example:3478"],
				"username": "guest",
				"credential": "secret",
			},
		],
		&"ice_servers",
	)
	add_child(control)
	await get_tree().process_frame

	(control.get_node("Server1/Fields/URLsEdit") as LineEdit).text = (
			"turn:one.example:3478, turns:two.example:5349"
	)
	var username: LineEdit = control.get_node("Server1/Fields/UsernameEdit")
	username.text = "player"
	(control.get_node("AddServerButton") as Button).pressed.emit()

	assert_that(
		ConnectFieldList.value_from_control(control, TYPE_ARRAY),
	).is_equal(
		[
			{ "urls": ["stun:stun.example:19302"] },
			{
				"urls": ["turn:one.example:3478", "turns:two.example:5349"],
				"username": "player",
				"credential": "secret",
			},
			{ "urls": [] },
		],
	)
	control.queue_free()
