# Salivo Assistant Command

Adhere strictly to the **Salivo AI Agent Master Guidelines & Architecture Laws** (`AGENTS.md`):

1. **Imports:** Standardized exclusively on stream headers `><` (e.g., `><salivo.std.core::{...};`). Legacy `import` is strictly prohibited.
2. **Types:** Zero-underscore standard. Structs, enums, traits, and type annotations must be clean PascalCase (`Point`, `Option`, `Result`). Never use `_` as a datatype.
3. **Identifiers:** Functions, variables, and struct fields can freely use `camelCase` or `snake_case`.
4. **No Internal Prefix Leakage:** Never expose `salivo_`, `sal_`, `crt_`, or `__` in user-facing APIs or code.
5. **Error Handling:** Use postfix `?` for early propagation on `Result` and `Option`.
6. **Drop:** Use `pub trait Drop { func drop(self); }` for deterministic RAII cleanup.
7. **Toolchain Operations:**
   - Execute: `sf run <file.sal>`
   - Typecheck without codegen: `sf check <file.sal>`
   - Format in-place: `salivofmt -w <file.sal>`
   - Lint: `salivolint <file.sal>`
   - Package manager: `spm build`, `spm test`, `spm run`
