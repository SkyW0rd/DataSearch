<div align="center">

# 🔍 DataSearch

[![Build](https://github.com/SkyW0rd/DataSearch/actions/workflows/build.yml/badge.svg)](https://github.com/SkyW0rd/DataSearch/actions/workflows/build.yml)
[![License: PolyForm Noncommercial](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-blue)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)](#-platform-status)

**Русская версия:** [README.md](README.md)

A cross-platform desktop file search tool, built on Elasticsearch-style principles (inverted index, tokenization, BM25 ranking) — but fully local and offline.

Closest in spirit to **Everything** (Windows) or **Spotlight** (macOS), but with full-text search inside file contents (including DOCX/XLSX/PDF) and first-class support for Russian morphology.

![App screenshot](assets/screenshot.png)

</div>

> [!IMPORTANT]
> Built and actually tested on **macOS** and **Windows**. The Linux implementation is written against the documented platform APIs but hasn't been built or run yet — see [platform status](#-platform-status).

## ⚙️ Features

- **Search by filename and content** — TXT, MD, source code, DOCX, XLSX, PDF.
- **Russian morphology**: searching "практикум" also finds "практикума", "практикумом", "практикумы" — a from-scratch implementation of the Snowball stemming algorithm, plugged in as a custom SQLite FTS5 tokenizer.
- **Query operators**: `"exact phrase"`, `-exclude`, `ext:docx`, `path:D:\Work\`.
- **Relevance ranking** (BM25) with highlighted match snippets.
- **Index several disks/folders at once**, with search results merged across all selected sources.
- **Live filesystem watching** (Windows: `ReadDirectoryChangesW`, macOS: `FSEvents`, Linux: `inotify`) — new/changed files are picked up automatically, no manual reindexing needed.
- **Fast startup reconciliation**: indexes from a previous session open instantly; only metadata changes are checked in the background instead of a full rescan.
- **Resilient to unavailable sources**: if a network drive is temporarily disconnected, the last saved index is used instead of deleting its data.
- **Pause/resume indexing** without losing progress.
- Fully offline — no network calls, no telemetry.

## 🖥️ Platform status

| Platform | Status |
|---|---|
| **macOS** | ✅ Implemented and tested — the app builds and actually runs (volumes via `getmntinfo`, Finder integration via `NSWorkspace`, filesystem watching via `FSEvents`). |
| **Windows** | ✅ Implemented and tested — the app builds and actually runs (`ReadDirectoryChangesW`, `ShellExecuteW`, `SHOpenFolderAndSelectItems`). |
| **Linux** | ⚠️ Implemented (`/proc/mounts`, `inotify`, `xdg-open`), but never built or run — no Linux machine was available. |

The core (indexing, search, text extraction, Russian stemmer) is fully cross-platform, covered by automated tests, and runs in CI/locally independent of the GUI layer.

## 🚀 Building

### Requirements

- CMake ≥ 3.21, a C++20-capable compiler
- Qt6 (Core/Gui/Widgets — on macOS via Homebrew this is the lightweight `qtbase` package, not the full `qt`)
- SQLite3 (with FTS5 support), zlib

### macOS

```bash
brew install cmake qtbase sqlite3
git clone <repo-url>
cd DataSearch
cmake -S . -B build
cmake --build build -j
./build/src/app/datasearch_app
```

### Windows

```powershell
# Qt6 and CMake must be on PATH
cmake -S . -B build
cmake --build build --config Release
.\build\src\app\Release\datasearch_app.exe
```

### Core only (no GUI, for CI/tests on any OS)

```bash
cmake -S . -B build -DDATASEARCH_BUILD_APP=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## ℹ️ Usage

1. Launch the app — the disks visible to the system appear on the left (same as File Explorer/Finder).
2. The **"Add folder..."** button lets you index a specific folder instead of a whole disk — recommended for a first try, since indexing an entire system disk can take a long time.
3. Check the sources you want and click **"Index selected"**.
4. Search in the search box — results update as you type.

## 🏗️ Architecture

```
src/
  core/       — platform-independent core: filesystem scanning, SQLite/FTS5
                index, text extraction (DOCX/XLSX/PDF), Russian stemmer, search
  platform/   — IPlatformService: disks, "show in folder", filesystem watching —
                separate implementation per Windows/macOS/Linux
  app/        — Qt Widgets UI
tests/        — core automated tests (ctest)
```

`core` has no dependency on Qt or any specific OS — it's tested independently and ports between platforms unchanged.

## ⭐ Support the project

If you find this useful, consider giving the repo a :star: — it helps others find it.

## ⚖️ License

[PolyForm Noncommercial License 1.0.0](LICENSE) — free to use for noncommercial purposes; commercial use requires separate permission from the rightsholder.

---

<div align="center">

Made with 🩷 by [SkyW0rd](https://github.com/SkyW0rd)

</div>
