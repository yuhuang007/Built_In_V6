# Built_In_V6 Full Local Snapshot

This repository accompanies a complete local backup requested by the project owner.
The binary backup is distributed as numbered GitHub Release assets because it
exceeds normal Git file-size limits. Existing source files were not rewritten.
The repository also contains a browsable copy of the versioned main project,
the latest uncommitted development record and the external voice SDK.

## Included

- Built_In_V6, including hidden files, its original Git history, uncommitted files,
  local secrets, virtual environment, component cache and build outputs.
- The sibling ConversationalAI-Embedded-Kit-2.0-main_Korvo_2 SDK used by CMake.
- The local ESP-IDF tree, Espressif tools directory and virtual environment's
  referenced Python base installation.
- File-by-file SHA256 inventory, volume hashes and complete archive hash.

CS_001 and CS_002 secondary-board test projects are not included. Files from the
separate training computer, physical SD card and hardware are not part of this
local filesystem snapshot. This backup does not imply complete product acceptance.

## Restore

Download every numbered `Built_In_V6_full_snapshot.zip.partNNN` asset from the same
release, plus `checksums.json`, `snapshot_manifest.json` and `restore_snapshot.py`.
Keep them in one directory and run:

```powershell
python .\restore_snapshot.py
```

The script checks all volume hashes and the reconstructed ZIP hash. Extract the
resulting ZIP into a new directory. Keep Built_In_V6 and the SDK as siblings.
`SNAPSHOT_ROOTS.json` records the original paths for environment restoration.

Copied virtual environments and compiler installations are machine-specific.
Restore original paths or recreate the environment and adapt the build entry
point to the new machine; copying alone does not guarantee compilation.

The main product build explicitly uses `-Profile sign`. Do not automatically
flash hardware or execute historical training prompts during handoff.

## Source Baseline

- Branch: feature/voice-to-text
- Git HEAD: f8a2bb5ec875fba5f975fcd2ba5999329af2589a
- Last documented physical P4 flash: 5fe8fe6 on COM20 (latest recorded firmware; scrolling and camera color issues remain unresolved).
- Original source working tree and its existing remote were left unchanged.

This is an owner-authorized unredacted backup, not a sanitized source release.
Its configuration and binaries can contain operational credentials.

Snapshot refreshed 2026-11-10 (Asia/Shanghai). Includes current uncommitted MQDD and design files. Earlier release packages are superseded.
