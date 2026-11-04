# G.O.R.E: Morrowind 1.0 — validation exception

The Windows engine was compiled once from `31b0943e93241a2b5c7aa4a9ee15b71d5d2f10dc` in [run 41793776050](https://github.com/vault13xcv-star/4656/actions/runs/41793776050). Its x64 executable, installed runtime, launchers, resources and archive integrity checks passed. The release retains that executable and every original runtime file unchanged; only this notice and provenance metadata are added.

- Windows physics/render/combat regression groups: **5/5 passed**.
- Windows component tests: **1404/1405 passed**. `SettingsValuesTest.physicsLayoutBindsEverySliderToHalfAndTripleItsActualDefault` assumed every Physics control was a numeric slider. It attempted to look up the new `kill slow motion` boolean as a float.
- The source correction distinguishes sliders from checkboxes and checks the boolean's default, off value and reset using the same default-to-user initialization as the engine. The **17 settings tests passed on Linux** after that correction. This is a targeted rerun, not a passing rerun of all Windows tests.
- The Windows native integration test executable compiled but **was not executed**: the component failure stopped the combined test step first. In particular, the new native DateTimeManager test is not reported as passing. The state-machine checks for cancelling active/queued slow motion are covered by the passing regression group.
- The original `VALIDATION.txt` and bundled logs correctly retain the failed test-step result. Packaging and publication do not turn that result into a passing test run.

The publication commit changes tests, documentation and release machinery only. Engine and resource sources are identical to the compiled commit. `RELEASE-INFO.json` and `package.json` identify both commits and the original archive hash; the source archive contains the corrected tests. A full engine rebuild was deliberately avoided for this late test-only issue.

There was no gameplay run with Morrowind data, no measured in-game FPS test, and no promise of compatibility with every model/animation mod.
