## What this changes

<!-- One or two sentences. For a pack: its name and what the sounds are. For a plugin: its name, kind and firmware. -->

## For a patch pack

<!-- Delete this section if the pull request does not add or change a pack. -->

- [ ] I made these sounds myself, or I have the right to share them under the licence in `pack.json`.
- [ ] No samples come from commercial sample packs, records, films or other people's patches (unless their licence allows redistribution under the chosen licence).
- [ ] I chose one of the five Creative Commons licences listed in `docs/contributing-packs.md`.
- [ ] `pnpm --filter @co-op/web packs:validate` passes (or the pull request check is green).
- [ ] I tried the patches on an OP-1 running the firmware listed in `pack.json`.

## For a plugin

<!-- Delete this section if the pull request does not add or change a plugin. See docs/contributing-plugins.md. -->

- [ ] I wrote this plugin, or I have the right to share it under the licence in `plugin.json`, and any code it ports is credited in `credits`.
- [ ] `source` links to the source, and I say below which commit the file was built from and how.
- [ ] I understand a plugin runs as firmware code, and that maintainers merge only from authors they trust or with source they have looked at.
- [ ] `manual.md` follows `docs/plugin-manual-prompt.md`, and I checked every line against how the plugin behaves.
- [ ] `pnpm --filter @co-op/web plugins:validate` passes (or the pull request check is green).
- [ ] I installed and ran the plugin on an OP-1 with the firmware in `requires`, and it starts again cleanly after a restart.
- [ ] For an update: `version` is bumped.

## For code changes

<!-- Delete this section for a pack-only or plugin-only pull request. -->

- [ ] `pnpm lint && pnpm typecheck && pnpm test` passes.
- [ ] New behaviour has tests; UI changes are covered by the Playwright test where it makes sense (`pnpm --filter @co-op/web e2e`).
- [ ] Patch files are written with their original bytes unless the change is a metadata edit.
- [ ] Docs (README, spec or plans) are updated if behaviour changed.
