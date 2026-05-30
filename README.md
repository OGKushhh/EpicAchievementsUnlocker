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
- ❌ Many games use **DX12 / Vulkan** – standard D3D11 overlay hooks don’t work.  

### Our Solution

✅ **Global hotkeys** – unlock everything with `Ctrl+Shift+U` (works anywhere, even in DX12).  
✅ **Selective unlock** – use `Ctrl+Shift+L` with a simple text file.  
✅ **Clean overlay** – for D3D11 games, a modern ImGui window shows progress, search, and filter.  
✅ **No background threads** – the tool is lightweight and won’t trigger anti‑tamper.  

---

## ✨ Features

| Feature | Description |
|---------|-------------|
| 🕹️ **Universal** | Works with any EOS‑powered game (e.g., *TMNT: Splintered Fate*, *Dying Light*, *Northgard*). |
| ⌨️ **Global Hotkeys** | `Ctrl+Shift+U` → unlock all. `Ctrl+Shift+L` → unlock list from `unlock_list.txt`. |
| 🖥️ **D3D11 Overlay** | Beautiful ImGui window with stats, search/filter, and per‑achievement unlock buttons. |
| 📁 **File‑based unlock** | Drop a text file with achievement IDs – press hotkey to unlock only those. |
| 📊 **Auto‑logging** | Achievement statistics are written to `ScreamAPI.log` every time player data loads. |
| ⚙️ **Configurable** | Toggle overlay, logging, forced achievement queries, custom EOS SDK path, etc. |
| 🔌 **MinHook + Kiero** | Reliable hooking of both EOS functions and graphics APIs. |

---

## 🚀 Quick Start

Get Epic Unlocker running in your favourite game **in under 5 minutes**.

### 1. Download the latest release

Grab `EOSSDK-Win64-Shipping.dll` (and `ScreamAPI.ini`) from the [Releases](../../releases) page.

### 2. Inject the DLL

You have two options:

#### A) Proxy Mode (easiest)
1. Rename the game’s original `EOSSDK-Win64-Shipping.dll` to `EOSSDK-Win64-Shipping_o.dll`.
2. Copy `EOSSDK-Win64-Shipping.ll` into the same folder as original `EOSSDK-Win64-Shipping.dll`.
3. Launch the game – Epic Unlocker will forward most calls to the original DLL.

#### B) Hook Mode (using Koaloader)
1. Download [Koaloader](https://github.com/acidicoala/Koaloader/releases).
2. Rename our DLL to `ScreamAPI64.dll` (or 32) and place it (with the INI) in your game directory.
3. Configure `Koaloader.json` to inject `ScreamAPI64.dll` into the game process.
4. Launch the game – no file renaming needed.

### 3. Unlock achievements

For DirectX 11 Games An overlay shows up. For non-DX11:
- **Unlock everything** – press **`Ctrl+Shift+U`** anywhere (even when the game is in the background).
- **Unlock specific achievements**  
  1. Create a file called `unlock_list.txt` in the **same folder as the game’s .exe**.  
  2. Put one achievement ID per line (IDs are shown in `ScreamAPI.log` or in the overlay).  
  3. Press **`Ctrl+Shift+L`** – only those achievements will unlock.

> 💡 **Tip**: If the overlay doesn’t appear (e.g., DX12 games), don’t worry – the hotkeys still work!

---

## 🧠 How It Works

Epic Unlocker uses two powerful hooking libraries:

- **MinHook** – intercepts EOS SDK functions (`EOS_Achievements_UnlockAchievements`, `EOS_Auth_Login`, etc.).  
- **Kiero** – hooks the graphics API (`IDXGISwapChain::Present`) to render the ImGui overlay.

When the game requests achievement definitions or player progress, Epic Unlocker stores the data locally.  
When you press a hotkey, it calls the original (hooked) `EOS_Achievements_UnlockAchievements` with the correct parameters – **instantly** unlocking the achievement on Epic’s servers.

---

## ⚙️ Configuration

Edit `ScreamAPI.ini` to customise behaviour:

```ini
[ScreamAPI]
EnableOverlay = true          ; Show ImGui window (D3D11 only)
EnableLogging = true          ; Write logs to ScreamAPI.log
LogLevel = debug              ; debug, info, warn, error
ForceAchievementsConfig = false ; Some games need this to unlock (enable if stuck)
EnableKeyboardNavigation	= true			; Enable keyboard navigation in overlay (arrow keys, Tab, Enter, Esc)? Default: True
CustomEOSPath =               ; Manual path to EOSSDK-Win64-Shipping.dll (if auto search failed)
