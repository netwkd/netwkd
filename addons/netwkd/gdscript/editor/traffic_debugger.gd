@tool
## Shows a traffic strip for each debug session, in the Network Profiler
## when the editor has one.
extends EditorDebuggerPlugin

const _TrafficStrip := preload("traffic_strip.gd")

var _strips: Dictionary[int, _TrafficStrip] = { }
var _docked: Array[_TrafficStrip] = []


func _has_capture(prefix: String) -> bool:
	return prefix == "netw"


func _capture(message: String, data: Array, session_id: int) -> bool:
	if message != "netw:traffic" or not _strips.has(session_id):
		return false

	_strips[session_id].show_rates(data)
	return true


func _setup_session(session_id: int) -> void:
	var session: EditorDebuggerSession = get_session(session_id)
	var strip := _TrafficStrip.new()
	strip.name = "Traffic"
	_strips[session_id] = strip
	session.add_session_tab(strip)
	session.stopped.connect(strip.clear)
	_dock.call_deferred(strip)


func _dock(strip: _TrafficStrip) -> void:
	var tabs: Node = strip.get_parent()
	if tabs == null:
		return

	# The Network Profiler tab is found by class name because the editor
	# publishes no API for it. Without it the strip stays a tab of its own.
	var profilers: Array[Node] = tabs.find_children(
		"*",
		"EditorNetworkProfiler",
		false,
		false,
	)
	if profilers.is_empty():
		return

	strip.reparent(profilers[0], false)
	profilers[0].move_child(strip, 1)
	strip.show()
	_docked.append(strip)


## Moves the docked strips back out and forgets every strip.
func release() -> void:
	for strip: Variant in _docked:
		if is_instance_valid(strip):
			strip.reparent(strip.get_parent().get_parent(), false)
	_docked.clear()
	_strips.clear()
