extends CharacterBody2D

var exploding := false


@rpc("call_local")
func exploded(by_who: int) -> void:
	if exploding:
		return
	exploding = true
	$"../../Score".increase_score(Netw.session(self).player_of(by_who))
	$"AnimationPlayer".play(&"explode")
