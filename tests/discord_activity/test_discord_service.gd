class_name NetwTestDiscordService
extends DiscordActivityService

var fake_instance_id: String:
	set(value):
		fake_instance_id = value
		instance_id = value

var fake_worker: Callable


func _grant(nonce: String) -> NetwPromise:
	return NetwPromise.resolved({ "id_token": fake_worker.call(nonce) })
