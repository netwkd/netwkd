@tool
extends EditorDebuggerPlugin

const TrafficStrip := preload("traffic_strip.gd")

var strips: Dictionary[int, TrafficStrip] = { }
var docked: Array[TrafficStrip] = []


func _has_capture(prefix: String) -> bool:
	return prefix == "netw"


func _capture(message: String, data: Array, session_id: int) -> bool:
	if message != "netw:traffic" or not strips.has(session_id):
		return false

	strips[session_id].show_rates(data)
	return true


func _setup_session(session_id: int) -> void:
	var session: EditorDebuggerSession = get_session(session_id)
	var strip := TrafficStrip.new()
	strip.name = "Traffic"
	strips[session_id] = strip
	session.add_session_tab(strip)
	session.stopped.connect(strip.clear)
	dock.call_deferred(strip)


func dock(strip: TrafficStrip) -> void:
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
	docked.append(strip)


func release() -> void:
	for strip: Variant in docked:
		if is_instance_valid(strip):
			strip.reparent(strip.get_parent().get_parent(), false)
	docked.clear()
	strips.clear()
