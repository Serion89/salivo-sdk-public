# Salivo Portable SDK & Language Pack (Universal AI-Ready)

This package contains everything needed to compile, run, develop, and teach ANY AI assistant (Antigravity, Cursor, Claude Code, GitHub Copilot, Windsurf, Cline, ChatGPT, etc.) to write 100% accurate Salivo programs!

====================================================================
WHAT DOES RUNNING "install.bat" DO FOR YOU?
====================================================================
Double-clicking `install.bat` performs a 100% AUTOMATED installation.
You do NOT need to manually configure any paths, settings, or tools!

Here is exactly what running `install.bat` does:

1. AUTOMATICALLY SETS UP THE COMPILER & TOOLCHAIN:
   - Copies the complete Salivo compiler & JIT runner (`sf.exe`), package
     manager (`spm.exe`), code formatter (`salivofmt.exe`), linter (`salivolint.exe`),
     and language server (`salivo-lsp.exe`) to:
     %USERPROFILE%\.salivo\bin\

2. AUTOMATICALLY CONFIGURES WINDOWS ENVIRONMENT (PATH):
   - Permanently appends `%USERPROFILE%\.salivo\bin` to your Windows User PATH.
   - This means you can immediately open Command Prompt, PowerShell,
     Windows Terminal, or VS Code terminal ANYWHERE on your computer and run
     `sf` commands directly without setting environment variables manually!

3. AUTOMATICALLY DEPLOYS THE STANDARD LIBRARY:
   - Copies all standard library modules (`core.sal`, `fs.sal`, `collections.sal`,
     `crypto.sal`, `io.sal`, `mem.sal`, `math.sal`, etc.) to:
     %USERPROFILE%\.salivo\lib\salivo\std\

4. AUTOMATICALLY INSTALLS THE VS CODE EXTENSION:
   - Installs the official Salivo VS Code extension directly into:
     %USERPROFILE%\.vscode\extensions\salivo.salivo-1.0.7\
   - When you open VS Code, you instantly get:
     * Vivid syntax highlighting for `.sal` files
     * Language snippets for structs, loops, functions, and imports
     * A 1-click ▶️ "Run Salivo" button in the top right editor toolbar

5. AUTOMATICALLY TRAINS YOUR AI ASSISTANTS:
   - Injects the Salivo Language Master Laws & Quick Reference directly into
     Antigravity AI (`%USERPROFILE%\.gemini\config\rules\salivo.md` and Knowledge Base).
   - Your AI will immediately understand Salivo syntax without any manual prompt engineering!

====================================================================
QUICK START (3 STEPS)
====================================================================
1. Double-click `install.bat`.
2. Restart your terminal or VS Code (so Windows loads the updated PATH).
3. Test your installation by running:
      sf version
      sf run your_code.sal
   Or open any `.sal` file in VS Code and click the ▶️ Run button!

====================================================================
HOW ANY AI ASSISTANT LEARNS SALIVO FROM THIS PACKAGE
====================================================================
This package is pre-configured with native rule files for every major AI tool:

1. Antigravity AI:
   - `install.bat` automatically configures Antigravity globally!
   - You can also drop `AGENTS.md` into any project folder.

2. Cursor IDE:
   - The included `.cursorrules` file automatically instructs Cursor's models
     (Claude 3.7 Sonnet, GPT-4o) on all Salivo syntax laws and standard library APIs.

3. Claude Code CLI (Anthropic):
   - The included `CLAUDE.md` file instructs Claude Code on Salivo build commands,
     syntax guidelines, and API conventions.

4. GitHub Copilot (VS Code):
   - The included `.github/copilot-instructions.md` file gives Copilot full knowledge
     of Salivo streams (`><`), RAII Drop traits, error propagation (`?`), and types.

5. Windsurf / Codeium:
   - The included `.windsurfrules` file teaches Windsurf Cascade on Salivo coding rules.

6. Cline / Roo Code / Aider / Devin:
   - All read the included `AGENTS.md` automatically at project root.

7. ChatGPT / Claude Web / Gemini Web / DeepSeek:
   - Simply upload or drag-and-drop `docs/salivo_quick_reference.md` or
     `docs/SALIVO_GUIDE.pdf` into your conversation, and the AI will instantly
     write, debug, and explain Salivo code flawlessly!

====================================================================
PACKAGE CONTENTS
====================================================================
- `bin/`: Compiled Salivo compiler (`sf.exe`), package manager (`spm.exe`), and tools.
- `std/`: Official Salivo Standard Library source files.
- `vscode-extension/`: Official VS Code extension (syntax highlighting + run button).
- `docs/`: Complete language documentation (`SALIVO_GUIDE.pdf`, `SALIVO_GUIDE.html`,
           and `salivo_quick_reference.md`).
- `assets/`: Official Salivo brand logos (salivo_logo.png and vector logo.svg).
- `salivo_logo.png`: High-resolution official brand emblem.
- `AGENTS.md`, `CLAUDE.md`, `.cursorrules`, `.windsurfrules`, `.github/`:
  Pre-configured AI rulebooks for every AI assistant.
- `install.bat`: 1-click automated compiler, environment, extension, and AI installer.
