# MQ2CharCreate

MacroQuest plugin for automated EverQuest character creation. Drives the full character creation flow (login → server select → create → enter world) and populates the MQ2AutoLogin database on success.

## Installation

Clone into the MacroQuest plugins directory and regenerate the solution:

```powershell
cd macroquest/plugins
git clone https://github.com/JMFlin/MQ2CharCreate.git
cd ../..
.\gen_solution.ps1
# Open build\solution\MacroQuest.sln, select Release x64, Build
```

## Architecture

- **tinyfsm** state machine drives the UI automation (same pattern as MQ2AutoLogin)
- `StateMachine.cpp` — FSM state implementations (splash → connect → server select → char create screens)
- `MQ2CharCreate.h` — FSM base class, events, window helper templates, UI control name constants
- `MQ2CharCreate.cpp` — Plugin lifecycle, `/charcreate` slash command, OnPulse sensor, ImGui overlay

### Key patterns

- **HAS_GAMEFACE_UI dual-path:** window helpers mirror MQ2AutoLogin's approach for PRECHARSELECT compatibility
- **Window constants:** all UI control names in `CCWindowNames` namespace, discovered via in-game hover logging
- **Race/class resolution:** abbreviations and full names both accepted (e.g., `"shd"` → `"Shadow Knight"`)
- **Coexistence with MQ2AutoLogin:** AutoLogin self-pauses when no profile exists; this plugin's FSM takes over

## Usage

```
/charcreate <server> <account> <password> <name> <race> <class> [gender]
/charcreate status
/charcreate stop
```

## Build

Requires MacroQuest SDK (built as part of the MacroQuest solution). C++17, MSVC x64.

## MacroQuest documentation

- **[docs.macroquest.org](https://docs.macroquest.org/)** — Main documentation site
- **[Top-Level Objects](https://docs.macroquest.org/reference/top-level-objects/)** — All TLOs (Me, Spawn, Spell, Group, Raid, etc.)
- **[Slash Commands](https://docs.macroquest.org/reference/commands/)** — All `/commands`
- **[Data Types](https://docs.macroquest.org/reference/data-types/)** — TLO member/method reference per type
- **[Plugins](https://docs.macroquest.org/plugins/)** — Plugin documentation (MQ2Nav, MQ2AutoLogin, etc.)

## Code Quality Rules

- Follow MacroQuest plugin conventions (PreSetup, PLUGIN_VERSION macros)
- Use `CC_` prefix on all exported symbols to avoid collisions with MQ2AutoLogin
- Window/control names go in `CCWindowNames` namespace, not scattered as string literals
- Debug logging via `CC_DebugLog()` — writes to config dir, visible even during PRECHARSELECT

## Related Repos

- **[eq-dashboard](https://github.com/JMFlin/eq-dashboard)** — Tauri dashboard that orchestrates character creation requests
- **[eq-lua](https://github.com/JMFlin/eq-lua)** — Custom Lua scripts for coordinator bridge and utilities
