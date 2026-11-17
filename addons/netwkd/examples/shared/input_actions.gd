extends RefCounted

static func add(
		action: StringName,
		events: Array[InputEvent],
		deadzone := 0.5,
) -> void:
	if InputMap.has_action(action):
		return
	InputMap.add_action(action, deadzone)
	for event in events:
		InputMap.action_add_event(action, event)


static func add_driving() -> void:
	add(
		&"forward",
		[
			key(KEY_W),
			key(KEY_UP),
			joy_axis(JOY_AXIS_TRIGGER_RIGHT, 1.0),
		],
	)
	add(
		&"back",
		[
			key(KEY_S),
			key(KEY_DOWN),
			joy_axis(JOY_AXIS_TRIGGER_LEFT, 1.0),
		],
	)
	add(&"left", [key(KEY_A), key(KEY_LEFT), joy_axis(JOY_AXIS_LEFT_X, -1.0)])
	add(&"right", [key(KEY_D), key(KEY_RIGHT), joy_axis(JOY_AXIS_LEFT_X, 1.0)])


static func key(keycode: Key) -> InputEventKey:
	var event := InputEventKey.new()
	event.physical_keycode = keycode
	return event


static func mouse(button: MouseButton) -> InputEventMouseButton:
	var event := InputEventMouseButton.new()
	event.button_index = button
	return event


static func joy_axis(axis: JoyAxis, value: float) -> InputEventJoypadMotion:
	var event := InputEventJoypadMotion.new()
	event.axis = axis
	event.axis_value = value
	return event


static func joy_button(button: JoyButton) -> InputEventJoypadButton:
	var event := InputEventJoypadButton.new()
	event.button_index = button
	return event
