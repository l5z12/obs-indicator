---
default: minor
---

#### Install and update with one line of PowerShell

`irm https://raw.githubusercontent.com/l5z12/obs-indicator/main/install.ps1 | iex` downloads the latest release,
checks its SHA-256, waits for you to close OBS if it has the plugin loaded, and installs it. Run it again to update.
