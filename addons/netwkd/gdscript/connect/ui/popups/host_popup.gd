## A form to host a game through one of the session's transports.
class_name HostPopup
extends PopupPanel

## Emitted when the player confirms the form.
signal submitted(
		transport: StringName,
		settings: Dictionary,
		info: NetwServerInfo,
		username: String,
		join_args: Array,
)

var _transports: Array[Dictionary] = []
var _settings := ConnectFieldList.new()
var _join_args := ConnectFieldList.new()

@onready var _transport_picker: OptionButton = %BackendPicker
@onready var _settings_container: VBoxContainer = %SettingsContainer
@onready var _args_container: VBoxContainer = %ArgsContainer
@onready var _name_edit: LineEdit = %NameEdit
@onready var _username_edit: LineEdit = %UsernameEdit
@onready var _confirm_button: Button = %ConfirmButton
@onready var _cancel_button: Button = %CancelButton


func _ready() -> void:
	hide()
	_transport_picker.item_selected.connect(_on_transport_selected)
	_confirm_button.pressed.connect(_on_confirm_pressed)
	_cancel_button.pressed.connect(hide)


## Opens the form with the transports of [param connection] that can host.
func open(connection: NetwConnection, username: String) -> void:
	var hostable := (
			NetwConnection.TRANSPORT_AVAILABLE
			| NetwConnection.TRANSPORT_CAN_HOST
	)
	_transports.clear()
	_transport_picker.clear()
	for transport in connection.transports():
		if transport.capabilities & hostable == hostable:
			_transports.append(transport)
			_transport_picker.add_item(transport.display_name)
	_name_edit.text = ""
	_username_edit.text = username
	_join_args.render_schema(_args_container, connection.join_schema())
	_confirm_button.disabled = _transports.is_empty()
	if not _transports.is_empty():
		_on_transport_selected(0)
	popup_centered()


func _on_transport_selected(index: int) -> void:
	_settings.render_settings(
		_settings_container,
		_transports[index].host_settings,
	)


func _on_confirm_pressed() -> void:
	var chosen := _settings.as_dictionary()
	var info := NetwServerInfo.new()
	info.motd = (
			_name_edit.text if not _name_edit.text.is_empty()
			else _name_edit.placeholder_text
	)
	info.max_players = chosen.get("max_players", 0)
	hide()
	submitted.emit(
		_transports[_transport_picker.selected].peer_class,
		chosen,
		info,
		(
				_username_edit.text if not _username_edit.text.is_empty()
				else _username_edit.placeholder_text
		),
		_join_args.as_array(),
	)
