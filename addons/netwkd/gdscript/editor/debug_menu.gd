@tool
## Adds the netwkd debug draw toggles to the editor's Debug menu.
extends RefCounted

const _HINTS: Array[Dictionary] = [
	{
		"name": "prediction",
		"label": "Prediction Rings",
		"tooltip": "Draw prediction rings in the next run.",
	},
	{
		"name": "interpolation",
		"label": "Interpolation Timeline",
		"tooltip": "Draw interpolation samples and playheads in the next run.",
	},
]
const _FIRST_ID := 9100
const _SECTION := "netwkd"

var _plugin: EditorPlugin
var _menu: PopupMenu
var _owns_menu := false


func _init(plugin: EditorPlugin) -> void:
	_plugin = plugin


## Adds the toggles to the Debug menu, or to a submenu of Project > Tools
## when the Debug menu is missing.
func attach() -> void:
	# The Debug menu is found by node name because the editor publishes no
	# API for it.
	var found := EditorInterface.get_base_control().find_children(
		"Debug",
		"PopupMenu",
		true,
		false,
	)
	if found.is_empty():
		_menu = PopupMenu.new()
		_owns_menu = true
		_plugin.add_tool_submenu_item(_SECTION, _menu)
	else:
		_menu = found[0]
		_menu.add_separator(_SECTION)
	var enabled := _enabled()
	for at in _HINTS.size():
		var hint := _HINTS[at]
		var id := _FIRST_ID + at
		_menu.add_check_item(hint["label"], id)
		var index := _menu.get_item_index(id)
		_menu.set_item_checked(index, enabled.has(hint["name"]))
		_menu.set_item_tooltip(index, hint["tooltip"])
	_menu.id_pressed.connect(_on_id_pressed)


## Removes the toggles.
func detach() -> void:
	if _menu == null:
		return
	_menu.id_pressed.disconnect(_on_id_pressed)
	if _owns_menu:
		_plugin.remove_tool_menu_item(_SECTION)
	else:
		var first := _menu.get_item_index(_FIRST_ID)
		for _hint in _HINTS:
			_menu.remove_item(first)
		if first > 0 and _menu.is_item_separator(first - 1):
			_menu.remove_item(first - 1)
	_menu = null


## Returns [param args] with a [code]--netw-hints[/code] argument for the
## checked toggles.
func run_args(args: PackedStringArray) -> PackedStringArray:
	var enabled := _enabled()
	if not enabled.is_empty():
		args.append("--netw-hints=%s" % ",".join(enabled))
	return args


func _on_id_pressed(id: int) -> void:
	var at := id - _FIRST_ID
	if at < 0 or at >= _HINTS.size():
		return
	var index := _menu.get_item_index(id)
	var checked := not _menu.is_item_checked(index)
	_menu.set_item_checked(index, checked)
	var enabled := _enabled()
	var hint_name: String = _HINTS[at]["name"]
	if checked and not enabled.has(hint_name):
		enabled.append(hint_name)
	elif not checked and enabled.has(hint_name):
		enabled.remove_at(enabled.find(hint_name))
	_settings().set_project_metadata("netwkd", "hints", ",".join(enabled))


func _enabled() -> PackedStringArray:
	var stored: String = _settings().get_project_metadata(
		"netwkd",
		"hints",
		"",
	)
	return stored.split(",", false)


func _settings() -> EditorSettings:
	return EditorInterface.get_editor_settings()
