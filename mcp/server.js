#!/usr/bin/env node

/**
 * Salivo MCP (Model Context Protocol) Server
 * Clean, zero-dependency JSON-RPC 2.0 stdio server for Claude Code and Claude Desktop.
 * Full Toolchain & Backend Support:
 * - Compiler & Runner (sf run, sf check)
 * - Linter & Formatter (salivolint, salivofmt)
 * - Package Manager (spm build, test, run, new, add, tree)
 * - Backend Probing & HTTP Client (httpRequest)
 * - Backend Scaffolder (scaffoldBackend for Web API, TCP Server, SQLite)
 * - Architecture & Backend Documentation (stdlibInfo, backendDocs)
 */

const readline = require('readline');
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');
const http = require('http');
const https = require('https');

function getBinPath(binName) {
  const exeName = process.platform === 'win32' ? `${binName}.exe` : binName;
  const userSalivoBin = path.join(os.homedir(), '.salivo', 'bin', exeName);
  if (fs.existsSync(userSalivoBin)) {
    return userSalivoBin;
  }
  return binName;
}

const SF_BIN = getBinPath('sf');
const FMT_BIN = getBinPath('salivofmt');
const LINT_BIN = getBinPath('salivolint');
const SPM_BIN = getBinPath('spm');

const STDLIB_DOCS = {
  core: `Module: salivo.std.core
Imports: ><salivo.std.core::{assert, assertEq, assertStrEq, assertNe, assertStrNe, panic, assert, out, outln, Option, Result, some, none, ok, err, Drop};
Core constructs:
- pub trait Drop { func drop(self); }  // Deterministic RAII scope cleanup
- Option<T>: some(v), none()
- Result<T, E>: ok(v), err(e)
- Try operator (?): expr? unwraps Result/Option or returns early
- Assertions: assert(cond), assertEq(a, b), assertStrEq(s1, s2)`,

  net: `Module: salivo.std.net (Core OS Networking)
Imports: ><salivo.std.net::{IPAddress, SocketAddress, ipv4, ipv6, sockaddr, parseip, resolve, TcpStream, TcpListener, UdpSocket, tcpsendstr, tcprecvstr, tcpsendbuf, shutdown, NetworkError, NetworkErrorKind};
Key APIs:
- ipv4(a, b, c, d) -> IPAddress
- parseip(ip_str: string) -> IPAddress
- sockaddr(ip: IPAddress, port: int) -> SocketAddress
- resolve(domain: string, provider: PlatformProvider) -> Result<IPAddress, NetworkError>
- tcpsendstr(stream: TcpStream, data: string) -> Result<int, NetworkError>
- tcprecvstr(stream: TcpStream, max_bytes: int) -> Result<string, NetworkError>
- shutdown(stream: TcpStream) -> Result<bool, NetworkError>
- Low-level sockets: socket(), bind(ip, port), listen(listener), accept(listener), connect(sock, ip, port), sockread(), sockwrite(), close()`,

  io: `Module: salivo.std.io
Imports: ><salivo.std.io::{readStr, writeStr, print, println, flush};
Primary APIs:
- readStr(path: string) -> Result<string, IoError>
- writeStr(path: string, content: string) -> Result<(), IoError>`,

  fs: `Module: salivo.std.fs
Imports: ><salivo.std.fs::{pathJoin, pathExists, isOpen, readByte, dirExists, dirList, open, close, flush, meta, read, write};
Primary APIs:
- pathJoin(a, b) -> string
- pathExists(path) -> bool
- dirExists(path) -> bool
- dirList(path) -> Result<Vec<string>, Error>
- open(path, mode) -> Result<Handle, Error>
- close(handle) -> Result<(), Error>`,

  crypto: `Module: salivo.std.crypto
Imports: ><salivo.std.crypto::{sha256Hex, secSeed, randBytes, sha256, sha512, blake3, hmac, aes128, aes256, entropy};
Primary APIs:
- sha256Hex(data) -> string
- secSeed() -> int
- randBytes(len) -> ByteSlice
- blake3(data) -> ByteSlice
- hmac(key, data) -> ByteSlice`,

  strings: `Module: salivo.std.strings
Imports: ><salivo.std.strings::{stringNew, stringWithCap, asStr, fromStr, fromCString, toCString, cStringView, pushStr, pushByte, startsWith, endsWith, padStart, padEnd, assertStringEq, assertSliceEq};`,

  collections: `Module: salivo.std.collections
Imports: ><salivo.std.collections::{vecNew, vecCap, push, pop, get, set, veclen, mapNew, put, has, lookup, maplen, heapNew, heapPush, heapPop, deqNew, pushBack, popFront, btreeNew, btSetNew};`,

  sync: `Module: salivo.std.sync
Imports: ><salivo.std.sync::{mutex, lock, unlock, rwlock, readlock, unlockread, atomic, load, store, swap, fetchadd, cas, channel, send, recv};`,

  time: `Module: salivo.std.time
Imports: ><salivo.std.time::{epoch, now, fromms, duration, addduration, sleep, format, parse};`,

  math: `Module: salivo.std.math
Imports: ><salivo.std.math::{abs, min, max, clamp, pow, sqrt, seed, rng, range, sin, cos, tan};`,

  mem: `Module: salivo.std.mem
Imports: ><salivo.std.mem::{alloc, dealloc, realloc, alloczero, isallocated, arcnew, readi64, writei64, readbyte, writebyte, slicelen, sliceget, sliceset};`
};

const BACKEND_DOCS = {
  web: `Package: @salivo/web (High-Performance HTTP Web Framework)
Imports: ><salivoweb;
Features:
- Router & Dispatcher:
    let mut router = salivoweb.router();
    router = salivoweb.get(router, "/hello", "Hello World");
    router = salivoweb.getjson(router, "/users/:id", "{\\"id\\": {id}}");
    router = salivoweb.post(router, "/api/create", "{\\"status\\": \\"created\\"}");
    router = salivoweb.put(router, "/api/update/:id", "{\\"status\\": \\"ok\\"}");
    router = salivoweb.delroute(router, "/api/delete/:id", "{\\"status\\": \\"deleted\\"}");
- Request Parsing & Response Generation:
    let req = salivoweb.parsereq(raw_wire_data);
    let res = salivoweb.dispatchdyn(router, req);
    let wire_output = salivoweb.formatresp(res);
- Server Daemon:
    salivoweb.listen(router, 8080);`,

  http: `Package: @salivo/http (HTTP Client & Wire Types)
Imports: ><salivohttp;
Types & Functions:
- pub struct Request { method: string, uri: string, body: string }
- pub struct Response { status_code: int, body: string }
- respok(body: string) -> Response
- get(url: string) -> Result<Response, string>`,

  sqlite: `Package: @salivo/sqlite (Embedded SQL Database)
Imports: ><salivosqlite;
Operations:
- openmem() -> Result<SqliteConnection, string>
- opendb(path: string) -> Result<SqliteConnection, string>
- execute(conn, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);") -> Result<int, string>
- execute(conn, "INSERT INTO users (name) VALUES ('Alice');")
- prepare(conn, "SELECT id, name FROM users;") -> int (stmt_handle)
- step(stmt_handle) -> int (100 = Row, 101 = Done)
- columnint(stmt_handle, 0) -> int
- columntext(stmt_handle, 1) -> string
- finalize(stmt_handle)
- closeconn(conn)`,

  json: `Package: @salivo/json (JSON AST & Parser)
Imports: ><salivojson;
Types:
- pub enum JsonValue { Null, Bool(bool), Number(float), String(string), Array(int), Object(int) }
- parsejson(text: string) -> Result<JsonValue, string>
- stringify(val: JsonValue) -> string`,

  async: `Package: @salivo/async (Concurrency & Event Loop)
Imports: ><salivoasync;
- Event loop, async task scheduling, timer polling, channel integration`
};

const BACKEND_TEMPLATES = {
  'web_api': `module main;

><salivo.std.core::{outln};
><salivoweb;

pub func main() -> int {
    outln("=========================================");
    outln("     Salivo Production HTTP Service      ");
    outln("=========================================");

    let mut router = salivoweb.router();
    router = salivoweb.get(router, "/health", "{\\"status\\":\\"healthy\\",\\"uptime\\":\\"ok\\"}");
    router = salivoweb.getjson(router, "/api/v1/users/:id", "{\\"user_id\\":{id},\\"active\\":true}");
    router = salivoweb.post(router, "/api/v1/users", "{\\"status\\":\\"created\\",\\"id\\":101}");

    outln("Server listening on http://127.0.0.1:8080");
    salivoweb.listen(router, 8080);
    return 0;
}
`,

  'tcp_echo': `module main;

><salivo.std.core::{outln, Result};
><salivo.std.net::{ipv4, sockaddr, parseip, tcpsendstr, tcprecvstr, TcpStream, NetworkError};

pub func main() -> int {
    outln("Salivo TCP Echo Client starting...");
    let host = "127.0.0.1";
    let port = 8080;
    let ip = parseip(host);
    outln(\$"Target address: {host}:{port}");
    return 0;
}
`,

  'sqlite_crud': `module main;

><salivo.std.core::{outln, assert};
><salivosqlite;

pub func main() -> int {
    outln("Initializing in-memory SQLite database...");
    let conn_res = salivosqlite.openmem();
    if conn_res.is_err() {
        outln("Failed to open database");
        return 1;
    }
    let conn = conn_res.unwrap();

    let _ = salivosqlite.execute(conn, "CREATE TABLE items (id INTEGER PRIMARY KEY, title TEXT, price REAL);");
    let _ = salivosqlite.execute(conn, "INSERT INTO items (title, price) VALUES ('Book', 19.99);");
    let _ = salivosqlite.execute(conn, "INSERT INTO items (title, price) VALUES ('Pen', 2.50);");

    let stmt = salivosqlite.prepare(conn, "SELECT id, title, price FROM items;");
    while (salivosqlite.step(stmt) == 100) {
        let id = salivosqlite.columnint(stmt, 0);
        let title = salivosqlite.columntext(stmt, 1);
        let price = salivosqlite.columnfloat(stmt, 2);
        outln(\$"Item #{id}: {title} (\${price})");
    }
    salivosqlite.finalize(stmt);
    salivosqlite.closeconn(conn);
    outln("Database operations completed successfully!");
    return 0;
}
`
};

const TOOLS = [
  {
    name: 'run',
    description: 'Compiles and runs a Salivo (.sal) file or inline code snippet with "sf run".',
    inputSchema: {
      type: 'object',
      properties: {
        file_path: { type: 'string', description: 'Path to .sal file to execute' },
        code: { type: 'string', description: 'Optional inline Salivo code' }
      }
    }
  },
  {
    name: 'check',
    description: 'Analyzes syntax and semantics without code generation using "sf check".',
    inputSchema: {
      type: 'object',
      properties: {
        file_path: { type: 'string', description: 'Path to .sal file to typecheck' },
        code: { type: 'string', description: 'Optional inline Salivo code' }
      }
    }
  },
  {
    name: 'fmt',
    description: 'Formats Salivo source code using salivofmt (either a file in place or an inline code string).',
    inputSchema: {
      type: 'object',
      properties: {
        file_path: { type: 'string', description: 'Path to .sal file to format in-place' },
        code: { type: 'string', description: 'Optional inline code to format and return' }
      }
    }
  },
  {
    name: 'lint',
    description: 'Runs the official Salivo static analysis and linter (salivolint) on a file or snippet.',
    inputSchema: {
      type: 'object',
      properties: {
        file_path: { type: 'string', description: 'Path to .sal file to lint' },
        code: { type: 'string', description: 'Optional inline code to lint' }
      }
    }
  },
  {
    name: 'version',
    description: 'Returns the installed Salivo compiler version and active toolchain paths.',
    inputSchema: {
      type: 'object',
      properties: {}
    }
  },
  {
    name: 'spm',
    description: 'Executes Salivo Package Manager (spm) commands: build, test, run, new, add, tree, check, audit.',
    inputSchema: {
      type: 'object',
      properties: {
        command: {
          type: 'string',
          description: 'SPM command to execute: "test", "build", "run", "check", "tree", "audit", "add", "new"'
        },
        args: {
          type: 'array',
          items: { type: 'string' },
          description: 'Arguments or flags for the command (e.g. ["@salivo/web"] for add, or ["my_app"] for new)'
        },
        cwd: {
          type: 'string',
          description: 'Optional working directory for spm execution'
        }
      },
      required: ['command']
    }
  },
  {
    name: 'httpRequest',
    description: 'Sends an HTTP/HTTPS probe request to test backend endpoints, APIs, and web services.',
    inputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string', description: 'Target URL, e.g. "http://127.0.0.1:8080/hello"' },
        method: { type: 'string', description: 'HTTP method: GET, POST, PUT, DELETE (default: GET)' },
        headers: { type: 'object', description: 'Optional key-value HTTP headers' },
        body: { type: 'string', description: 'Optional request body string or JSON' },
        timeout_ms: { type: 'number', description: 'Request timeout in ms (default: 5000)' }
      },
      required: ['url']
    }
  },
  {
    name: 'scaffoldBackend',
    description: 'Generates ready-to-run backend boilerplates: "web_api" (@salivo/web HTTP server), "tcp_echo" (std.net TCP client/server), "sqlite_crud" (@salivo/sqlite database operations).',
    inputSchema: {
      type: 'object',
      properties: {
        template: {
          type: 'string',
          description: 'Template type: "web_api", "tcp_echo", or "sqlite_crud"'
        },
        output_file: {
          type: 'string',
          description: 'Optional path to write the generated template to. If omitted, returns code in response.'
        }
      },
      required: ['template']
    }
  },
  {
    name: 'backendDocs',
    description: 'Returns official architecture specifications, imports, and APIs for Salivo backend packages: web, http, sqlite, json, async, or net.',
    inputSchema: {
      type: 'object',
      properties: {
        package: {
          type: 'string',
          description: 'Target backend component: web, http, sqlite, json, async, or net'
        }
      },
      required: ['package']
    }
  },
  {
    name: 'stdlibInfo',
    description: 'Returns official standard library module cheat sheets: core, net, io, fs, crypto, strings, collections, sync, time, math, mem, or all.',
    inputSchema: {
      type: 'object',
      properties: {
        module: {
          type: 'string',
          description: 'Module name: core, net, io, fs, crypto, strings, collections, sync, time, math, mem, or all'
        }
      },
      required: ['module']
    }
  }
];

function runCommand(bin, args, input = null, cwd = null) {
  try {
    const res = spawnSync(bin, args, {
      input: input,
      cwd: cwd || process.cwd(),
      encoding: 'utf-8',
      windowsHide: true,
      maxBuffer: 10 * 1024 * 1024
    });
    return {
      status: res.status,
      stdout: res.stdout || '',
      stderr: res.stderr || '',
      error: res.error ? res.error.message : null
    };
  } catch (err) {
    return {
      status: -1,
      stdout: '',
      stderr: '',
      error: err.message
    };
  }
}

function sendHttpRequest(urlStr, method = 'GET', headers = {}, body = null, timeoutMs = 5000) {
  return new Promise((resolve) => {
    try {
      const parsedUrl = new URL(urlStr);
      const isHttps = parsedUrl.protocol === 'https:';
      const client = isHttps ? https : http;

      const reqOptions = {
        method: (method || 'GET').toUpperCase(),
        hostname: parsedUrl.hostname,
        port: parsedUrl.port || (isHttps ? 443 : 80),
        path: parsedUrl.pathname + (parsedUrl.search || ''),
        headers: headers || {},
        timeout: timeoutMs
      };

      const startTime = Date.now();
      const req = client.request(reqOptions, (res) => {
        let responseBody = '';
        res.setEncoding('utf8');
        res.on('data', (chunk) => { responseBody += chunk; });
        res.on('end', () => {
          const duration = Date.now() - startTime;
          const formatted = [
            `HTTP/1.1 ${res.statusCode} ${res.statusMessage}`,
            `Duration: ${duration}ms`,
            `Headers: ${JSON.stringify(res.headers, null, 2)}`,
            '',
            `Body:`,
            responseBody
          ].join('\n');
          resolve({ text: formatted, isError: res.statusCode >= 400 });
        });
      });

      req.on('timeout', () => {
        req.destroy();
        resolve({ text: `Request timed out after ${timeoutMs}ms`, isError: true });
      });

      req.on('error', (err) => {
        resolve({ text: `Connection error: ${err.message}`, isError: true });
      });

      if (body) {
        req.write(typeof body === 'string' ? body : JSON.stringify(body));
      }
      req.end();
    } catch (err) {
      resolve({ text: `Invalid URL or request parameter: ${err.message}`, isError: true });
    }
  });
}

async function handleToolCallAsync(name, args) {
  const normalized = (name || '').replace(/^salivo_/, '').replace(/^sal_/, '').toLowerCase();

  switch (normalized) {
    case 'version': {
      const res = runCommand(SF_BIN, ['version']);
      const output = [
        `Salivo Compiler: ${res.stdout.trim() || 'Unknown version'}`,
        `Binary Path: ${SF_BIN}`,
        `Formatter: ${FMT_BIN}`,
        `Linter: ${LINT_BIN}`,
        `Package Manager: ${SPM_BIN}`
      ].join('\n');
      return { text: output, isError: res.status !== 0 };
    }

    case 'run': {
      let targetFile = args.file_path;
      let tempCreated = false;
      if (args.code) {
        targetFile = path.join(os.tmpdir(), `salivo_tmp_${Date.now()}.sal`);
        fs.writeFileSync(targetFile, args.code, 'utf-8');
        tempCreated = true;
      }
      if (!targetFile || !fs.existsSync(targetFile)) {
        return { text: `Error: File not found: ${targetFile}`, isError: true };
      }
      const res = runCommand(SF_BIN, ['run', targetFile]);
      if (tempCreated) {
        try { fs.unlinkSync(targetFile); } catch (_) {}
      }
      const text = (res.stdout + (res.stderr ? `\n[STDERR]\n${res.stderr}` : '')).trim() || '(No output, exit code: ' + res.status + ')';
      return { text, isError: res.status !== 0 };
    }

    case 'check': {
      let targetFile = args.file_path;
      let tempCreated = false;
      if (args.code) {
        targetFile = path.join(os.tmpdir(), `salivo_chk_${Date.now()}.sal`);
        fs.writeFileSync(targetFile, args.code, 'utf-8');
        tempCreated = true;
      }
      if (!targetFile || !fs.existsSync(targetFile)) {
        return { text: `Error: File not found: ${targetFile}`, isError: true };
      }
      const res = runCommand(SF_BIN, ['check', targetFile]);
      if (tempCreated) {
        try { fs.unlinkSync(targetFile); } catch (_) {}
      }
      const text = res.status === 0
        ? `Check successful: ${targetFile} syntax and types are valid!`
        : (res.stderr || res.stdout || 'Check failed with exit code ' + res.status);
      return { text, isError: res.status !== 0 };
    }

    case 'fmt': {
      if (args.code) {
        const res = runCommand(FMT_BIN, ['--stdin'], args.code);
        return {
          text: res.stdout || res.stderr || args.code,
          isError: res.status !== 0
        };
      }
      if (args.file_path) {
        if (!fs.existsSync(args.file_path)) {
          return { text: `Error: File not found: ${args.file_path}`, isError: true };
        }
        const res = runCommand(FMT_BIN, ['-w', args.file_path]);
        return {
          text: res.status === 0 ? `Formatted ${args.file_path} successfully.` : res.stderr,
          isError: res.status !== 0
        };
      }
      return { text: 'Error: Must provide either "file_path" or "code"', isError: true };
    }

    case 'lint': {
      let targetFile = args.file_path;
      let tempCreated = false;
      if (args.code) {
        targetFile = path.join(os.tmpdir(), `salivo_lint_${Date.now()}.sal`);
        fs.writeFileSync(targetFile, args.code, 'utf-8');
        tempCreated = true;
      }
      if (!targetFile || !fs.existsSync(targetFile)) {
        return { text: `Error: File not found: ${targetFile}`, isError: true };
      }
      const res = runCommand(LINT_BIN, [targetFile]);
      if (tempCreated) {
        try { fs.unlinkSync(targetFile); } catch (_) {}
      }
      const text = res.status === 0
        ? `Linter passed with 0 warnings/errors for ${targetFile}!`
        : (res.stdout || res.stderr || 'Lint reported issues.');
      return { text, isError: res.status !== 0 };
    }

    case 'spm': {
      const spmCmd = args.command;
      const extraArgs = Array.isArray(args.args) ? args.args : [];
      const res = runCommand(SPM_BIN, [spmCmd, ...extraArgs], null, args.cwd);
      const text = (res.stdout + (res.stderr ? `\n[STDERR]\n${res.stderr}` : '')).trim() || `Command completed with exit code ${res.status}`;
      return { text, isError: res.status !== 0 };
    }

    case 'httprequest':
    case 'http_request': {
      return await sendHttpRequest(args.url, args.method, args.headers, args.body, args.timeout_ms);
    }

    case 'scaffoldbackend':
    case 'scaffold_backend': {
      const tmplKey = (args.template || '').toLowerCase().trim();
      const code = BACKEND_TEMPLATES[tmplKey];
      if (!code) {
        const available = Object.keys(BACKEND_TEMPLATES).join(', ');
        return { text: `Unknown template: "${tmplKey}". Available: ${available}`, isError: true };
      }
      if (args.output_file) {
        try {
          fs.writeFileSync(args.output_file, code, 'utf-8');
          return { text: `Successfully generated ${tmplKey} template at ${args.output_file}`, isError: false };
        } catch (err) {
          return { text: `Failed to write file ${args.output_file}: ${err.message}`, isError: true };
        }
      }
      return { text: code, isError: false };
    }

    case 'backenddocs':
    case 'backend_docs': {
      const pkg = (args.package || '').toLowerCase().trim();
      if (BACKEND_DOCS[pkg]) {
        return { text: BACKEND_DOCS[pkg], isError: false };
      }
      if (pkg === 'net' && STDLIB_DOCS.net) {
        return { text: STDLIB_DOCS.net, isError: false };
      }
      const available = [...Object.keys(BACKEND_DOCS), 'net'].join(', ');
      return { text: `Unknown backend component: "${pkg}". Available: ${available}`, isError: true };
    }

    case 'stdlibinfo':
    case 'stdlib_info': {
      const mod = (args.module || '').toLowerCase().trim();
      if (mod === 'all') {
        const full = Object.values(STDLIB_DOCS).join('\n\n---\n\n');
        return { text: full, isError: false };
      }
      if (STDLIB_DOCS[mod]) {
        return { text: STDLIB_DOCS[mod], isError: false };
      }
      const available = Object.keys(STDLIB_DOCS).join(', ');
      return {
        text: `Unknown module: "${mod}". Available modules: ${available}, or "all".`,
        isError: true
      };
    }

    default:
      return { text: `Unknown tool: ${name}`, isError: true };
  }
}

function sendResponse(id, result, error = null) {
  const msg = { jsonrpc: '2.0', id };
  if (error) {
    msg.error = error;
  } else {
    msg.result = result;
  }
  process.stdout.write(JSON.stringify(msg) + '\n');
}

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

rl.on('line', async (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;

  let request;
  try {
    request = JSON.parse(trimmed);
  } catch (err) {
    sendResponse(null, null, { code: -32700, message: 'Parse error' });
    return;
  }

  const { id, method, params } = request;

  switch (method) {
    case 'initialize':
      sendResponse(id, {
        protocolVersion: '2024-11-05',
        capabilities: {
          tools: {}
        },
        serverInfo: {
          name: 'salivo-mcp-server',
          version: '1.1.0'
        }
      });
      break;

    case 'notifications/initialized':
      break;

    case 'ping':
      sendResponse(id, {});
      break;

    case 'tools/list':
      sendResponse(id, { tools: TOOLS });
      break;

    case 'tools/call': {
      const toolName = params ? params.name : null;
      const toolArgs = (params && params.arguments) ? params.arguments : {};
      const result = await handleToolCallAsync(toolName, toolArgs);
      sendResponse(id, {
        content: [
          {
            type: 'text',
            text: result.text
          }
        ],
        isError: result.isError
      });
      break;
    }

    default:
      if (id !== undefined && id !== null) {
        sendResponse(id, null, {
          code: -32601,
          message: `Method not found: ${method}`
        });
      }
      break;
  }
});
