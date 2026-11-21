## The address another player types to join this session, with a button to
## copy it.
##
## The bar shows while [member NetwConnection.join_address] is not empty. A
## game that does not want it hides or removes the node.
extends Control

@onready var _connection: NetwConnection = Netw.session(self).connection
@onready var _panel: Control = %Panel
@onready var _code_edit: LineEdit = %CodeEdit


func _ready() -> void:
	_connection.join_address_changed.connect(_on_join_address_changed)
	%CopyButton.pressed.connect(_on_copy_pressed)
	_on_join_address_changed(_connection.join_address)


func _on_join_address_changed(address: String) -> void:
	_code_edit.text = address
	_panel.visible = not address.is_empty()


func _on_copy_pressed() -> void:
	DisplayServer.clipboard_set(_code_edit.text)
