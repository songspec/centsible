# Centsible: notes for coding agents

Build, test and design notes are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). This file covers what an agent needs that isn't there, starting with releases.

## Releasing

Releases are published to **mainstage** (https://mainstage.songspec.org/centsible), SongSpec's download site. songspec.org/centsible reads the live version from there. Releases are made by the manual GitHub Actions workflow `.github/workflows/release.yml`. It never runs on push.

When someone asks for a release, do all of this, in order:

1. **Pick the version.** It lives in one place: `project(Centsible VERSION x.y.z)` in `CMakeLists.txt`. Bump it with semver (fixes = patch, features = minor). A published version number can never be reused. To see what is live:

   ```bash
   curl -s https://mainstage.songspec.org/api/projects/centsible | jq '.live.version'
   ```

   Also update the version badge near the top of `README.md`. The installer takes its version from CMakeLists.txt on its own.
2. **Write the release notes** in `docs/release-notes-<version>.md`, following `docs/release-notes-1.3.0.md`. Write them for the people who use the plugin, not for developers. Summarise what changed since the last tag (`git log v<previous>..HEAD --oneline`), and add an entry to `CHANGELOG.md`. The workflow refuses to run without the notes file.
3. **Commit and push to `main`.** Pushing doesn't build or release anything.
4. **Run the workflow** and wait for it (about 20 minutes):

   ```bash
   gh workflow run release.yml -R songspec/centsible -f mode=live
   sleep 5
   gh run watch -R songspec/centsible $(gh run list -R songspec/centsible --workflow release.yml --limit 1 --json databaseId --jq '.[0].databaseId') --exit-status
   ```

   It checks mainstage first, then builds, runs the test subset, makes the installer, tags the commit `v<version>`, and publishes `Centsible-Setup-<version>.exe` as the live version.
5. **Confirm it is live:** `curl -s https://mainstage.songspec.org/api/projects/centsible | jq '.live.version'` should print the new version. Report the version, the run URL and https://mainstage.songspec.org/centsible back to the person who asked.

Workflow inputs:

| input | values | use |
|---|---|---|
| `mode` | `live` (default), `publish`, `draft` | `publish` adds it to the archive without changing the download everyone gets. `draft` leaves it unpublished, for someone to check on mainstage first. |
| `existing` | `fail` (default), `fill` | `fill` only adds files a published version is missing. Use it when a release went out incomplete. It never replaces files that are already there. |
| `dry_run` | `false` (default), `true` | Builds and tests everything, checks the token, publishes nothing and pushes no tag. |

If it fails:

- **"already published":** the version wasn't bumped. Go back to step 1.
- **"release notes ... missing":** do step 2.
- **"release token isn't valid":** the `MAINSTAGE_RELEASE_TOKEN` repository secret is wrong or was revoked. A SongSpec admin makes a new one in the project's settings on mainstage (Release tokens). Don't try to work around it.
- **Build or test failures:** fix the code and push again. Running the workflow again for the same version is safe. A draft is updated in place, and the tag is only created once.

Never upload releases by hand, and never publish to GitHub Releases. Mainstage is the only release channel. `.github/scripts/mainstage-release.mjs` is a copy of `tools/mainstage-release.mjs` from songspec/mainstage. Update it from there rather than editing it here.
