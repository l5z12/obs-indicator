---
default: minor
---

#### Show the overlay over fullscreen games (advanced)

A new off-by-default setting raises the overlay into a higher window-manager z-order band (via the
bundled WindowTopMost) so it can appear over games that out-rank a normal always-on-top window. A
**Layer** picker chooses between above everything, including Task Manager (the default), and above
the game but below Task Manager. It injects a helper into Explorer.exe, so it is opt-in, and it has no effect on legacy
exclusive fullscreen, which bypasses the desktop compositor.
