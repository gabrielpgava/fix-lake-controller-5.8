# Lake Controller 5.8 — Windowed Fix

[Português](README.md) | [English](README.en.md)

> **PATCH ONLY — Lake Controller software is NOT included.**
> Lake Controller, Contour and other proprietary files are not redistributed.
> You need a legitimate original installation and a license where required.
> This is an independent, unofficial project, not endorsed by the manufacturer.
> MIT covers only this project's own files, not third-party software or trademarks.
> [Portuguese and English notice](NOTICE.md).

The notice is also included in `LICENSE` inside the ZIP and installer payload.

## Download and install

**[Download the Windows v0.7.0 installer](https://github.com/gabrielpgava/fix-lake-controller-5.8/releases/download/v0.7.0/LakeWindowedFix-Setup-v0.7.0.exe)**
· [Release, portable ZIP and checksums](https://github.com/gabrielpgava/fix-lake-controller-5.8/releases/tag/v0.7.0)

Requirements: Windows 10/11 and the legitimate **Lake Controller 5.8 Build 007
x86** installation admitted by the patch's hashes. The patch is x86; it also runs
on x64 Windows through WOW64. Other Lake builds, operating systems and processor
models are not automatically supported or validated.

1. Download the installer, `SHA256SUMS.txt` and `RELEASE-MANIFEST.json`. Verify the
   checksum. This is an **experimental, unsigned** installer; follow local Windows/
   EDR policy and do not disable security protections.
2. Close Lake. Open `LakeWindowedFix-Setup-v0.7.0.exe`, select **English** in
   **Idioma / Language**, check the detected installation, then **Install / Repair**.
   **Locate Lake...** selects a different original installation folder. Unsupported
   hashes are refused, not silently adapted.
3. Start using the **Lake Controller - Janela** shortcut. Its historical name is
   intentionally preserved in both languages. Setup never starts Lake automatically.
4. Test in an isolated, non-operational session, without active audio or equipment
   under control. **OFFLINE inside Lake does not isolate the network.** Establish
   actual isolation separately before testing on the computer connected to a processor.

The installer copies only the patch's own files into the current user's profile,
with desktop/Start menu shortcuts. It does not overwrite the original application's
files, change licensing, firmware, presets, audio backend or global display mode.
No administrator permission, compiler or PowerShell scripts are required on the
destination computer. The original software and its legitimate dependencies must
already be installed separately.

### Existing installations, repair and rollback

For a different patch generation, explicitly run the **external new installer**:

```powershell
.\LakeWindowedFix-Setup-v0.7.0.exe --upgrade --lang en
```

- `--lang en` / `--lang pt-BR`: select English / Portuguese. The initial language
  follows Windows (Portuguese for a Portuguese UI, English otherwise); the combo
  switches the installer's own texts without changing system language. Native
  Windows folder-picker buttons follow Windows itself.
- `--check`: read-only compatibility check; no installation.
- Same-generation **Install / Repair** restores recognized missing objects and
  merges missing configuration keys without replacing existing values.
- `--uninstall`: remove recognized patch files; edited INI, logs and modified/
  unknown objects are preserved. Receipts and recovery markers remain intentionally.
- `--rollback`: restore the **single retained previous generation**, including an
  interrupted upgrade. Keep Lake closed throughout. There is no arbitrary history,
  redo or automatic purge yet. Use the same installer to resume its interrupted update.

Do not recursively delete the install folder, remove receipts/journals to force an
update, or mix the native setup with the legacy PowerShell install/uninstall scripts.
The historical `v0.6` directory/key identifiers are retained to discover older installs;
they do not identify the version of the new executables. Detailed recovery procedure
in Portuguese: [SETUP-RECOVERY.md](docs/SETUP-RECOVERY.md).

## Portable alternative

Extract the ZIP's six files into a separate folder, not System32/SysWOW64. Keep
launcher, DLL and INI from the same build together:

```powershell
.\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

No injection into an existing instance. `--wait` observes the real exit code without
a session timeout; startup has separate limits. It does not kill/restart a session
because of a modal dialog, slow frame or absence of frames. Do not unload the pinned
DLL or remove active hooks.

`--native-only --wait` is a diagnostic control using the original `windowed
nonfullscreen` flags without patch DLL/host/hooks. It still runs the original
application's network/configuration behavior; it is not an isolation or audio safety mode.

## Configuration — close and reopen Lake to apply

Edit `LakeWindowedFix.ini` beside the DLL. Each session uses an immutable snapshot,
configuration schema 1 and an effective-configuration SHA-256. Missing legacy schema
is accepted; future schemas are refused. Repair preserves edits and unknown keys,
adds only missing keys and retains backups.

| Option | Default | Meaning |
| --- | --- | --- |
| `[render] Width/Height` | 1088×612 | Fixed canvas; admitted ranges 1024–4096 / 600–4096 |
| `[window] Width/Height` | 1024×576 | Initial logical window size; resize changes presentation, not canvas |
| `[window] MinWidth/MinHeight` | 800×450 | Interactive minimum capped to the work area; 320/180 opts into the older minimum |
| `[compatibility] ScaleRendering / NativeResize` | 1 / 0 | Aspect-preserving scaling/letterbox; native graphics rebuild remains experimental opt-in |
| `[render] SmoothFonts` / `[input] SystemCursor` | 1 / 1 | Existing smoothing/cursor options; set SystemCursor=0 for legacy/specialized bitmap cursor |
| `[diagnostics] DetailedInput` | 0 | Optional counters; does not enable fractional-delta accumulation |

The patch retains 32 bpp and legacy DPI awareness. Logical pixels do not guarantee
physical 1:1 pixels. Hybrid cursor and fractional-delta accumulation remain disabled
without sufficient proprietary callsite semantics evidence.

## Diagnostics and validation limits

Local logs: `logs\LakeWindowedFix-<PID>.log` beside the DLL, with a TEMP fallback.
They record session GUID, UTC/monotonic time, build/configuration and observed stages.
A fixed asynchronous queue and 4 MiB per-file limit can drop **diagnostic records
only**, counting losses; never input/frames. Logs are not uploaded. Review before sharing.
A first-chance exception may be handled by the application and is not proof of a crash.

The user reported a positive local smoke test of the previous candidate (`8949dac`).
That is not an independent measurement, destination-computer approval, real GPU/
driver matrix, sustained FPS/leak result, audio/hardware approval or validation of
the newly localized installer. Synthetic tests do not establish those properties.
No operational Lake/audio/equipment test was run automatically for this release.

[Implementation and remaining scope](docs/IMPLEMENTATION.md) ·
[Recorded validation](docs/VALIDATION-07.md) · [Historical results](HISTORY.md).
These detailed engineering documents are currently in Portuguese. Window placement/
profile UI, external dump collector, cross-session retention, PE VERSIONINFO and
production signing remain pending. Symbol stripping is not path anonymization.

## Build and contribute

Windows x86, C++17. Use existing Zig **0.14.1**, local LLVM objcopy for own symbols,
PowerShell 5.1, Git and Python 3; no automatic downloads in the build scripts.

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:\tools\zig\zig.exe -ObjcopyPath C:\tools\LLVM\bin\llvm-objcopy.exe -OutputDirectory dist-release-v0.7.0
.\scripts\Build-Installer.ps1 -ZigPath C:\tools\zig\zig.exe -ObjcopyPath C:\tools\LLVM\bin\llvm-objcopy.exe -AllowUnsignedDevelopment
make release-check ZIG=C:/tools/zig/zig.exe OBJCOPY=C:/tools/LLVM/bin/llvm-objcopy.exe
```

MSVC x86/CMake/Windows SDK is an alternative via `-Toolchain MSVC`; MinGW GCC remains
experimental without local execution evidence. `-SkipTests`, Debug and profiling
builds cannot be packaged for distribution. Final stripped images are tested before
payload/hash freeze. Exact optimized same-link PDBs, originals and receipts remain
in private build stores, never in the operator ZIP/setup.

`release-check` builds/tests/packages a local candidate, never publishes. Publication
is a separate explicit operation with clean-source, exact-artifact and `origin/main`
guards. No proprietary binaries, resources, firmware, presets, dumps or private logs
may be committed/packaged. Do not broaden the Build 007 allowlist or bypass licensing.
See [CONTRIBUTING.md](CONTRIBUTING.md), [BUILD-PROVENANCE.md](docs/BUILD-PROVENANCE.md)
and [SYMBOLS.md](docs/SYMBOLS.md). The [MIT license](LICENSE) applies only to our files.
