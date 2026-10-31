# Publishing a release

The release must contain **only the DLL** as an asset.

1. Build: run `BUILD_WINDOWS.cmd` (Zig 0.15.2). Result: `dist\NitroSystemII.dll`.
2. Test the DLL in game once (platform `fh2-mods\platform.log` should show the NitroSystem II lines without errors).
3. Make sure the version in the source, README, CHANGELOG.md and the tag agree (current: `2.0.0`). The platform's update check compares the GitHub release tag (a leading `v` is ignored) with the version the mod registers.
4. Tag and publish:

```sh
git tag v2.0.0
git push origin v2.0.0
gh release create v2.0.0 dist/NitroSystemII.dll --title "NitroSystem II 2.0.0" --notes-file release-notes.md
```

`release-notes.md` can be the matching section of [CHANGELOG.md](CHANGELOG.md) plus the requirements line from the README. Do not attach source archives, ini files or anything else; GitHub adds the source zip by itself.

The DLL is build output and is listed in `.gitignore`; it is never committed.
