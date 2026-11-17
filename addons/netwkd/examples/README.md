# Examples

Each folder is a small multiplayer game. Open its `main.tscn` and press F6.
For more than one player, enable multiple instances in Debug > Customize Run
Instances, press Host in one window and Join in the others.

The examples add nothing to your project. They declare no `class_name`, and
each one adds its input actions from code when it starts. This keeps them
out of your global classes and your Input Map. It is not how a game should
be organized. Your own game should use `class_name` and Project Settings >
Input Map as usual.

An export includes this folder. Delete it, or add
`addons/netwkd/examples/*` to the export preset's exclude filter.
