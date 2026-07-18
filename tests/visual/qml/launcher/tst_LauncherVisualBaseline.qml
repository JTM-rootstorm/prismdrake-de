import QtQuick
import QtTest
import "../../../../src/shell/launcher/qml" as Launcher

// The Quick Test setup publishes real settings/theme/launcher adapters and the recorder.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "LauncherVisualBaseline"
    when: windowShown
    width: 780
    height: 720
    visible: true

    Launcher.LauncherSurface {
        id: surface
        x: 10
        y: 10
        width: 740
        height: 680
        themeGeneration: launcherFixture.themeGeneration
        launcherModel: launcherFixture.launcherModel
    }

    function capture(name) {
        let completed = false
        let recorded = false
        const accepted = surface.grabToImage(result => {
            const saved = result.saveToFile(baselineRecorder.imagePath(name))
            recorded = saved && baselineRecorder.record(
                name, surface.themeGeneration, surface.width, surface.height,
                surface.Screen.devicePixelRatio, "ltr", true, false)
            completed = true
        })
        verify(accepted)
        tryVerify(() => completed, 5000)
        verify(recorded, baselineRecorder.lastError)
        verify(baselineRecorder.metadataComplete(name))
    }

    function initTestCase() {
        failOnWarning(/.*/)
        verify(baselineRecorder.expectedFontAvailable())
    }

    function test_captureSharedResultsEmptyAndFallbackStates() {
        verify(launcherFixture.resetLustre())
        verify(launcherFixture.publishRepresentativeResults())
        tryCompare(surface, "viewState", "results")
        tryCompare(surface, "resultCount", 3)
        surface.focusResult(0, false)
        tryCompare(surface.resultAt(0), "activeFocus", true)
        capture("launcher-lustre-results")
        capture("launcher-lustre-results-repeat")
        verify(baselineRecorder.imagesEqual(
            "launcher-lustre-results", "launcher-lustre-results-repeat"))

        const sharedTree = surface
        verify(launcherFixture.publishEmptyCatalog())
        tryCompare(surface, "viewState", "emptyCatalog")
        surface.focusSearch()
        capture("launcher-lustre-empty")

        verify(launcherFixture.publishRepresentativeResults())
        verify(launcherFixture.publishNoResults())
        tryCompare(surface, "viewState", "noResults")
        tryCompare(surface, "resultCount", 0)
        surface.focusSearch()
        capture("launcher-lustre-no-results")

        verify(launcherFixture.publishForge())
        verify(launcherFixture.publishRepresentativeResults())
        tryCompare(surface.themeGeneration, "profileId", "forge")
        tryCompare(surface, "viewState", "results")
        compare(surface, sharedTree)
        surface.focusResult(1, false)
        capture("launcher-forge-results")

        verify(launcherFixture.resetAccessible())
        verify(launcherFixture.publishRepresentativeResults())
        tryCompare(surface.themeGeneration, "highContrast", true)
        compare(surface.themeGeneration.reducedMotion, true)
        compare(surface.themeGeneration.transparencyDisabled, true)
        compare(surface.opaqueFallbackActive, true)
        surface.focusSearch()
        capture("launcher-accessible-fallback")
    }
}

// qmllint enable unqualified
