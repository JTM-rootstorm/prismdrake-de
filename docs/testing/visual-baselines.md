# Deterministic visual baseline candidates

PD1-WP13 provides candidate-capture lanes around the production panel, launcher,
and notification QML trees. They render real settings/theme projections and the
production task, launcher, and notification presentation adapters without
creating profile-specific component forks. The harness does not capture or blur
desktop content.

The current candidates cover:

- panel: Lustre, Forge, accessibility, missing blur, and right-to-left layout;
- launcher: Lustre results, empty catalog, and a non-empty catalog with no query
  results, Forge results, and the accessible opaque fallback; and
- notifications: Lustre normal urgency, Forge critical urgency, and critical
  urgency with the accessible opaque fallback.

The launcher lane captures the same Lustre results state twice and requires
byte-identical decoded images. This guards the locked renderer contract before
any reviewed tolerance policy is introduced.

Configure, build, and run the focused lane with:

```bash
cmake -S . -B build/visual -DCMAKE_BUILD_TYPE=Debug
cmake --build build/visual --target \
  prismdrake-panel-visual-baseline-tests \
  prismdrake-launcher-visual-baseline-tests \
  prismdrake-notification-visual-baseline-tests
ctest --test-dir build/visual \
  -R 'PanelVisualBaselineTest|LauncherVisualBaselineTest|NotificationVisualBaselineTest' \
  --output-on-failure
```

The CTest definition fixes the offscreen QPA, software scene graph, basic render
loop, Basic controls style, scale factor, C locale, and UTC timezone. Configure
time locks the theme's generic `sans-serif` request to the family and source
reported by fontconfig. The tests fail if Qt cannot resolve that same family at
render time, and each sidecar records both values for review. Generated PNGs
and JSON sidecars are written below `build/visual/test-artifacts/visual/`.

Each sidecar follows
[`baseline-metadata.schema.json`](../../tests/visual/baseline-metadata.schema.json)
and records the profile, settings generation, complete theme-generation SHA-256,
capabilities, transparency, motion, contrast, text scale, locale, direction,
output geometry and scale, rendering backend, font resolution, Qt version, test
name, and encoded-image SHA-256.

These outputs are deliberately marked `candidate`. They make review artifacts
reproducible but are not approved golden images yet. Promoting a candidate to an
approved baseline requires maintainer visual review on the documented reference
guest. Later image comparison must use a separately reviewed narrow tolerance;
the harness must never rewrite approved images merely to make CI pass.

This slice exercises `PD-THEME-002`, `PD-THEME-003`, `PD-THEME-008`,
`PD-A11Y-002`, `PD-A11Y-004` through `PD-A11Y-010`, `PD-PERF-009`, and
`PD-TEST-006`. The production-shell AT-SPI procedure is documented separately;
approved cross-version image-diff policy remains open WP13 work.
