# Release guide for plugin authors

co-op lists OP-1 plugins under the people who made them and links to their own repos. It hosts no copy. If your plugin is listed, or you'd like it to be, this guide shows how to publish updates so co-op picks them up by itself.

## How co-op follows your plugin

Every day, co-op checks the GitHub repo of each plugin it lists:

- **A hundekraft plugin with its `.knl` in the repo:** when a new commit changes that file, co-op downloads it and checks it is a valid kennel plugin with the same saved name and kind. It then pins the new commit, its SHA-256 and its size, and takes the version from the file's header.
- **Any other plugin:** when you publish a GitHub release with a higher version than the one listed, its tag becomes the listed version.

Each update becomes a pull request on co-op. A maintainer looks at what changed, the commit diff or your release notes, and merges it. Your listing then updates on the site. Nothing changes on co-op without that review: co-op's Install over USB button sends your file to people's OP-1s as firmware code, so a person always looks first.

## What your repo needs

1. **A public GitHub repo** with a README that says what the plugin does, who made it, which firmware and version it needs, and what each control does. co-op's manual for your plugin is written from it.
2. **A licence file.** GPL-3.0-or-later with the OP-1 firmware permission is common for kennel plugins.
3. **The built file committed in the repo**, at a path that stays the same from release to release:
   - hundekraft: `<id>.knl`, named after its saved name (spaces as `_`), for example `build/erdtree.knl`;
   - opus: your `.opmod`.
4. **A picture of the plugin's screen** in the README, an OP-1 capture or your host build's render.

Why commit the file, not only attach it to a release: browsers can't download GitHub release attachments from another site, so co-op's Install over USB can't fetch them. A file in the repo can be fetched. You can still attach it to your releases too.

## Publishing an update

1. Build the new version, with its version number set in the plugin's metadata so the built `.knl` carries it: co-op reads the version from the file's header.
2. Commit the new file at the same path, replacing the old one.
3. Publish a GitHub release whose tag is the version: `1.2.0` or `v1.2.0`. Versions must go up; co-op never lists a lower one.
4. Write release notes: they go into co-op's pull request, and they help the maintainer review it.

Mark betas and test builds as **pre-releases**. co-op ignores pre-releases, so testers can try them without the listing moving.

## Things that hold an update back

co-op leaves your listing as it is, and tells the maintainer why, when:

- the new `.knl` isn't a valid kennel plugin;
- its saved name or kind differs from the listing's (a different plugin under the same path);
- its version is lower than the listed one;
- the file was moved or removed. If you move it, open an issue on co-op with the new path.

Don't move or re-point release tags after publishing: a tag is meant to name one version.

## When the controls change

The daily check updates the file, its version and its pin. It doesn't rewrite the manual. If your update adds or changes controls, update your README and say so in the release notes; the maintainer re-runs `/add-plugin` to rewrite the manual. You can also do it yourself in a pull request.

## Getting listed

From co-op's Plugins tab, choose **Submit a plugin**, pick your firmware, and follow its steps. With Claude Code, run `/add-plugin <your repo>` in a fork of co-op and open a pull request. Without it, offer your plugin in an issue with your repo's link.

## Checklist

- Public GitHub repo, README with credits, controls and requirements
- Licence file
- The built `.knl` or `.opmod` committed, always at the same path
- The version set inside the `.knl`
- A GitHub release per version, tags that only go up, betas as pre-releases
- A screenshot in the README
