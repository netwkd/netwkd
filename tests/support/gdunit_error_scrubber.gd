## GdUnit4-adapted eraser for errors a test knowingly tolerates.
extends RefCounted

## Erases every recorded error or warning whose message contains all of
## [param markers], and returns how many it erased.
##
## Every marker must match, so a caller narrows by adding one rather than by
## writing a pattern. A case that provokes a fault on purpose asserts the count,
## so the fault is checked rather than hidden.
## [codeblock]
## assert_int(await erase_matching(["[NW0027]"])).is_equal(1)
## [/codeblock]
static func erase_matching(markers: Array) -> int:
	var monitor := (
			GdUnitThreadManager.get_current_context().get_execution_context().error_monitor
	)
	var entries: Array[ErrorLogEntry] = await monitor.scan(true)
	var erased := 0
	for entry: ErrorLogEntry in entries.duplicate():
		var message: String = entry._message
		var matched := true
		for marker: String in markers:
			if marker not in message:
				matched = false
				break
		if matched:
			monitor.erase_log_entry(entry)
			erased += 1
	return erased
