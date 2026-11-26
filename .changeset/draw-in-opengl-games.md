---
default: minor
---

#### Draw the overlay inside fullscreen OpenGL games such as Minecraft

In fullscreen, OpenGL games like Minecraft send their picture straight to the screen, so no window can show over them,
even with *Show the overlay over fullscreen games*. The new *Draw inside OpenGL games* option under it loads a small
helper into such a game and draws the indicator and notifications into the game's own picture. It's off by default
because it injects into the game, so don't use it with games that have anti-cheat. OBS's Game Capture still leaves
the overlay out of recordings; Display Capture and Window Capture include it. 64-bit games only. Minecraft's Vulkan
renderer doesn't need it: there, *Show the overlay over fullscreen games* is enough.
