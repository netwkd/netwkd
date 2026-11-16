@tool
extends Control

const CAUSES: PackedStringArray = [
	"state",
	"input",
	"calls",
	"spawns",
	"upkeep",
	"headers",
]
const DIRECTIONS: PackedStringArray = ["up", "down"]
const CAUSE_COLORS: Array[Color] = [
	Color("0072b2"),
	Color("e69f00"),
	Color("cc79a7"),
	Color("f0e442"),
	Color("56b4e9"),
]
const BAR_ORDER: PackedInt32Array = [5, 4, 0, 1, 2, 3]
const LEGEND_ORDER: PackedInt32Array = [5, 0, 1, 2, 3, 4]
const HEADERS := 5
const PACKETS := 12
const RATE_COUNT := 14
const RECENT_COUNT := 40
const IDLE_SECONDS := 1.0
const GAP := 4.0
const TICK_SPACING := 3.0

var rates: PackedFloat64Array = []
var totals: PackedFloat64Array = [0.0, 0.0]
var pinned_totals: PackedFloat64Array = [0.0, 0.0]
var recent: PackedFloat64Array = []
var peak := 0.0
var is_pinned := false
var hint_blocks: Array[Rect2] = []
var hint_texts: PackedStringArray = []
var idle := Timer.new()


func _init() -> void:
	idle.one_shot = true
	idle.wait_time = IDLE_SECONDS
	idle.timeout.connect(clear)
	add_child(idle)
	rates.resize(RATE_COUNT)
	rates.fill(0.0)


func _ready() -> void:
	var font_size: int = get_theme_default_font_size()
	var line: float = get_theme_default_font().get_height(font_size)
	custom_minimum_size.y = line * 3.0 + GAP * 2.0


func show_rates(data: Array) -> void:
	if data.size() != RATE_COUNT:
		return

	rates = PackedFloat64Array(data)
	totals = [0.0, 0.0]
	for direction: int in DIRECTIONS.size():
		for cause: int in CAUSES.size():
			totals[direction] += rates[direction * CAUSES.size() + cause]

	recent.append(maxf(totals[0], totals[1]))
	if recent.size() > RECENT_COUNT:
		recent.remove_at(0)
	peak = 0.0
	for seen: float in recent:
		peak = maxf(peak, seen)

	idle.start()
	queue_redraw()


func clear() -> void:
	rates.fill(0.0)
	totals = [0.0, 0.0]
	recent = []
	peak = 0.0
	is_pinned = false
	pinned_totals = [0.0, 0.0]
	queue_redraw()


func _gui_input(event: InputEvent) -> void:
	var click: InputEventMouseButton = event as InputEventMouseButton
	if click == null or not click.pressed or totals[0] + totals[1] <= 0.0:
		return
	if click.button_index != MOUSE_BUTTON_LEFT:
		return

	is_pinned = not is_pinned
	pinned_totals = [0.0, 0.0]
	if is_pinned:
		pinned_totals = totals.duplicate()
	queue_redraw()


func _get_tooltip(at_position: Vector2) -> String:
	for at: int in hint_blocks.size():
		if hint_blocks[at].has_point(at_position):
			return hint_texts[at]
	return ""


func _draw() -> void:
	hint_blocks.clear()
	hint_texts.clear()

	var font: Font = get_theme_default_font()
	var font_size: int = get_theme_default_font_size()
	var line: float = font.get_height(font_size)
	var ascent: float = font.get_ascent(font_size)
	var ink: Color = get_theme_color("font_color", "Editor")
	var colors: Array[Color] = CAUSE_COLORS.duplicate()
	colors.append(Color(ink, 0.3))

	var widest := "000.0 KB/s  000 pkt/s"
	var left: float = font.get_string_size("down ", 0, -1, font_size).x + GAP
	var right: float = font.get_string_size(widest, 0, -1, font_size).x + GAP
	var span: float = maxf(size.x - left - right, 0.0)
	var pinned_peak: float = maxf(pinned_totals[0], pinned_totals[1])
	var full: float = maxf(maxf(peak, pinned_peak), 1.0)

	for direction: int in DIRECTIONS.size():
		var top: float = direction * (line + GAP)
		var first: int = direction * CAUSES.size()
		var total: float = maxf(totals[direction], 0.001)
		var x: float = left
		for cause: int in BAR_ORDER:
			var rate: float = rates[first + cause]
			var block := Rect2(x, top, span * rate / full, line)
			draw_rect(block, colors[cause])
			hint_blocks.append(block)
			hint_texts.append(
				"%s %s  %.2f KB/s  %d%%" % [
					DIRECTIONS[direction],
					CAUSES[cause],
					rate / 1000.0,
					roundi(100.0 * rate / total),
				],
			)
			x = block.end.x

		var packets: float = rates[PACKETS + direction]
		var head: float = span * rates[first + HEADERS] / full
		var ticks: int = mini(roundi(packets), int(head / TICK_SPACING))
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
			DIRECTIONS[direction],
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			font_size,
			ink,
		)
		draw_string(
			font,
			Vector2(left + span + GAP, top + ascent),
			"%.1f KB/s  %d pkt/s" % [
				totals[direction] / 1000.0,
				roundi(packets),
			],
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			font_size,
			ink,
		)

	var both: float = totals[0] + totals[1]
	var header_rate: float = rates[HEADERS] + rates[CAUSES.size() + HEADERS]
	var words: PackedStringArray = CAUSES.duplicate()
	words[HEADERS] = "headers %d%%" % roundi(
		100.0 * header_rate / maxf(both, 0.001),
	)
	var legend: float = 2.0 * (line + GAP)
	var swatch: float = line * 0.6
	var x: float = left
	for cause: int in LEGEND_ORDER:
		draw_rect(
			Rect2(x, legend + line * 0.2, swatch, swatch),
			colors[cause],
		)
		x += swatch + GAP
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
		x += GAP * 3.0

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
	if not is_pinned:
		return

	var accent: Color = get_theme_color("accent_color", "Editor")
	for direction: int in DIRECTIONS.size():
		var top: float = direction * (line + GAP)
		var outline := Rect2(
			left,
			top,
			span * pinned_totals[direction] / full,
			line,
		)
		draw_rect(outline, accent, false, 2.0)

	var before: float = pinned_totals[0] + pinned_totals[1]
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
