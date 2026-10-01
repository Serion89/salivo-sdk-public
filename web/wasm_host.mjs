// Stage 37.1 host for Salivo wasm32 modules (docs/STAGE_37_1_WASM.md).
// Usage: node scripts/wasm_host.mjs module.wasm            run _start, stdout + exit status
//        node scripts/wasm_host.mjs --inspect module.wasm  print validation, imports, exports as JSON
import { readFileSync, writeSync } from "node:fs";

const inspect = process.argv[2] === "--inspect";
const bytes = readFileSync(process.argv[inspect ? 3 : 2]);
if (!WebAssembly.validate(bytes)) {
  console.error("invalid WebAssembly module");
  process.exit(2);
}
const mod = new WebAssembly.Module(bytes);
if (inspect) {
  const mem = WebAssembly.Module.exports(mod).find((e) => e.kind === "memory");
  console.log(JSON.stringify({ valid: true, imports: WebAssembly.Module.imports(mod), exports: WebAssembly.Module.exports(mod), memory: !!mem }));
  process.exit(0);
}
class Exit { constructor(code) { this.code = code; } }
let memory;
const imports = {
  salivo: {
    write(fd, ptr, len) {
      if (fd !== 1 && fd !== 2) throw new Error(`bad fd ${fd}`);
      writeSync(fd, new Uint8Array(memory.buffer, ptr, len));
    },
    exit(code) { throw new Exit(code); },
  },
};
const inst = new WebAssembly.Instance(mod, imports);
memory = inst.exports.memory;
try {
  inst.exports._start();
  process.exitCode = 0;
} catch (e) {
  if (e instanceof Exit) process.exitCode = e.code;
  else { console.error(`wasm trap: ${e.message}`); process.exitCode = 134; }
}
