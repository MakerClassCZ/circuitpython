# CircuitPython host debugger for VS Code

Debug `code.py` running in the Linux or Windows build of CircuitPython: breakpoints, stepping,
the call stack, local and global variables, and expressions, with the display in its SDL window.

The debug adapter runs inside CircuitPython (`debugadapter.py` beside the program, started by
`circuitpython --debug PORT`); this extension starts the program in a terminal and connects
VS Code to it.

## Install

Copy this folder to `~/.vscode/extensions/circuitpython-host-debug` (on Windows
`%USERPROFILE%\.vscode\extensions\`) and restart VS Code, or install the `.vsix`.

## Use

Open the CIRCUITPY folder in VS Code and add to `.vscode/launch.json`:

```json
{
  "type": "circuitpython",
  "request": "launch",
  "name": "CircuitPython: run code.py",
  "program": "/path/to/circuitpython",
  "drive": "${workspaceFolder}",
  "port": 5678
}
```

Set breakpoints in `code.py` or in `lib/*.py` and press F5. The program starts in a terminal,
waits for the editor and then runs `code.py` under the debugger. Its console is the terminal.

To attach to a program you started yourself with `circuitpython --debug 5678`, use a
configuration with `"request": "attach"`.

Python files compiled from source show their local variables; `.mpy` files keep no variable
names, so for them only globals are visible.
