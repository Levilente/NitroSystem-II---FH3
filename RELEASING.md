# Publishing a release

The release must contain **only the DLL** as an asset.

1. Build: `bash scripts/build.sh` (llvm-mingw, Linux/macOS) or `BUILD_WINDOWS.cmd` (Zig). Result: `dist/NitroSystemII.dll`.
2. Test the DLL in game once (the platform's `fh3-mods\platform.log` should show the NitroSystem II lines without errors; buy a kit, boost).
3. Make sure the version in `src/nitro_mod.cpp` (`info.version`), `BUILD_WINDOWS.cmd`, `scripts/build.sh`, CHANGELOG.md and the tag agree (current: `1.0.0`). The platform's update check compares the GitHub release tag (a leading `v` is ignored) with the version the mod registers.
4. Tag and publish:

```sh
git tag v1.0.0
git push origin v1.0.0
gh release create v1.0.0 dist/NitroSystemII.dll --title "NitroSystem II for FH3 1.0.0" --notes-file release-notes.md
```

`release-notes.md` can be the matching section of [CHANGELOG.md](CHANGELOG.md) plus the requirements line from the README (FH3 Mod Platform 1.0.7 or newer). Do not attach source archives, ini files or anything else; GitHub adds the source zip by itself.

The DLL is build output and is listed in `.gitignore`; it is never committed.
