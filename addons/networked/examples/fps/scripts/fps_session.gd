extends Node

const Actions := preload("../../shared/input_actions.gd")
const LEVEL := preload("../scenes/main.tscn")

@onready var browser: ConnectBrowser = %ConnectBrowser
@onready var session: NetwSession = Netw.session(self)

var level: Node


func _init() -> void:
	Netw.configure_spawn(spawn_level)
	Netw.configure_session(self).app(&"netw-example-fps")
	Netw.configure_clock(self).ticks_per_second(60)
	Netw.configure_join(enter_level)

	Actions.add(&"move_left", [Actions.key(KEY_A)])
	Actions.add(&"move_right", [Actions.key(KEY_D)])
	Actions.add(
		&"move_forward",
		[Actions.key(KEY_W), Actions.joy_axis(JOY_AXIS_LEFT_Y, -1.0)],
		0.25,
	)
	Actions.add(
		&"move_back",
		[Actions.key(KEY_S), Actions.joy_axis(JOY_AXIS_LEFT_Y, 1.0)],
		0.25,
	)
	Actions.add(
		&"jump",
		[Actions.key(KEY_SPACE), Actions.joy_button(JOY_BUTTON_A)],
	)
	Actions.add(&"camera_left", [Actions.joy_axis(JOY_AXIS_RIGHT_X, -1.0)])
	Actions.add(&"camera_right", [Actions.joy_axis(JOY_AXIS_RIGHT_X, 1.0)])
	Actions.add(&"camera_up", [Actions.joy_axis(JOY_AXIS_RIGHT_Y, -1.0)])
	Actions.add(&"camera_down", [Actions.joy_axis(JOY_AXIS_RIGHT_Y, 1.0)])
	Actions.add(&"mouse_capture", [Actions.mouse(MOUSE_BUTTON_LEFT)])
	Actions.add(&"mouse_capture_exit", [Actions.key(KEY_ESCAPE)])
	Actions.add(
		&"shoot",
		[
			Actions.mouse(MOUSE_BUTTON_LEFT),
			Actions.joy_axis(JOY_AXIS_TRIGGER_RIGHT, 1.0),
		],
	)
	Actions.add(
		&"weapon_toggle",
		[
			Actions.key(KEY_E),
			Actions.joy_button(JOY_BUTTON_RIGHT_SHOULDER),
			Actions.mouse(MOUSE_BUTTON_MIDDLE),
		],
	)


func _ready() -> void:
	session.exited.connect(on_exited)


func spawn_level() -> Node:
	return LEVEL.instantiate()


func enter_level(player: NetwPlayer) -> void:
	if not is_instance_valid(level):
		level = Netw.spawn(spawn_level)
		add_child(level)
	var players: Node = level.get_node(^"Players")
	players.add_child(
		Netw.spawn_player(player, level.spawn_player, players.get_child_count()),
	)


func on_exited(_reason: NetwSession.ExitReason, _detail: String) -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	browser.visible = true
