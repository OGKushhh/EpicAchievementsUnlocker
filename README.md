<!-- Banner – replace with your own image URL or keep as text -->
<p align="center">
  <img src="https://github.com/user-attachments/assets/56341dfa-2309-4d55-8e51-b8e919360540" alt="Epic Unlocker Banner">
</p>

<h1 align="center">🎮 Epic Unlocker – Achievements & DLC Unlocker for EOS Games</h1>

<p align="center">
  <strong>Unlock Epic Games Store achievements in any EOS‑powered game – with or without an overlay.</strong>
</p>

<p align="center">
  <a href="#-features">Features</a> •
  <a href="#-quick-start">Quick Start</a> •
  <a href="#-how-it-works">How It Works</a> •
  <a href="#-configuration">Configuration</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/EOS%20SDK-1.18.1.2-blue" alt="EOS SDK">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue" alt="C++">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  <img src="https://img.shields.io/badge/platform-Windows-lightgrey" alt="Platform">
</p>

---

## 🔥 What is Epic Unlocker?

Epic Unlocker is a **DLL injection tool** that hooks the Epic Online Services (EOS) SDK. It lets you **unlock any achievement** in games that use EOS – even if the game uses **DirectX 12** where traditional overlays fail.

> **No more staring at a black overlay.**  
> Press `Ctrl+Shift+U/L` and watch your achievements pop.

### The Problem We Solve

- ❌ ScreamAPI removed the feature.  
- ❌ Many games use **DX12 / Vulkan** – standard D3D11 overlay hooks don't work.  

### Our Solution

✅ **Global hotkeys** – unlock everything with `Ctrl+Shift+U` (works anywhere, even in DX12).  
✅ **Selective unlock** – use `Ctrl+Shift+L` with a simple text file.  
✅ **Clean overlay** – a modern ImGui window shows progress, search, and filter.  
✅ **External GUI** – a standalone Achievement Unlocker window for unlocking without touching the game.  
✅ **No background threads** – the tool is lightweight and won't trigger anti‑tamper.  

---

## ✨ Features

| Feature | Description |
|---------|-------------|
| 🕹️ **Universal** | Works with any EOS‑powered game (e.g., *TMNT: Splintered Fate*, *Dying Light*, *Northgard*). |
| ⌨️ **Global Hotkeys** | `Ctrl+Shift+U` → unlock all. `Ctrl+Shift+L` → unlock list from `unlock_list.txt`. |
| 🖥️ **Overlay** | Beautiful ImGui window with stats, search/filter, and per‑achievement unlock buttons (press `Shift+F5`). |
| 🪟 **External GUI** | Standalone dark-themed GUI app — connect to any running game and unlock achievements without an overlay. Double-click any achievement to unlock instantly. |
| 📁 **File‑based unlock** | Drop a text file with achievement IDs – press hotkey to unlock only those. |
| 📊 **Auto‑logging** | Achievement statistics are written to `ScreamAPI.log` every time player data loads. |
| ⚙️ **Configurable** | Toggle overlay, DX12 hook, logging, forced achievement queries, custom EOS SDK path, etc. |
| 🔌 **MinHook + Kiero** | Reliable hooking of both EOS functions and graphics APIs. |

---

## 🚀 Quick Start

Get Epic Unlocker running in your favourite game **in under 5 minutes**.

### 1. Download the latest release

Grab `EOSSDK-Win64-Shipping.dll` (and `ScreamAPI.ini`) from the [Releases](../../releases) page.

### 2. Inject the DLL

You have two options:

#### A) Proxy Mode (easiest)
1. Rename the game's original `EOSSDK-Win64-Shipping.dll` to `EOSSDK-Win64-Shipping_o.dll`.
2. Copy `EOSSDK-Win64-Shipping.dll` into the same folder.
3. Launch the game – Epic Unlocker will forward most calls to the original DLL.

#### B) Hook Mode (using Koaloader)
1. Download [Koaloader](https://github.com/acidicoala/Koaloader/releases).
2. Rename our DLL to `ScreamAPI64.dll` (or 32) and place it (with the INI) in your game directory.
3. Configure `Koaloader.json` to inject `ScreamAPI64.dll` into the game process.
4. Launch the game – no file renaming needed.

### 3. Unlock achievements

#### Using the Overlay (in-game)
Press **`Shift+F5`** to toggle the overlay. Search, filter, and unlock achievements directly from inside the game.

> ⚠️ **Fullscreen Exclusive games** will minimize when you click overlay buttons. Either:
> - Switch the game to **Borderless Windowed** mode, or
> - **Double-click** an achievement in the list to unlock it (no minimize).

#### Using the External GUI
Launch `EpicAchievementUnlocker.exe` while the game is running. The GUI connects automatically and shows all achievements. Double-click any achievement to unlock it instantly.

> ⚠️ The external GUI also minimizes fullscreen exclusive games when using the **Unlock Selected** button. Use double-click or switch to Borderless Windowed.

#### Using Hotkeys (works everywhere, even DX12)
- **Unlock everything** – press **`Ctrl+Shift+U`** anywhere.
- **Unlock specific achievements**
  1. Create `unlock_list.txt` in the **same folder as the game's .exe**.
  2. Put one achievement ID per line (IDs shown in `ScreamAPI.log` or the overlay).
  3. Press **`Ctrl+Shift+L`**.

---

## 🧠 How It Works

Epic Unlocker uses two powerful hooking libraries:

- **MinHook** – intercepts EOS SDK functions (`EOS_Achievements_UnlockAchievements`, `EOS_Auth_Login`, etc.).
- **Kiero** – hooks the graphics API (`IDXGISwapChain::Present`) to render the ImGui overlay.

When the game requests achievement definitions or player progress, Epic Unlocker stores the data locally.  
When you press a hotkey or click unlock, it calls the original (hooked) `EOS_Achievements_UnlockAchievements` with the correct parameters – **instantly** unlocking the achievement on Epic's servers.

The external GUI communicates with the in-game DLL over a named pipe. Unlock commands are queued and executed safely on the game thread during `EOS_Platform_Tick` – avoiding any thread-safety issues with the EOS SDK.

---

## ⚙️ Configuration

Edit `ScreamAPI.ini` to customise behaviour:

```ini
[ScreamAPI]
; General options
EnableOwnershipUnlocker       = True      ; Unlock DLC ownership checks (was EnableItemUnlocker)
EnableEntitlementUnlocker     = True      ; Unlock entitlement queries
EnableLogging                 = True      ; Generate ScreamAPI.log
EnableOverlay                 = True      ; Show achievement overlay
ForceAchievementsConfig       = False     ; Force achievement definitions query before unlock
EnableKeyboardNavigation      = True      ; Allow keyboard navigation in overlay (arrows, Tab, Enter, Esc)
EnableDX12Hook                = False     ; Hook DX12 games (DX11 is always tried first)
BlockMetrics                  = True     ; Block Epic telemetry/metrics (if supported)
; Optional custom paths / overrides
CustomEOSPath                 =           ; Absolute path to EOSSDK-Win64-Shipping.dll (optional)
NamespaceId                   =           ; Override the game's namespace ID (rarely needed)

[Logging]
LogLevel                      = INFO      ; DEBUG / INFO / WARN / ERROR
LogFilename                   = ScreamAPI.log
LogDLCQueries                 = True      ; Log DLC ownership/entitlement checks
LogAchievementQueries         = True      ; Log achievement queries and unlocks
LogOverlay                    = True      ; Log overlay (initialization, toggles, etc.)

[Overlay]
LoadIcons                     = False     ; Download achievement icons (True = download, False = no icons)
CacheIcons                    = True      ; Save icons locally (if LoadIcons=True)
ValidateIcons                 = True      ; Check cached icon size against online version (if LoadIcons=True)
ForceEpicOverlay              = False     ; Re-enable the original Epic overlay if the game disabled it

[DLC]
UnlockAllDLC                  = True      ; Respond positively to all DLC requests
ForceSuccess                  = False      ; Always return EOS_SUCCESS even if internal checks fail

; ----------------------------------------------------------------------
; NEW: Per-item DLC override (replaces old [DLC_List] for fine control)
; Format: {item_id} = unlocked | locked | original
;   unlocked – always return owned/unlocked
;   locked   – always return not owned/locked
;   original – use the value returned by the EOS backend (or game default)
; ----------------------------------------------------------------------
[DLC_Override]
; Example: 56acef6d526e4b819caff773dd244635 = unlocked   ; Subject 2923 DLC unlocked
; Example: 14d1517dbd7242bcb5cb94881ca1c28f = locked     ; Swamps of Corsus locked

; ----------------------------------------------------------------------
; NEW: Manually inject extra entitlements (useful for testing or adding missing items)
; Format: {entitlement_id} = Display Title
; ----------------------------------------------------------------------
[Extra_Entitlements]
; Example: some_entitlement_id = My Custom Item

; ----------------------------------------------------------------------
; LEGACY: Old-style DLC list (still supported, but consider using [DLC_Override] instead)
; This section is only used when UnlockAllDLC = False.
; Format: {item_id} = True   (to unlock) or = False (to block)
; ----------------------------------------------------------------------
[DLC_List]
; Example: 56acef6d526e4b819caff773dd244635 = True
; Example: 14d1517dbd7242bcb5cb94881ca1c28f = False
```

### When to enable `EnableDX12Hook`

Only enable this for **true DX12 games** (e.g., games where neither the overlay nor the DX11 fallback renders). For most games — including DX11 games that load `d3d12.dll` — leave it `False`. The hotkeys and external GUI always work regardless of this setting.
