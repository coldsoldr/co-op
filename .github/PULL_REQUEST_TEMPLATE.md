## What this changes

<!-- One or two sentences. For a pack: its name and what the sounds are. -->

## For a patch pack

<!-- Delete this section if the pull request does not add or change a pack. -->

- [ ] I made these sounds myself, or I have the right to share them under the licence in `pack.json`.
- [ ] No samples come from commercial sample packs, records, films or other people's patches (unless their licence allows redistribution under the chosen licence).
- [ ] I chose one of the five Creative Commons licences listed in `docs/contributing-packs.md`.
- [ ] `pnpm --filter @co-op/web packs:validate` passes (or the pull request check is green).
- [ ] I tried the patches on an OP-1 running the firmware listed in `pack.json`.

## For code changes

<!-- Delete this section for a pack-only pull request. -->

- [ ] `pnpm lint && pnpm typecheck && pnpm test` passes.
- [ ] New behaviour has tests; UI changes are covered by the Playwright test where it makes sense (`pnpm --filter @co-op/web e2e`).
- [ ] Patch files are written with their original bytes unless the change is a metadata edit.
- [ ] Docs (README, spec or plans) are updated if behaviour changed.
