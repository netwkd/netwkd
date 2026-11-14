class_name RocketBall
extends RigidBody3D

const STARTING_POSITION := Vector3(0.0, 2.381, 0.0)
const CONFETTI := preload("res://examples/rocket_league/scenes/confetti.tscn")

var goal_team := -1

@onready var session: NetwSession = Netw.session(self)
@onready var clock: NetwClock = session.clock
@onready var entity := NetwEntity.of(self)
@onready var state := PhysicsServer3D.body_get_direct_state(get_rid())
@onready var level: Node = entity.scene.root
@onready var game: RocketGame = level.get_node(^"game")
@onready var goals := level.get_node(^"field").find_children("*", "Area3D", false)


func _init() -> void:
	Netw.configure_property(self, &"position").state() \
			.on_spawn().teleport_threshold(1.5) \
			.quantize(NetwQuantizeScalar.new().bits(19).limits(-128.0, 128.0)) \
			.interpolate(
				NetwInterpolate.new().lerp().snap_at(3.0) \
						.project_by(&"linear_velocity"),
			)
	Netw.configure_property(self, &"quaternion").state() \
			.on_spawn() \
			.quantize(NetwQuantizeQuaternion.new().bits(16)) \
			.interpolate(NetwInterpolate.new().slerp())
	Netw.configure_property(self, &"linear_velocity").state() \
			.teleport_only() \
			.quantize(NetwQuantizeScalar.new().bits(16).limits(-128.0, 128.0))
	Netw.configure_property(self, &"angular_velocity").state() \
			.teleport_only() \
			.quantize(NetwQuantizeScalar.new().bits(16).limits(-64.0, 64.0))
	Netw.configure_property(self, &"sleeping").state()
	Netw.configure_property(self, &"goal_team").state() \
			.quantize(NetwQuantizeScalar.new().bits(2).limits(-1.0, 1.0))

	Netw.configure_rpc(announce_goal).authority().reliable()
	var e := Netw.configure_entity(self)
	e.name = &"ball"
	e.display.visual_root = ^"MeshInstance3D"


func _ready() -> void:
	state.transform = transform


func _process(_delta: float) -> void:
	visible = clock.tick >= game.kickoff_tick


func _network_process(_delta: float, tick: int, _is_fresh: bool) -> void:
	goal_team = sample_goal()

	if not multiplayer.is_server():
		return

	if tick < game.kickoff_tick:
		reset()
		return

	if goal_team >= 0:
		game.rule_goal(goal_team, tick)
		Netw.rpc(announce_goal, goal_team)


func reset() -> void:
	state.transform = Transform3D(Basis.IDENTITY, STARTING_POSITION)
	state.linear_velocity = Vector3.ZERO
	state.angular_velocity = Vector3.ZERO


func announce_goal(_team: int) -> void:
	var confetti_instance := CONFETTI.instantiate() as Node3D
	get_parent().add_child(confetti_instance)
	confetti_instance.global_position = global_position


func sample_goal() -> int:
	for goal: Area3D in goals:
		if goal.overlaps_body(self):
			return int(String(goal.name).right(1))
	return -1
