## A server browser that lists, joins and hosts games for its session.
##
## A game that checks an invitation before the player leaves its session
## turns off [member join_invites] and calls [method join] itself.
## [codeblock]
## @onready var session: NetwSession = Netw.session(self)
## @onready var browser: ConnectBrowser = %ConnectBrowser
## @onready var leave_dialog: ConfirmationDialog = %LeaveDialog
## var invitation: NetwEndpoint
##
## func _ready() -> void:
##     browser.join_invites = false
##     session.connection.invited.connect(_on_invited)
##     leave_dialog.confirmed.connect(_on_leave_dialog_confirmed)
##
## func _on_invited(endpoint: NetwEndpoint) -> void:
##     invitation = endpoint
##     if session.is_online:
##         leave_dialog.popup_centered()
##     else:
##         browser.join(endpoint, { }, [])
##
## func _on_leave_dialog_confirmed() -> void:
##     await session.leave().wait()
##     browser.join(invitation, { }, [])
## [/codeblock]
class_name ConnectBrowser
extends Control

const _ROW := preload("res://addons/netwkd/gdscript/connect/ui/row.tscn")

## Answers each [signal NetwConnection.invited] by joining that server, or by
## opening the join form when the session is online or the join needs inputs.
@export var join_invites := true

## Hides the server list while the session is online.
@export var hide_when_session_active := true

## Settings every connection through a transport starts from.
@export var transport_defaults: Array[ConnectTransportConfig] = []

## On the web, keeps the hosted room after [code]#[/code] in the page URL.
@export var use_url_fragment := true

## The player name sent with the next join or host.
var username := "Player"

var _rows: Dictionary[NetwEndpoint, ConnectBrowserRow] = { }
var _selected: NetwEndpoint

@onready var _session: NetwSession = Netw.session(self)
@onready var _connection: NetwConnection = _session.connection
@onready var _server_list: VBoxContainer = $VBox
@onready var _list_box: VBoxContainer = %ListBox
@onready var _empty_state: Control = %EmptyState
@onready var _banner: Control = %Banner
@onready var _banner_label: Label = %BannerLabel
@onready var _details: Control = %Details
@onready var _details_status_dot: StatusDot = %DetailsStatusDot
@onready var _details_name_label: Label = %DetailsNameLabel
@onready var _details_source_label: Label = %DetailsSourceLabel
@onready var _details_address_label: Label = %DetailsAddressLabel
@onready var _details_status_label: Label = %DetailsStatusLabel
@onready var _details_ping_label: Label = %DetailsPingLabel
@onready var _details_players_label: Label = %DetailsPlayersLabel
@onready var _join_button: Button = %DetailsJoinButton
@onready var _host_popup: HostPopup = %HostPopup
@onready var _join_popup: JoinPopup = %JoinPopup
@onready var _join_direct_popup: JoinDirectPopup = %JoinDirectPopup
@onready var _connecting_popup: ConnectingPopup = %ConnectingPopup


func _enter_tree() -> void:
	get_viewport().gui_embed_subwindows = true


func _ready() -> void:
	for config in transport_defaults:
		_connection.set_defaults(config.peer_class(), config.settings())
	_connection.endpoint_added.connect(_on_endpoint_added)
	_connection.endpoint_removed.connect(_on_endpoint_removed)
	_session.state_changed.connect(_on_state_changed)
	_connection.progressed.connect(_on_progressed)
	_connection.failed.connect(_on_failed, CONNECT_DEFERRED)
	_session.join_failed.connect(_on_failed, CONNECT_DEFERRED)
	_connection.join_address_changed.connect(_on_join_address_changed)
	_connection.invited.connect(_on_invited)
	_server_list.visibility_changed.connect(
		_on_server_list_visibility_changed,
	)
	for endpoint in _connection.endpoints:
		_on_endpoint_added(endpoint)
	_connection.browsing = _server_list.is_visible_in_tree()
	_on_state_changed(_session.state)
	_on_endpoint_changed()
	_banner.hide()

	%HostButton.pressed.connect(_on_host_pressed)
	%JoinDirectButton.pressed.connect(_on_join_direct_pressed)
	_join_button.pressed.connect(_on_join_pressed)
	_host_popup.submitted.connect(_on_host_popup_submitted)
	_join_popup.submitted.connect(_on_join_popup_submitted)
	_join_direct_popup.submitted.connect(_on_join_direct_popup_submitted)
	_connecting_popup.cancelled.connect(_connection.cancel)

	var room := (
			str(JavaScriptBridge.eval("location.hash")).trim_prefix("#")
			if use_url_fragment and OS.has_feature("web") else ""
	)
	if not room.is_empty():
		_join_direct_popup.open(
			_connection,
			username,
			&"WebRTCMultiplayerPeer",
			room,
		)


func _exit_tree() -> void:
	_connection.browsing = false


func _on_endpoint_added(endpoint: NetwEndpoint) -> void:
	if _rows.has(endpoint):
		return

	var row: ConnectBrowserRow = _ROW.instantiate()
	row.endpoint = endpoint
	_rows[endpoint] = row
	_list_box.add_child(row)
	row.selected.connect(_on_row_selected.bind(endpoint))
	row.activated.connect(_on_row_activated.bind(endpoint))
	endpoint.changed.connect(_on_endpoint_changed)
	_empty_state.hide()


func _on_endpoint_removed(endpoint: NetwEndpoint) -> void:
	if not _rows.has(endpoint):
		return

	_rows[endpoint].queue_free()
	_rows.erase(endpoint)
	_empty_state.visible = _rows.is_empty()
	if endpoint == _selected:
		_on_row_selected(null)


func _on_state_changed(state: NetwSession.State) -> void:
	match state:
		NetwSession.STATE_OFFLINE:
			_connecting_popup.hide()
			_server_list.show()
		NetwSession.STATE_CREATING, NetwSession.STATE_CONNECTING:
			_banner.hide()
			_connecting_popup.open()
		NetwSession.STATE_ONLINE, NetwSession.STATE_DISCONNECTING:
			_connecting_popup.hide()
			_server_list.visible = not hide_when_session_active


func _on_progressed(message: String, ratio: float) -> void:
	_connecting_popup.progressed(message, ratio)


func _on_failed(_error: Error, reason: String) -> void:
	_banner_label.text = reason
	_banner.show()
	_connecting_popup.failed(reason)


func _on_join_address_changed(address: String) -> void:
	if use_url_fragment and OS.has_feature("web"):
		JavaScriptBridge.get_interface("history").replaceState(
			null,
			"",
			"#" + address,
		)


func _on_server_list_visibility_changed() -> void:
	_connection.browsing = _server_list.is_visible_in_tree()


func _on_invited(endpoint: NetwEndpoint) -> void:
	if not join_invites:
		return
	_on_row_selected(endpoint)
	if _session.is_online or not _connection.join_schema().is_empty():
		_on_join_pressed()
		return
	join(endpoint, { }, [])


## Joins [param endpoint] as [member username]. [param join_args] follow the
## username in [method Netw.prepare_join].
func join(
		endpoint: NetwEndpoint,
		settings: Dictionary,
		join_args: Array,
) -> void:
	Netw.prepare_join.callv([self, username] + join_args)
	_connection.join(endpoint, settings)


## Hosts over [param transport] as [member username], advertising
## [param info]. [param join_args] follow the username in
## [method Netw.prepare_join].
func host(
		transport: StringName,
		settings: Dictionary,
		info: NetwServerInfo,
		join_args: Array,
) -> void:
	_session.set_server_info(info)
	Netw.prepare_join.callv([self, username] + join_args)
	_connection.host(transport, settings)


func _on_row_selected(endpoint: NetwEndpoint) -> void:
	_selected = endpoint
	_on_endpoint_changed()


func _on_row_activated(endpoint: NetwEndpoint) -> void:
	_on_row_selected(endpoint)
	_on_join_pressed()


func _on_endpoint_changed() -> void:
	_details.visible = _selected != null
	if _selected == null:
		return
	var row := _rows[_selected]
	_details_status_dot.status = _selected.status
	_details_name_label.text = row._name_label.text
	_details_source_label.text = row._badge_label.text
	_details_address_label.text = _selected.address
	_details_status_label.text = _details_status_dot.tooltip_text
	_details_ping_label.text = row._ping_label.text
	_details_players_label.text = row._players_label.text
	_join_button.disabled = _selected.status in [
		NetwEndpoint.STATUS_FULL,
		NetwEndpoint.STATUS_INCOMPATIBLE,
		NetwEndpoint.STATUS_UNSUPPORTED,
	]


func _on_join_pressed() -> void:
	_join_popup.open(_connection, _selected, username)


func _on_join_direct_pressed() -> void:
	_join_direct_popup.open(_connection, username)


func _on_host_pressed() -> void:
	_host_popup.open(_connection, username)


func _on_join_popup_submitted(
		settings: Dictionary,
		player_name: String,
		join_args: Array,
) -> void:
	var endpoint := _selected
	if _session.is_online:
		await _session.leave().wait()

	username = player_name
	join(endpoint, settings, join_args)


func _on_join_direct_popup_submitted(
		transport: StringName,
		address: String,
		settings: Dictionary,
		player_name: String,
		join_args: Array,
) -> void:
	username = player_name
	join(_connection.add(transport, address), settings, join_args)


func _on_host_popup_submitted(
		transport: StringName,
		settings: Dictionary,
		info: NetwServerInfo,
		player_name: String,
		join_args: Array,
) -> void:
	username = player_name
	host(transport, settings, info, join_args)
