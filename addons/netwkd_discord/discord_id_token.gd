## Proves a player's Discord account to the server.
##
## [codeblock]
## func _init() -> void:
##     Netw.configure_admission(admit).accept(DiscordIdToken.new(DISCORD_APP))
##
## func admit(request: NetwJoinRequest) -> void:
##     if request.account:
##         request.admit()
##     else:
##         request.deny()
## [/codeblock]
## Discord's own ID token carries no name, so
## [member NetwJoinRequest.account_name] is empty. A token from
## [method signed_by] carries the player's Discord name.
class_name DiscordIdToken
extends NetwIdToken


func _init(application_id: int) -> void:
	issuer = "https://discord.com"
	keys_url = "https://discord.com/api/oauth2/keys"
	audience = str(application_id)
	prefix = "discord"


## Returns a preset for the tokens a Discord activity's token Worker signs,
## checked with the Worker's public key at [param public_key_path].
## [codeblock]
## func _init() -> void:
##     Netw.configure_admission(admit).accept(
##         DiscordIdToken.signed_by(DISCORD_APP, "res://activity_token.pub"),
##     )
##
## func admit(request: NetwJoinRequest) -> void:
##     if request.account.begins_with("discord:"):
##         request.admit()
##     else:
##         request.deny()
## [/codeblock]
static func signed_by(
		application_id: int,
		public_key_path: String,
) -> DiscordIdToken:
	var token := DiscordIdToken.new(application_id)
	token.issuer = "https://%d.discordsays.com" % application_id
	token.keys_url = ""
	token.key = load(public_key_path)
	return token
