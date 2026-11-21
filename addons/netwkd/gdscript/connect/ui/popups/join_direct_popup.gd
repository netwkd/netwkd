## A form to join a server by its address.
class_name JoinDirectPopup
extends PopupPanel

## Emitted when the player confirms the form.
signal submitted(
		transport: StringName,
		address: String,
		settings: Dictionary,
		username: String,
		join_args: Array,
)

var _transports: Array[Dictionary] = []
var _settings := ConnectFieldList.new()
var _join_args := ConnectFieldList.new()

@onready var _transport_picker: OptionButton = %BackendPicker
@onready var _address_edit: LineEdit = %AddressEdit
@onready var _username_edit: LineEdit = %UsernameEdit
@onready var _settings_container: VBoxContainer = %SettingsContainer
@onready var _args_container: VBoxContainer = %ArgsContainer
@onready var _confirm_button: Button = %ConfirmButton
@onready var _cancel_button: Button = %CancelButton


func _ready() -> void:
	hide()
	_transport_picker.item_selected.connect(_on_transport_selected)
	_confirm_button.pressed.connect(_on_confirm_pressed)
	_cancel_button.pressed.connect(hide)


## Opens the form with the available transports of [param connection],
## choosing [param peer_class] and filling in [param address] when given.
func open(
		connection: NetwConnection,
		username: String,
		peer_class := &"",
		address := "",
) -> void:
	_transports.clear()
	_transport_picker.clear()
	for transport in connection.transports():
		if transport.capabilities & NetwConnection.TRANSPORT_AVAILABLE:
			_transports.append(transport)
			_transport_picker.add_item(transport.display_name)
	var peer_classes := _transports.map(func(entry): return entry.peer_class)
	_transport_picker.selected = maxi(0, peer_classes.find(peer_class))
	_address_edit.text = address
	_username_edit.text = username
	_join_args.render_schema(_args_container, connection.join_schema())
	_on_transport_selected(_transport_picker.selected)
	popup_centered()


func _on_transport_selected(index: int) -> void:
	var transport := _transports[index]
	_address_edit.placeholder_text = transport.address_placeholder
	_address_edit.tooltip_text = transport.address_help
	_settings.render_settings(_settings_container, transport.client_settings)


func _on_confirm_pressed() -> void:
	hide()
	submitted.emit(
		_transports[_transport_picker.selected].peer_class,
		_address_edit.text.strip_edges(),
		_settings.as_dictionary(),
		(
				_username_edit.text if not _username_edit.text.is_empty()
				else _username_edit.placeholder_text
		),
		_join_args.as_array(),
	)
