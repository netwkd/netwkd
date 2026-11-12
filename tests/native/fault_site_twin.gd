extends Node


func _init() -> void:
	Netw.configure_spawn(make)


func make() -> Node:
	return null
