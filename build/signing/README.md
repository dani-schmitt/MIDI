# ipMIDI release signing

Ordinary Debug and Release builds are intentionally unsigned. Use the dedicated
signed-release target only for artifacts that will be distributed.

1. Sign in to Certum SimplySign Desktop and confirm that the cloud certificate
   is available.
2. From `build\nuke_build-plugins`, run:

   ```powershell
   .\build.cmd T_BuildSignedIpMidiPluginInstaller --confirm-simple-sign-ready
   ```

The signing preflight selects the valid Current User `My`-store code-signing
certificate whose subject is `Daniel Schmitt`. If more than one valid
certificate matches, select one for that run without committing its thumbprint:

```powershell
.\build.cmd T_BuildSignedIpMidiPluginInstaller `
    --confirm-simple-sign-ready `
    --ip-midi-signing-thumbprint <thumbprint>
```

The target builds and signs the x64 and ARM64 Retail and Trial artifacts from
the inside outward. It writes the four public setup executables and
`ipmidi-signing-manifest.json` to a timestamped folder under `build\release`.

Never store certificate private keys, PINs, or a replacement certificate's
thumbprint in the repository.
