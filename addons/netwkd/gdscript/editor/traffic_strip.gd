@tool
## Draws the bandwidth of a running game as one bar per direction.
extends Control

const _CAUSES: PackedStringArray = [
	"state",
	"input",
	"calls",
	"spawns",
	"upkeep",
	"headers",
]
const _DIRECTIONS: PackedStringArray = ["up", "down"]
const _CAUSE_COLORS: Array[Color] = [
	Color("0072b2"),
	Color("e69f00"),
	Color("cc79a7"),
	Color("f0e442"),
	Color("56b4e9"),
]
const _BAR_ORDER: PackedInt32Array = [5, 4, 0, 1, 2, 3]
const _LEGEND_ORDER: PackedInt32Array = [5, 0, 1, 2, 3, 4]
const _HEADERS := 5
const _PACKETS := 12
const _RATE_COUNT := 14
const _RECENT_COUNT := 40
const _IDLE_SECONDS := 1.0
const _GAP := 4.0
const _TICK_SPACING := 3.0

var _rates: PackedFloat64Array = []
var _totals: PackedFloat64Array = [0.0, 0.0]
var _pinned_totals: PackedFloat64Array = [0.0, 0.0]
var _recent: PackedFloat64Array = []
var _peak := 0.0
var _is_pinned := false
var _hint_blocks: Array[Rect2] = []
var _hint_texts: PackedStringArray = []
var _idle := Timer.new()


func _init() -> void:
	_idle.one_shot = true
	_idle.wait_time = _IDLE_SECONDS
	_idle.timeout.connect(clear)
	add_child(_idle)
	_rates.resize(_RATE_COUNT)
	_rates.fill(0.0)


func _ready() -> void:
	var font_size: int = get_theme_default_font_size()
	var line: float = get_theme_default_font().get_height(font_size)
	custom_minimum_size.y = line * 3.0 + _GAP * 2.0


## Draws [param data], the bytes per second of each kind of traffic up then
## down, followed by the packets per second up and down.
func show_rates(data: Array) -> void:
	if data.size() != _RATE_COUNT:
		return

	_rates = PackedFloat64Array(data)
	_totals = [0.0, 0.0]
	for direction: int in _DIRECTIONS.size():
		for cause: int in _CAUSES.size():
			_totals[direction] += _rates[direction * _CAUSES.size() + cause]

	_recent.append(maxf(_totals[0], _totals[1]))
	if _recent.size() > _RECENT_COUNT:
		_recent.remove_at(0)
	_peak = 0.0
	for seen: float in _recent:
		_peak = maxf(_peak, seen)

	_idle.start()
	queue_redraw()


## Resets the strip to zero and unpins it.
func clear() -> void:
	_rates.fill(0.0)
	_totals = [0.0, 0.0]
	_recent = []
	_peak = 0.0
	_is_pinned = false
	_pinned_totals = [0.0, 0.0]
	queue_redraw()


func _gui_input(event: InputEvent) -> void:
	var click: InputEventMouseButton = event as InputEventMouseButton
	if click == null or not click.pressed or _totals[0] + _totals[1] <= 0.0:
		return
	if click.button_index != MOUSE_BUTTON_LEFT:
		return

	_is_pinned = not _is_pinned
	_pinned_totals = [0.0, 0.0]
	if _is_pinned:
		_pinned_totals = _totals.duplicate()
	queue_redraw()


func _get_tooltip(at_position: Vector2) -> String:
	for at: int in _hint_blocks.size():
		if _hint_blocks[at].has_point(at_position):
			return _hint_texts[at]
	return ""


func _draw() -> void:
	_hint_blocks.clear()
	_hint_texts.clear()

	var font: Font = get_theme_default_font()
	var font_size: int = get_theme_default_font_size()
	var line: float = font.get_height(font_size)
	var ascent: float = font.get_ascent(font_size)
	var ink: Color = get_theme_color("font_color", "Editor")
	var colors: Array[Color] = _CAUSE_COLORS.duplicate()
	colors.append(Color(ink, 0.3))

	var widest := "000.0 KB/s  000 pkt/s"
	var left: float = font.get_string_size("down ", 0, -1, font_size).x + _GAP
	var right: float = font.get_string_size(widest, 0, -1, font_size).x + _GAP
	var span: float = maxf(size.x - left - right, 0.0)
	var pinned_peak: float = maxf(_pinned_totals[0], _pinned_totals[1])
	var full: float = maxf(maxf(_peak, pinned_peak), 1.0)

	for direction: int in _DIRECTIONS.size():
		var top: float = direction * (line + _GAP)
		var first: int = direction * _CAUSES.size()
		var total: float = maxf(_totals[direction], 0.001)
		var x: float = left
		for cause: int in _BAR_ORDER:
			var rate: float = _rates[first + cause]
			var block := Rect2(x, top, span * rate / full, line)
			draw_rect(block, colors[cause])
			_hint_blocks.append(block)
			_hint_texts.append(
				"%s %s  %.2f KB/s  %d%%" % [
					_DIRECTIONS[direction],
					_CAUSES[cause],
					rate / 1000.0,
					roundi(100.0 * rate / total),
				],
			)
			x = block.end.x

		var packets: float = _rates[_PACKETS + direction]
		var head: float = span * _rates[first + _HEADERS] / full
		var ticks: int = mini(roundi(packets), int(head / _TICK_SPACING))
		for tick: int in ticks:
			var at: float = left + head * (tick + 0.5) / ticks
			draw_line(
				Vector2(at, top),
				Vector2(at, top + line),
				Color(ink, 0.6),
			)

		draw_string(
			font,
			Vector2(0.0, top + ascent),
			_DIRECTIONS[direction],
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			font_size,
			ink,
		)
		draw_string(
			font,
			Vector2(left + span + _GAP, top + ascent),
			"%.1f KB/s  %d pkt/s" % [
				_totals[direction] / 1000.0,
				roundi(packets),
			],
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			font_size,
			ink,
		)

	var both: float = _totals[0] + _totals[1]
	var header_rate: float = (
			_rates[_HEADERS] + _rates[_CAUSES.size() + _HEADERS]
	)
	var words: PackedStringArray = _CAUSES.duplicate()
	words[_HEADERS] = "headers %d%%" % roundi(
		100.0 * header_rate / maxf(both, 0.001),
	)
	var legend: float = 2.0 * (line + _GAP)
	var swatch: float = line * 0.6
	var x: float = left
	for cause: int in _LEGEND_ORDER:
		draw_rect(
			Rect2(x, legend + line * 0.2, swatch, swatch),
			colors[cause],
		)
		x += swatch + _GAP
		draw_string(
			font,
			Vector2(x, legend + ascent),
			words[cause],
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			font_size,
			ink,
		)
		x += font.get_string_size(words[cause], 0, -1, font_size).x
		x += _GAP * 3.0

	if both <= 0.0:
		draw_string(
			font,
			Vector2(0.0, legend + ascent),
			"press Start",
			HORIZONTAL_ALIGNMENT_RIGHT,
			size.x,
			font_size,
			Color(ink, 0.6),
		)
	if not _is_pinned:
		return

	var accent: Color = get_theme_color("accent_color", "Editor")
	for direction: int in _DIRECTIONS.size():
		var top: float = direction * (line + _GAP)
		var outline := Rect2(
			left,
			top,
			span * _pinned_totals[direction] / full,
			line,
		)
		draw_rect(outline, accent, false, 2.0)

	var before: float = _pinned_totals[0] + _pinned_totals[1]
	draw_string(
		font,
		Vector2(0.0, legend + ascent),
		"pinned %.1f KB/s -> now %.1f KB/s  %+d%%" % [
			before / 1000.0,
			both / 1000.0,
			roundi(100.0 * (both - before) / maxf(before, 0.001)),
		],
		HORIZONTAL_ALIGNMENT_RIGHT,
		size.x,
		font_size,
		ink,
	)
