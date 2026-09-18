const vscode = require('vscode');
const path = require('path');
const fs = require('fs');

let terminal = null;

function getTerminal() {
    if (!terminal || terminal.exitStatus !== undefined) {
        const home = process.env.USERPROFILE || process.env.HOME || '';
        const salivoBinDir = path.join(home, '.salivo', 'bin');
        const currentPath = process.env.PATH || '';
        terminal = vscode.window.createTerminal({
            name: "Salivo",
            env: {
                PATH: `${salivoBinDir};${currentPath}`
            }
        });
    }
    return terminal;
}

function getActiveSalivoDoc() {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('No active Salivo file open.');
        return null;
    }
    const document = editor.document;
    if (document.languageId !== 'salivo' && !document.fileName.endsWith('.sal') && !document.fileName.endsWith('.sf')) {
        vscode.window.showWarningMessage('The active file is not a Salivo source file (.sal / .sf).');
        return null;
    }
    return document;
}

const cp = require('child_process');

function getSalivoBin(binName) {
    const config = vscode.workspace.getConfiguration('salivo');
    let custom = '';
    if (binName === 'sf') custom = config.get('executablePath', '');
    else if (binName === 'salivofmt') custom = config.get('formatterPath', '');
    else if (binName === 'salivolint') custom = config.get('linterPath', '');
    else if (binName === 'spm') custom = config.get('packageManagerPath', '');

    if (custom && fs.existsSync(custom)) {
        return `"${custom}"`;
    }

    const ext = process.platform === 'win32' ? '.exe' : '';
    const home = process.env.USERPROFILE || process.env.HOME || '';
    const localBin = path.join(home, '.salivo', 'bin', binName + ext);
    if (fs.existsSync(localBin)) {
        return `"${localBin}"`;
    }
    return binName;
}

function getSalivoBinPath(binName) {
    const config = vscode.workspace.getConfiguration('salivo');
    let custom = '';
    if (binName === 'salivofmt') custom = config.get('formatterPath', '');
    if (custom && fs.existsSync(custom)) {
        return custom;
    }
    const ext = process.platform === 'win32' ? '.exe' : '';
    const home = process.env.USERPROFILE || process.env.HOME || '';
    const localBin = path.join(home, '.salivo', 'bin', binName + ext);
    if (fs.existsSync(localBin)) {
        return localBin;
    }
    return binName;
}

function activate(context) {
    // 1. Run Salivo File Command (sf run <file>)
    const runFileCmd = vscode.commands.registerCommand('salivo.runFile', () => {
        const document = getActiveSalivoDoc();
        if (!document) return;

        document.save().then(() => {
            const filePath = document.fileName;
            const isNew = (!terminal || terminal.exitStatus !== undefined);
            const term = getTerminal();
            term.show(false);
            const delay = isNew ? 500 : 50;
            setTimeout(() => {
                term.sendText(`${getSalivoBin('sf')} run "${filePath}"`);
            }, delay);
        });
    });

    // 2. Build Salivo File Command (sf build <file>)
    const buildFileCmd = vscode.commands.registerCommand('salivo.buildFile', () => {
        const document = getActiveSalivoDoc();
        if (!document) return;

        document.save().then(() => {
            const filePath = document.fileName;
            const isNew = (!terminal || terminal.exitStatus !== undefined);
            const term = getTerminal();
            term.show(false);
            const delay = isNew ? 500 : 50;
            setTimeout(() => {
                term.sendText(`${getSalivoBin('sf')} build "${filePath}"`);
            }, delay);
        });
    });

    // 3. Check Salivo File Command (sf check <file>)
    const checkFileCmd = vscode.commands.registerCommand('salivo.checkFile', () => {
        const document = getActiveSalivoDoc();
        if (!document) return;

        document.save().then(() => {
            const filePath = document.fileName;
            const isNew = (!terminal || terminal.exitStatus !== undefined);
            const term = getTerminal();
            term.show(false);
            const delay = isNew ? 500 : 50;
            setTimeout(() => {
                term.sendText(`${getSalivoBin('sf')} check "${filePath}"`);
            }, delay);
        });
    });

    // 4. Format Salivo File Command (salivofmt -w <file>)
    const formatFileCmd = vscode.commands.registerCommand('salivo.formatFile', () => {
        const document = getActiveSalivoDoc();
        if (!document) return;

        document.save().then(() => {
            const filePath = document.fileName;
            const term = getTerminal();
            term.show();
            term.sendText(`${getSalivoBin('salivofmt')} -w "${filePath}"`);
        });
    });

    // 5. Lint Salivo File Command (salivolint <file>)
    const lintFileCmd = vscode.commands.registerCommand('salivo.lintFile', () => {
        const document = getActiveSalivoDoc();
        if (!document) return;

        document.save().then(() => {
            const filePath = document.fileName;
            const term = getTerminal();
            term.show();
            term.sendText(`${getSalivoBin('salivolint')} "${filePath}"`);
        });
    });

    // 6. Test Salivo Package Command (spm test)
    const testFileCmd = vscode.commands.registerCommand('salivo.testFile', () => {
        const editor = vscode.window.activeTextEditor;
        let cwd = "";
        if (editor) {
            cwd = path.dirname(editor.document.fileName);
        } else if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
            cwd = vscode.workspace.workspaceFolders[0].uri.fsPath;
        }

        const term = getTerminal();
        term.show();
        const spmBin = getSalivoBin('spm');
        if (cwd) {
            term.sendText(`cd "${cwd}"; ${spmBin} test`);
        } else {
            term.sendText(`${spmBin} test`);
        }
    });

    // 7. Native Document Formatting Provider (Shift+Alt+F & Format on Save)
    const formattingProvider = vscode.languages.registerDocumentFormattingEditProvider('salivo', {
        provideDocumentFormattingEdits(document) {
            return new Promise((resolve) => {
                const text = document.getText();
                const binToRun = getSalivoBinPath('salivofmt');

                const proc = cp.spawn(binToRun, ['--stdin'], { windowsHide: true });
                let stdout = '';
                let stderr = '';
                proc.stdout.on('data', (chunk) => { stdout += chunk; });
                proc.stderr.on('data', (chunk) => { stderr += chunk; });
                proc.on('close', (code) => {
                    if (code === 0 && stdout.length > 0 && stdout !== text) {
                        const fullRange = new vscode.Range(
                            document.positionAt(0),
                            document.positionAt(text.length)
                        );
                        resolve([vscode.TextEdit.replace(fullRange, stdout)]);
                    } else {
                        resolve([]);
                    }
                });
                proc.on('error', () => {
                    resolve([]);
                });
                proc.stdin.write(text);
                proc.stdin.end();
            });
        }
    });

    context.subscriptions.push(
        runFileCmd,
        buildFileCmd,
        checkFileCmd,
        formatFileCmd,
        lintFileCmd,
        testFileCmd,
        formattingProvider
    );
}

function deactivate() {
    if (terminal) {
        terminal.dispose();
    }
}

module.exports = {
    activate,
    deactivate
};

