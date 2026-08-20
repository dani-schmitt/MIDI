# LoopBe30 2.0 setup

This WiX 4 setup installs the independent LoopBe30 Windows MIDI Services
transport and the `loough.exe` tray monitor. It deliberately neither installs
nor detects Microsoft Basic Loopback. A legacy LoopBe30 kernel-driver service or
device blocks installation until the old product has been removed and Windows
has restarted.

Retail replaces Trial while preserving the machine settings. Trial refuses to
install over Retail. Setup follows the proven ipMIDI pattern: it stops and
restarts Windows MIDI Services, activates two default ports immediately, and
launches the LoopBe30 Monitor without requiring a reboot. Later port-count
changes still take effect after the next reboot.

The build emits exactly these bundles:

- `setuploopbe30.exe`
- `setuploopbe30trial.exe`
- `setuploopbe30-arm64.exe`
- `setuploopbe30trial-arm64.exe`

Run the unsigned release through NUKE with
`T_BuildLoopBe30PluginInstaller`. After the unsigned matrix and tests pass,
sign in to Certum SimplySign Desktop and run
`T_BuildSignedLoopBe30PluginInstaller --confirm-simple-sign-ready`.

For a faster unsigned x64 Trial test build, run
`build\loopbe30\Build-LoopBe30.ps1 -X64TrialInstallerOnly`.
