# AeroDynamics Native Desktop Widget Suite

A high-performance, uniquely native Win32/GDI+ desktop widget suite engineered strictly in lightweight C++! 
It seamlessly marries real-time zero-overhead hardware monitoring with an aggressive token-economy Focus Engine designed specifically for power users.

## Features

### Hardware Monitoring (Zero Driver Overhead)
- **Top-tier Performance**: Evaluates entirely inside PDH & DXGI natively for sub 1% CPU footprint.
- **Dynamic Bars**: Automatically draws glowing gradient metrics for real-time CPU utilization, GPU processing overhead, dynamic RAM/VRAM saturation margins, and Disk/Network ingestion rates.
- **Deep WMI Hooks**: Interfaces securely with OpenHardwareMonitor protocols to scrape core component temperatures safely without requiring root-level driver abstraction.

### Focus Engine (The Native Token Economy)
- **Earning Tokens**: Native hooks into `GetForegroundWindow` implicitly track your focus on Good Apps (VS Code, Visual Studio, IDEs) and instantly increment a visual Token Bank at an exact 1:1 per-second timeline.
- **Draining Tokens**: Extracts the raw URL intelligently from chromium web browsers using Microsoft's UIAutomation (UIA) COM APIs! Browsing Social Media mathematically evaporates your bank at a severe *4x drain multiplier*, and Media ingestion drains at a *3x multiplier*. 
- **The Ban Hammer**: When you expend all earned time and try to inspect a distraction, the Focus Engine dynamically injects a synthesized `CTRL + W` direct keyboard injection keystroke to explicitly nuke the exact browser tab violating the rules, safely protecting your active research tabs natively.
- **AFK Suspension**: Automatically integrates native `GetLastInputInfo` hooks to completely freeze token generation securely the moment you walk away or idle dynamically for two straight minutes.
- **Persistent Analytics Dashboard**: Automatically catalogs every non-AFK millisecond spent inside every app or domain natively, then sorts and populates the Top 4 screen-time metrics distinctly beside the Master Screen Time Benchmark gradient!

## Architecture

Architected specifically across 3 independent, Z-order synchronized `WS_EX_LAYERED` Windows using GDI+ in memory compositions to permanently sidestep Windows 11 DWM bugs while anchoring flawlessly underneath active workflow operations natively:
1. **The Core Hardware Engine** (`340x430` footprint) tracking system vitality precisely.
2. **The Focus Analytics Deck** (`400x480` footprint) rendering detailed deep-dive app engagements.
3. **The Micro-Indicator Dot** (`16x16` pixel footprint) vaulting `HWND_TOPMOST` constantly mapping visual engine states passively without demanding visual focus. 

## Build Instructions
1. Open up Visual Studio natively (built with MSVC standard pipelines in x64 Debug/Release context).
2. All libraries linked via pragmas including `uiautomationcore.lib`, `pdh.lib`, and `wbemuuid.lib`.
3. Simply drop any localized `.ttf` or `.otf` assets right into `\assets\fonts\` directory relative natively to the exported `.exe` context to automatically populate and mount custom typography pipelines.

## Development Branches
- `main` strictly locked for verified, fully-functional desktop production deployments.
- `experimental` mapping deep-system architectural rewrites.
- `new_feature` isolated pipeline branch for live logic mapping and testing bounds.
