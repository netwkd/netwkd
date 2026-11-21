## A form to join a listed server.
class_name JoinPopup
extends PopupPanel

## Emitted when the player confirms the form.
signal submitted(settings: Dictionary, username: String, join_args: Array)

var _settings := ConnectFieldList.new()
var _join_args := ConnectFieldList.new()

@onready var _username_edit: LineEdit = %UsernameEdit
@onready var _settings_container: VBoxContainer = %SettingsContainer
@onready var _args_container: VBoxContainer = %ArgsContainer
@onready var _confirm_button: Button = %ConfirmButton
@onready var _cancel_button: Button = %CancelButton
@onready var _leave_warning: Label = %LeaveWarning
@onready var _session: NetwSession = Netw.session(self)


func _ready() -> void:
	hide()
	_confirm_button.pressed.connect(_on_confirm_pressed)
	_cancel_button.pressed.connect(hide)


## Opens the form for joining [param endpoint].
func open(
		connection: NetwConnection,
		endpoint: NetwEndpoint,
		username: String,
) -> void:
	_leave_warning.visible = _session.is_online
	_username_edit.text = username
	_settings.render_settings(
		_settings_container,
		connection.transport(endpoint.peer_class).client_settings,
	)
	_join_args.render_schema(_args_container, connection.join_schema())
	popup_centered()


func _on_confirm_pressed() -> void:
	hide()
	submitted.emit(
		_settings.as_dictionary(),
		(
				_username_edit.text if not _username_edit.text.is_empty()
				else _username_edit.placeholder_text
		),
		_join_args.as_array(),
	)
