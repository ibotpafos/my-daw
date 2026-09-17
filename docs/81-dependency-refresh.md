# 81. Dependency refresh — 2026-09-16

## Scope

This pass refreshes every dependency that is directly pinned by the repository,
without replacing an upstream project's own submodule choices. The distinction
matters for the Tracktion spike: My DAW pins Tracktion's `develop` commit, while
JUCE is consumed exactly at the submodule revision selected by that Tracktion
commit.

## Updated

| Dependency | Before | After | Notes |
|---|---|---|---|
| Tracktion Engine | `ff794da4f58e732528b06d7799dad087635de0cb` | `00fe42753a995c79dd857efe69dde17550e27e78` | Current `develop` HEAD on 2026-09-16; spike-only |
| `actions/checkout` | v4.2.2 / `11bd719…` | v7.0.1 / `3d3c42e5aac5ba805825da76410c181273ba90b1` | Full commit pin retained |
| `jsonschema` | 4.25.1 | 4.26.0 | Documentation/schema tooling only |
| Python-Markdown | 3.8.2 | 3.10.3 | Documentation tooling only |

## Already current

- VST3 SDK stays at `3.8.1_build_84`, commit
  `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`.
- DAWproject stays at current upstream `main`, commit
  `ee4dcdde75940f30e14e55401a26955a58b8322b`.
- `attrs 26.1.0`, `jsonschema-specifications 2025.9.1`,
  `referencing 0.37.0` and `rpds-py 2026.6.3` were already current in the
  documentation environment.

## Why JUCE is not forced to 9.x

JUCE has newer standalone releases, but it is not a direct production dependency
of My DAW. It is used only through Tracktion's spike-only submodule. The refreshed
Tracktion `develop` commit still pins JUCE to
`37c894f83d379179b2070d437ccd0f1cd9af9576`, so My DAW keeps that exact revision.
Overriding it independently would stop reproducing the upstream Tracktion tree
and would turn a dependency refresh into an unreviewed compatibility fork.

## Ongoing maintenance

`.github/dependabot.yml` now checks GitHub Actions and the root pip requirements
weekly. Native SDKs with exact git/submodule pins remain deliberate manual
updates because they require compatibility testing, license review and—in the
VST3 case—matching submodule revisions.

The Tracktion `develop` entry is pinned by git commit. Unlike stable archived
SDK entries, this moving spike pin no longer carries a stale source-archive
SHA-256 from the previous commit; the exact commit plus Tracktion's pinned JUCE
submodule are the reproducibility boundary used by the documented build flow.
