# Working in this repo

- Read `CONTEXT.md` first: the project's state, decisions and open items. Update it whenever a milestone, decision or measurement changes.
- `NOTES.private.md` and the project brief `taiko-pocket-prompt.md` are local files, not tracked by git. Read them too if they are there: the brief has the milestones and constraints, the notes have what concerns the owner's own device, library and charts. That kind of note goes there, never into a tracked file.
- **Stay inside this folder.** No files anywhere else (no `/tmp`, no home-directory caches). Build output and caches go to `build/`, downloads to `third_party/`. Installing dependencies with Homebrew is fine.
- Never commit anything from `taikojiro_reference/`, `taikopocket_library/` (charts and audio) or `third_party/`.
- `skins/taikojiro-original/` is the game's default skin and is part of the repository and of the releases. Keep its credit in `skins/README.md`.
- Docs are short and factual. One topic per file under `docs/`.
- Gameplay behaviour follows YataiDON (`third_party/YataiDON`); deviations are listed in `docs/tja-notes.md`.
- `make test` must pass before a commit.
