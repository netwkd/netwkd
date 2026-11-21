## The progress of a connection attempt, with a button to cancel it.
class_name ConnectingPopup
extends PopupPanel

## Emitted when the player presses the cancel button.
signal cancelled

@onready var _title_label: Label = %TitleLabel
@onready var _progress_bar: ProgressBar = %ProgressBar
@onready var _cancel_button: Button = %CancelButton


func _ready() -> void:
	hide()
	_cancel_button.pressed.connect(hide)
	_cancel_button.pressed.connect(cancelled.emit)


## Opens the popup for a new attempt.
func open() -> void:
	_title_label.text = "Connecting..."
	_progress_bar.hide()
	_cancel_button.text = "Cancel"
	popup_centered()


## Shows [param message] and [param ratio]. A negative [param ratio] hides
## the progress bar.
func progressed(message: String, ratio: float) -> void:
	if not message.is_empty():
		_title_label.text = message
	_progress_bar.visible = ratio >= 0.0
	_progress_bar.value = ratio


## Shows [param reason] with a button to close the popup.
func failed(reason: String) -> void:
	_title_label.text = reason
	_progress_bar.hide()
	_cancel_button.text = "Close"
	popup_centered()
