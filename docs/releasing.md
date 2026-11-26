# Releasing

[Knope](https://knope.tech) builds releases from the change files in `.changeset/`.

1. When change files land on `main`, CI opens (or updates) a **chore: release obs-indicator x.y.z** pull request.
   It bumps the version in `CMakeLists.txt`, adds the entries to `CHANGELOG.md` and deletes the change files.
2. Merge it when you want to release. CI tags `vX.Y.Z` and publishes a GitHub release with the zips.

To publish a version that's in `CMakeLists.txt` but has no tag yet, such as the first one (1.0.0), run the **Build**
workflow by hand on `main`.

- Don't edit the version or `CHANGELOG.md` by hand.
- The repo setting *Allow GitHub Actions to create and approve pull requests* must stay on.
- The release pull request gets no build check, because GitHub doesn't run workflows on pull requests opened by
  Actions. The build runs once it's merged.

## Updating OBS

Change `OBS_VERSION` and `OBS_SHA256` in `.github/workflows/build.yml`. To get the digest, run:

```powershell
gh api repos/obsproject/obs-studio/releases/tags/<version> --jq '.assets[] | select(.name | endswith("-Windows-x64.zip")) | .digest'
```
