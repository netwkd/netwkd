## One [NetwEndpoint] in a [ConnectBrowser] list.
class_name ConnectBrowserRow
extends PanelContainer

## Emitted when the row is clicked.
signal selected
## Emitted when the row is double clicked.
signal activated

## The endpoint this row shows.
var endpoint: NetwEndpoint

@onready var _button: Button = %ConnectButton
@onready var _status_dot: StatusDot = %StatusDot
@onready var _name_label: Label = %NameLabel
@onready var _badge_label: Label = %BadgeLabel
@onready var _address_label: Label = %AddressLabel
@onready var _players_label: Label = %PlayersLabel
@onready var _ping_label: Label = %PingLabel


func _ready() -> void:
	endpoint.changed.connect(_on_changed)
	_button.pressed.connect(selected.emit)
	_button.gui_input.connect(_on_button_gui_input)
	_on_changed()


func _on_changed() -> void:
	var info := endpoint.info
	_status_dot.status = endpoint.status
	_name_label.text = (
			endpoint.display_name if not endpoint.display_name.is_empty()
			else endpoint.address
	)
	_badge_label.text = (
			endpoint.source if not endpoint.source.is_empty()
			else String(endpoint.peer_class).trim_suffix("MultiplayerPeer")
	)
	_address_label.text = endpoint.address
	_players_label.text = (
			"%d/%d" % [info.players, info.max_players]
			if info.max_players > 0 else "-"
	)
	_ping_label.text = (
			"%d ms" % info.latency_ms if info.latency_ms >= 0 else "-"
	)


func _on_button_gui_input(event: InputEvent) -> void:
	var click := event as InputEventMouseButton
	if click != null and click.double_click:
		activated.emit()
