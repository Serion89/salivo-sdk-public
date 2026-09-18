const fs = require('fs');
const path = require('path');
const os = require('os');

// 1. Update ~/.claude.json
const homeDir = os.homedir();
const claudeJsonPath = path.join(homeDir, '.claude.json');
const serverPath = path.join(homeDir, '.salivo', 'mcp', 'server.js');

if (fs.existsSync(claudeJsonPath)) {
  const backupPath = path.join(homeDir, `.claude.json.bak.${Date.now()}`);
  fs.copyFileSync(claudeJsonPath, backupPath);
  console.log(`Backed up ~/.claude.json to ${backupPath}`);

  const raw = fs.readFileSync(claudeJsonPath, 'utf-8');
  const data = JSON.parse(raw);

  if (!data.mcpServers) {
    data.mcpServers = {};
  }

  data.mcpServers.salivo = {
    type: 'stdio',
    command: 'node',
    args: [serverPath]
  };

  fs.writeFileSync(claudeJsonPath, JSON.stringify(data, null, 2), 'utf-8');
  console.log('Successfully added "salivo" to ~/.claude.json mcpServers!');
} else {
  console.warn(`File not found: ${claudeJsonPath}`);
}

// 2. Update or create Claude Desktop config (%APPDATA%\Claude\claude_desktop_config.json)
const appData = process.env.APPDATA;
if (appData) {
  const claudeDesktopDir = path.join(appData, 'Claude');
  if (!fs.existsSync(claudeDesktopDir)) {
    fs.mkdirSync(claudeDesktopDir, { recursive: true });
  }

  const desktopConfigPath = path.join(claudeDesktopDir, 'claude_desktop_config.json');
  let desktopData = { mcpServers: {} };
  if (fs.existsSync(desktopConfigPath)) {
    try {
      desktopData = JSON.parse(fs.readFileSync(desktopConfigPath, 'utf-8'));
    } catch (e) {
      desktopData = { mcpServers: {} };
    }
  }

  if (!desktopData.mcpServers) {
    desktopData.mcpServers = {};
  }

  desktopData.mcpServers.salivo = {
    command: 'node',
    args: [serverPath]
  };

  fs.writeFileSync(desktopConfigPath, JSON.stringify(desktopData, null, 2), 'utf-8');
  console.log(`Successfully configured Salivo MCP in Claude Desktop at ${desktopConfigPath}!`);
}
