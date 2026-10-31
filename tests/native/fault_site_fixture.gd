extends Node

var speed := 0.0


func _init() -> void:
	Netw.configure_property(self, &"speed")
	Netw.configure_rpc(hit)
	Netw.configure_clock(self)
	Netw.configure_spawn(make)


func hit() -> void:
	pass


func make() -> Node:
	return null


func call_back(work: Callable) -> void:
	work.call()
