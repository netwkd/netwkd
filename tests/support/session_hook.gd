## GdUnit4 session hook that owns netwkd debug state during tests.
##
## Debugger scopes are closed after each test case so failures and early
## returns cannot leak state into the next test. Also baselines the SceneTree
## root child count to surface isolation leaks.
class_name NetwTestSessionHook
extends GdUnitTestSessionHook

static var _active_hook: NetwTestSessionHook

const _NATIVE_TESTS := &"NetwNativeTests"
const _TURN_URL_SETTING := "netwkd/webrtc/turn_credentials_url"

var _baseline_child_count: int = 0
var _baseline_time_scale: float = 1.0
var _baseline_physics_ticks: int = 60
var _saved_turn_url: Variant


func _init() -> void:
	super("NetwTestHook", "Resets shared netwkd state between tests.")


func startup(session: GdUnitTestSession) -> GdUnitResult:
	assert(
		Engine.has_meta("GdUnitRunner"),
		"NetwTestHook: GdUnit4 environment not detected! " +
		"Check markers (Engine meta or cmdline args).",
	)
	_active_hook = self
	_saved_turn_url = ProjectSettings.get_setting(_TURN_URL_SETTING, null)
	session.test_event.connect(_on_test_event)
	_baseline_time_scale = Engine.time_scale
	_baseline_physics_ticks = Engine.get_physics_ticks_per_second()

	if not ClassDB.class_exists(_NATIVE_TESTS):
		return GdUnitResult.error(
			"The netwkd library was built without its test classes, so "
			+ "state leaks between tests. Rebuild it with "
			+ "scons -C extension netw_tests=yes.",
		)
	ProjectSettings.set_setting(_TURN_URL_SETTING, "")
	return GdUnitResult.success()


func shutdown(_session: GdUnitTestSession) -> GdUnitResult:
	_reset_global_test_state()
	ProjectSettings.set_setting(_TURN_URL_SETTING, _saved_turn_url)
	if _active_hook == self:
		_active_hook = null
	return GdUnitResult.success()


func _on_test_event(event: GdUnitEvent) -> void:
	if event.type() == GdUnitEvent.TESTSUITE_BEFORE:
		# Ensure InputMap is fully loaded from project settings.
		# This is essential on fresh CI environments without prior editor import.
		InputMap.load_from_project_settings()
		_baseline_child_count = Engine.get_main_loop().root.get_child_count()

	elif event.type() == GdUnitEvent.TESTCASE_BEFORE:
		_reset_global_test_state()

	elif event.type() == GdUnitEvent.TESTCASE_AFTER:
		_assert_clean_state(event)
		_reset_global_test_state()


func _reset_global_test_state() -> void:
	# Schema declarations are process-wide static data that outlives the session
	# that adopted them, so a suite declaring one would otherwise hand every
	# later suite a table it never asked for. A table's wire id is its
	# name-sorted position among the bound tables, so one leaked declaration
	# renumbers another suite's frames and the failure names the wrong cause.
	_call_native_tests(&"schema_model_clear")

	if Engine.time_scale != _baseline_time_scale:
		Engine.time_scale = _baseline_time_scale
	if Engine.get_physics_ticks_per_second() != _baseline_physics_ticks:
		Engine.set_physics_ticks_per_second(_baseline_physics_ticks)

	# Sole owner of shared-session cleanup: this runs before and after every
	# test, so each case starts from a null shared regardless of how the prior
	# one left it.
	if LocalLoopbackSession.has_shared_session():
		LocalLoopbackSession.get_shared_session().reset()
		LocalLoopbackSession.set_shared_session(null)

	_call_native_tests(&"tracker_book_clear")
	_call_native_tests(&"fault_book_clear")


func _call_native_tests(method: StringName) -> void:
	if ClassDB.class_exists(_NATIVE_TESTS):
		ClassDB.class_call_static(_NATIVE_TESTS, method)


func _assert_clean_state(event: GdUnitEvent) -> void:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree:
		return

	var root := tree.root
	var current_count: int = root.get_child_count()

	if current_count > _baseline_child_count:
		var leaks: Array[String] = []
		for i in range(_baseline_child_count, current_count):
			var child := root.get_child(i)
			leaks.append("%s:<%s>" % [child.name, child.get_class()])

		push_error(
			(
					"Test %s left %d nodes under the root (%s). Free them before "
					+ "the test ends."
			) % [
				event.test_name(),
				current_count - _baseline_child_count,
				", ".join(leaks),
			],
		)

	# The shared loopback session is not asserted here. Unlike orphaned root
	# children, a lingering shared pointer never crosses into the
	# next test because _reset_global_test_state clears it before every case.
	# A harness releases it symmetrically in its own teardown.
