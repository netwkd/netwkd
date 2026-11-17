extends Node

const Actions := preload("../../shared/input_actions.gd")


func _init() -> void:
	Netw.configure_session(self).app(&"7x283tsfmy1xpr4")
	Netw.configure_clock(self).ticks_per_second(15).sends_per_second(5)

	Actions.add(&"move_up", [Actions.key(KEY_W)])
	Actions.add(&"move_down", [Actions.key(KEY_S)])
	Actions.add(&"move_left", [Actions.key(KEY_A)])
	Actions.add(&"move_right", [Actions.key(KEY_D)])
	Actions.add(&"set_bomb", [Actions.mouse(MOUSE_BUTTON_LEFT)])
