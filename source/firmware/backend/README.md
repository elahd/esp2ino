# Backend Firmware Source

1. Main program code lives in the shared folder.
2. esp32 and esp8266 folders hold files specific to those chips. Main program code is included in these directories via symlink.
3. **Do not work out of these folders directly. Use the VS Code workspaces in /workspaces.**