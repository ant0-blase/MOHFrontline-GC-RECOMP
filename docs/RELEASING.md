# CI / CD and releases

The repository uses two GitHub Actions workflows.

## CI

`.github/workflows/ci.yml` runs automatically on pushes to `main`, pull requests, and manual dispatch.

It requires **no retail game data**. It validates scripts and Python tools, runs game-independent ModernGekko tests on Linux and Windows, builds the standalone recompilation launcher on both platforms, and uploads short-lived launcher artifacts.

The CI also rejects accidentally tracked disc images and `HD/PS3_FILES` content.

## Releases

`.github/workflows/release.yml` is manual-only. Normal development commits never create tags or GitHub Releases.

To publish, open **Actions → Release → Run workflow**, enter a version such as `v0.1.0`, and run it from the revision you want to publish.

Produced assets:

```text
MOHFrontline-GC-RECOMP-vX.Y.Z-Linux-x86_64.tar.gz
MOHFrontline-GC-RECOMP-vX.Y.Z-Linux-x86_64.tar.gz.sha256
MOHFrontline-GC-RECOMP-vX.Y.Z-Windows-x86_64.zip
MOHFrontline-GC-RECOMP-vX.Y.Z-Windows-x86_64.zip.sha256
```

Each package contains the clean source tree and the prebuilt standalone launcher. It intentionally contains no retail game, extracted GMFE69 data, PS3 assets, or prebuilt game-derived recompilation module.

The user selects their own legally obtained GMFE69 ISO in the launcher and builds/prepares the recompilation locally.

If a release already exists, run the workflow again with the same version and `replace_assets=true` to replace only its uploaded assets.
