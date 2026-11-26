# Reverse-engineering setup

The project uses **REA (Reverse Engineer Anything)** with **Ghidra** as its native-code back-end. REA drives Ghidra headless and returns structured, citable evidence: function lists, pseudocode, cross-references and version comparisons. It also works as an MCP server, so AI coding agents can query the binaries directly.

## Requirements

| Tool | Version | Notes |
|---|---|---|
| Node.js | 22.19+ (22.x), 24.11+ (24.x), or 26+ | Runs REA (`npx rea-agents`) |
| Ghidra | **12.2 or newer** | From the official NationalSecurityAgency/ghidra GitHub releases |
| JDK | **25 or newer**, 64-bit | REA's Ghidra provider rejects older JDKs |

## Setup

1. Install Node.js and a 64-bit JDK 25.
2. Download Ghidra 12.2+ from GitHub releases and check the SHA-256 against the release page. For 12.2, `ghidra_12.2_PUBLIC_20261016.zip` has SHA-256 `2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67`.
3. Extract it and point REA at it:
   ```sh
   export GHIDRA_INSTALL_DIR=/path/to/ghidra_12.2_PUBLIC
   export JAVA_HOME=/path/to/jdk-25
   ```
   On Windows, set these as user environment variables.
4. Check that REA can find Ghidra:
   ```sh
   npx rea-agents@latest doctor --json
   ```
   Look for the `ghidra` provider with `available: true`.
5. Optional: register REA with your coding agent (Claude Code, Codex, etc.):
   ```sh
   npx rea-agents@latest setup
   ```

## Getting the binaries

Use your own install of the reference builds (see [hashes.md](hashes.md)) and copy the binaries into a folder **outside this repo**, or into `extracted/` (git-ignored). The game modules live inside `AVP2DLL.REZ`:

```sh
python tools/rez.py "C:/Program Files (x86)/Aliens vs. Predator 2/AVP2DLL.REZ" extracted/avp2/
python tools/rez.py "C:/Program Files (x86)/Aliens versus Predator 2 - Primal Hunt/AVP2DLL.REZ" extracted/ph/
```

Check that your hashes match before sharing any addresses.

## Common commands

```sh
# Overview + reusable analysis snapshot (fast follow-up queries)
npx rea-agents@latest analyze extracted/avp2/d3d.ren --provider ghidra --snapshot work/avp2_d3d.snap

# Pseudocode for one function, by name or address
npx rea-agents@latest decompile extracted/avp2/d3d.ren --procedure RenderDLLSetup --snapshot work/avp2_d3d.snap

# Dump the renderer slot table for two builds side by side
python tools/rea_render_slots.py work/avp2_RenderDLLSetup.txt work/ph_RenderDLLSetup.txt
```

## Remember

Ghidra/REA output is for **your understanding only**. Don't commit pseudocode, disassembly listings, Ghidra databases or REA evidence bundles (see [CONTRIBUTING.md](../CONTRIBUTING.md)). Document behavior, offsets and layouts in your own words, and write original code.
