# sonmat-hook-fmodex

A part of 손맛 햅틱 / Sonmat Haptics (`sonmat`): dshaptics_fmodex.dll.

Hook for games that ship FMOD Ex (fmodex64.dll: Sekiro): FMOD::System::playSound by its exported name, no byte patterns.

The app does not carry this in its installer for good: it fetches the release zip that `sonmat-games/parts.json` names
(address + SHA-256) into `<app>/parts/hook-fmodex/` when a game needs it (`sonmat/tools/partfetch.py`). A change here reaches
players when a new tag has been released and `parts.json` points at it.

Build: Visual Studio C++ tools (x64), MinHook in minhook\ at the commit the workflow pins: build.bat dshaptics_fmodex. Every push to main builds it on GitHub (artifact); a tag `v*` makes the release.

MinHook (Tsuda Kageyu, BSD 2-Clause) is compiled in; its license is in the release zip as `MinHook-LICENSE.txt`.
