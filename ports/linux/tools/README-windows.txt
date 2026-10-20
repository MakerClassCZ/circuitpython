CircuitPython for Windows (experimental build, 2026-10-20, branch linux-port)

Run it from a terminal (Windows Terminal, cmd or PowerShell):

    circuitpython.exe                      CIRCUITPY is the folder .\CIRCUITPY, created on first start
    circuitpython.exe --drive demo\picostellar
    circuitpython.exe --drive C:\path\to\folder

The terminal is the serial console: Ctrl-C interrupts, Ctrl-D reloads, any key opens the REPL,
Ctrl-\ or closing the window quits. Edit the files in the folder with any editor; code.py runs
again after each save. A window opens for the display; keys: arrows, Z X A S (A B X Y),
Enter (START), Backspace (SELECT), Q W (L R), Esc (MENU); F12 saves a screenshot. Game
controllers work through XInput.

DEBUGGING IN VS CODE
1. Install the extension: in VS Code, Extensions > "..." > Install from VSIX >
   circuitpython-host-debug-0.1.0.vsix.
2. Open the folder demo\hello (File > Open Folder). Its .vscode\launch.json points at
   circuitpython.exe two levels up; for your own project copy that file and set "program".
3. Set a breakpoint in code.py or lib\helper.py and press F5. The program starts in a terminal
   inside VS Code, waits for the editor, then runs code.py under the debugger: stepping, the
   call stack, Locals and Globals, and the Watch and Debug Console evaluate expressions.
   After you save a file the program reloads and waits again; press F5 again.
   debugadapter.py must stay beside circuitpython.exe.

Not on Windows yet: audiomp3, os.statvfs(). Sound plays through the default audio device.

A crash prints "hard fault, exception ... at 0x...": send that line back with what you ran.
