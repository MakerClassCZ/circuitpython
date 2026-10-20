// The editor's side of the CircuitPython host debugger. The debug adapter itself runs inside
// CircuitPython (python/debugadapter.py); this only starts the program in a terminal and points
// VS Code at the adapter's port.
const vscode = require('vscode');
const net = require('net');

// Connect and hang up until the adapter answers; it treats a silent client as a probe.
function waitForPort(port, timeoutMs) {
    return new Promise((resolve, reject) => {
        const started = Date.now();
        const attempt = () => {
            const socket = net.connect(port, '127.0.0.1');
            socket.once('connect', () => { socket.destroy(); resolve(); });
            socket.once('error', () => {
                socket.destroy();
                if (Date.now() - started > timeoutMs) {
                    reject(new Error('CircuitPython did not open port ' + port));
                } else {
                    setTimeout(attempt, 250);
                }
            });
        };
        attempt();
    });
}

// The program started by the last launch. After a reload (a file saved on the drive) it waits
// for the editor again, so the next F5 connects to it instead of starting a second one.
let running = null;

class Factory {
    async createDebugAdapterDescriptor(session) {
        const config = session.configuration;
        const port = config.port || 5678;
        if (config.request === 'launch') {
            if (running === null || running.exitStatus !== undefined) {
                const folders = vscode.workspace.workspaceFolders;
                const drive = config.drive || (folders && folders[0].uri.fsPath) || '.';
                // The program is the terminal's shell, so its console is the terminal and no
                // quoting of paths is needed on any system.
                running = vscode.window.createTerminal({
                    name: 'CircuitPython',
                    shellPath: config.program,
                    shellArgs: ['--drive', drive, '--debug', String(port)].concat(config.args || []),
                });
            }
            running.show(true);
            await waitForPort(port, 30000);
        }
        return new vscode.DebugAdapterServer(port, '127.0.0.1');
    }
}

function activate(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('circuitpython', new Factory()));
    context.subscriptions.push(vscode.window.onDidCloseTerminal((terminal) => {
        if (terminal === running) {
            running = null;
        }
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
