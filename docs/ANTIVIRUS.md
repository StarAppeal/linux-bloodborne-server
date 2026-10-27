# Antivirus false positives

v1.3.0's `BLauncher.exe` was flagged by Windows Defender as **Trojan:Win32/Bearfoos.A!ml** (player
report 2026-10-26; Malwarebytes found it clean). The `!ml` suffix means Defender's cloud machine
learning guessed, not that a signature matched. It happens often to PyInstaller programs: the launcher
is a Python program frozen with PyInstaller, unsigned, and every release is a file Microsoft has never
seen. Bearfoos's text ("executes commands from an attacker") is the family's generic description.

## What the build does against it

`bbport/packaging/windows/package.sh` freezes the launcher with:

- a **version resource** (`version_info.py`): product, company, description, version, license. v1.3.0
  had none, so nothing in the exe said what it was;
- **`--noupx`**: PyInstaller packs with UPX whenever `upx.exe` is on PATH, a packer malware uses too;
- a **pinned PyInstaller** (`pyinstaller_version`), so the bootloader only changes when we bump it.

`tools/release/check_release.py` fails a release whose launcher has no version resource or is UPX packed.

None of this guarantees a clean verdict. Only code signing builds lasting reputation (see the PR for the
options); until then, submit every release.

## Submitting a release to Microsoft (every release, a new hash each time)

1. Open <https://www.microsoft.com/en-us/wdsi/filesubmission> and sign in with a Microsoft account.
2. Choose **Software developer** (results come faster and reach every Defender user).
3. Product: **Microsoft Defender Antivirus (Windows 10/11)**.
4. File: the release's `BLauncher.exe` (from `<Out>\BloodborneSeamlessCoop\`), and in a second
   submission the setup `YharnamTogether-v<version>-setup.exe`.
5. "What do you believe this file is?": **Incorrectly detected as malware/malicious**.
6. Detection name: `Trojan:Win32/Bearfoos.A!ml`. Definition version: Windows Security, Settings,
   About (or `Get-MpComputerStatus | Select AntivirusSignatureVersion` in PowerShell).
7. Notes: "Open-source game launcher (GPL-2.0, https://github.com/lKaniel/bloodborne-server), frozen
   with PyInstaller. Release <version>, SHA-256 <hash from the .sha256 file>."
8. The submission page shows its status; a "not malware" verdict usually lands within a few days and
   ships in the next definition update.

## What players can do meanwhile

After Microsoft's verdict, update Defender's definitions (Windows Security, Virus & threat protection,
Protection updates, Check for updates), or in an administrator PowerShell:

```
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -RemoveDefinitions -DynamicSignatures
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -SignatureUpdate
```

Then restore the quarantined launcher (Protection history, the detection, Actions, Restore) or
reinstall. Before that, a player who trusts the download can compare its SHA-256 with the release's
`.sha256` file and allow it in Protection history.
