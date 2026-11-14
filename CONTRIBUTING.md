# Contributing to Nudge

Bug reports, small improvements, and translations are welcome. Open an
[issue](https://github.com/willykev/nudge/issues) to discuss a larger change first.

## Build and test

On Arch Linux, install `base-devel`, `qt6-base`, `qt6-declarative`, `qt6-svg`,
and `qt6-wayland`.

```sh
bin/build
bin/test
build/nudge open
```

The tests run offscreen with temporary data. Keep changes focused, add a
regression test for a bug fix, and describe how you checked the result in
your pull request. Follow the surrounding C++ and QML style.

The [full guide](docs/guide.md) covers the app's behavior and data format.
Contributions use the project's [MIT license](LICENSE).

## Releases

Update `VERSION`, `pkgver` in `pkgbuild/PKGBUILD`, and `CHANGELOG.md` together.
Write `docs/releases/v<VERSION>.md` with the changes and installation notes.
Then build, test, commit, and create an annotated tag:

```sh
release_version=$(cat VERSION)
git tag -a "v$release_version" -m "Release v$release_version"
bin/release
```

`bin/release` writes the tagged source archive and the standalone `PKGBUILD`
that downloads it to `dist/v<VERSION>/`, builds the Arch package from that
`PKGBUILD` and archive, runs the tests, and writes `SHA256SUMS`. It needs
Python 3.12 or newer and Arch's `makepkg`. Packages are built for the current
machine's architecture.

Maintainers can run `bin/release --publish` to push the commit and tag, set
the GitHub description and topics, and publish the release with its assets.
It needs the GitHub CLI, signed in with admin access to the repository.
Before it pushes anything, it checks the release notes, the tag, GitHub
access, and that the push will be accepted. Verified local assets are reused
on retries, and assets already on the release must match them: one that
differs stops the release before the push, so delete it from the release or
restore the uploaded file, then retry.
