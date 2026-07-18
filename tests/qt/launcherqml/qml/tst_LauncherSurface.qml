import QtQuick
import QtTest
import "../../../../src/shell/launcher/qml" as Launcher

// The Quick Test setup publishes real theme and launcher adapters before loading this file.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "LauncherSurfaceRealAdapters"
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
        onSearchRequested: query => launcherFixture.captureSearch(query)
    }

    SignalSpy { id: dismissed; target: surface; signalName: "dismissRequested" }
    SignalSpy { id: forwardExit; target: surface; signalName: "focusExitForward" }
    SignalSpy { id: backwardExit; target: surface; signalName: "focusExitBackward" }

    function search() {
        return findChild(surface, "launcherSearchField")
    }

    function stateLabel() {
        return findChild(surface, "launcherStateLabel")
    }

    function initTestCase() {
        failOnWarning(/.*/)
    }

    function init() {
        verify(launcherFixture.resetLustre())
        verify(launcherFixture.publishRepresentativeResults())
        surface.layoutDirection = Qt.LeftToRight
        dismissed.clear()
        forwardExit.clear()
        backwardExit.clear()
        tryCompare(surface, "viewState", "results")
        tryCompare(surface, "resultCount", 3)
    }

    function test_realProfilesShareTreeAndLauncherGeneration() {
        const original = surface
        compare(surface.themeGeneration.profileId, "lustre")
        compare(surface.tokens, surface.themeGeneration.launcher)
        compare(surface.tokens.tileRadius, 11)
        compare(surface.tokens.fallbackActive, false)
        verify(surface.tokens.blurRequested)

        verify(launcherFixture.publishForge())
        tryCompare(surface.themeGeneration, "profileId", "forge")
        compare(surface, original)
        compare(surface.tokens.tileRadius, 4)
        compare(surface.tokens.fallbackActive, true)
        compare(surface.tokens.blurRequested, false)
    }

    function test_accessibleFallbackTargetsAndPlainUntrustedText() {
        verify(launcherFixture.resetAccessible())
        tryCompare(surface.tokens, "minimumTargetSize", 48)
        tryCompare(surface.tokens, "reducedMotion", true)
        tryCompare(surface, "opaqueFallbackActive", true)
        verify(launcherFixture.publishLongResult())
        tryCompare(surface, "resultCount", 1)
        const result = surface.resultAt(0)
        verify(result !== null)
        verify(result.width >= surface.tokens.minimumTargetSize)
        verify(result.height >= surface.tokens.minimumTargetSize)
        verify(search().height >= surface.tokens.minimumTargetSize)
        compare(surface.motionDuration, 0)
        compare(surface.tokens.surfaceColor.a, 1)
        compare(result.Accessible.role, Accessible.Button)
        verify(result.Accessible.name.length > 1000)
        verify(result.Accessible.description.indexOf("<b>utility</b>") >= 0)
        const genericName = findChild(result, "launcherResultGenericName")
        const comment = findChild(result, "launcherResultComment")
        const terminal = findChild(result, "launcherResultTerminalState")
        compare(genericName.textFormat, Text.PlainText)
        compare(genericName.text, "Literal <b>utility</b>")
        compare(comment.textFormat, Text.PlainText)
        compare(comment.text, "Untrusted <script>plain text</script> & metadata")
        compare(terminal.visible, true)
        compare(terminal.text, "Requires a terminal")
        verify(result.Accessible.description.indexOf("Requires a terminal") >= 0)
        verify(result.Accessible.description.indexOf("private-command") < 0)
        verify(result.Accessible.description.indexOf("/fixture/private") < 0)
    }

    function test_explicitLoadingEmptyNoResultsErrorAndInvalidRetention() {
        verify(launcherFixture.publishLoading())
        tryCompare(surface, "viewState", "loading")
        compare(stateLabel().text, "Loading applications")
        compare(findChild(surface, "launcherBusyIndicator").visible, true)

        verify(launcherFixture.publishNoResults())
        tryCompare(surface, "viewState", "noResults")
        compare(stateLabel().text, "No matching applications")
        tryCompare(surface, "resultCount", 0)

        verify(launcherFixture.rejectInvalidPublication())
        compare(surface.viewState, "noResults")
        compare(stateLabel().text, "No matching applications")

        verify(launcherFixture.publishEmptyCatalog())
        tryCompare(surface, "viewState", "emptyCatalog")
        compare(stateLabel().text, "No applications are available")

        verify(launcherFixture.publishError())
        tryCompare(surface, "viewState", "error")
        compare(stateLabel().text, "Application search is unavailable")
        compare(stateLabel().font.bold, true)
    }

    function test_boundedSearchKeyboardActivationAndFocusExit() {
        const field = search()
        compare(field.maximumLength, 256)
        field.text = "x".repeat(400)
        verify(field.text.length <= 256)
        field.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        tryCompare(launcherFixture, "capturedSearch", field.text)
        tryCompare(launcherFixture, "launchCount", 1)
        compare(launcherFixture.lastLaunchedName, "Dragon Editor")

        surface.focusSearch()
        tryCompare(field, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(surface.resultAt(0), "activeFocus", true)
        keyClick(Qt.Key_Down)
        tryCompare(surface.resultAt(1), "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(surface.resultAt(0), "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(field, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(backwardExit, "count", 1)

        surface.resultAt(2).forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Tab)
        tryCompare(forwardExit, "count", 1)
        surface.focusSearch()
        keyClick(Qt.Key_Escape)
        tryCompare(dismissed, "count", 1)
    }

    function test_reorderRemovalAndStateChangeRecoverFocus() {
        const focused = surface.resultAt(0)
        const focusedPresentation = focused.presentation
        surface.focusResult(0, false)
        tryCompare(focused, "activeFocus", true)
        compare(focused.presentationName, "Dragon Editor")

        verify(launcherFixture.publishReorderedResults())
        tryVerify(function() {
            const result = surface.resultAt(1)
            return result !== null && result.activeFocus
        })
        compare(surface.resultAt(1).presentation, focusedPresentation)
        compare(surface.resultAt(1).presentationName, "Dragon Editor")

        verify(launcherFixture.removeResult(1))
        tryCompare(surface, "resultCount", 2)
        tryVerify(function() {
            const result = surface.resultAt(1)
            return result !== null && result.activeFocus
                    && result.presentationName === "Terminal"
        }, 5000, "pending=" + surface.pendingFocusIndex
                 + " ready=" + surface.focusRecoveryReady
                 + " first=" + surface.resultAt(0).presentationName
                 + ":" + surface.resultAt(0).activeFocus
                 + " second=" + surface.resultAt(1).presentationName
                 + ":" + surface.resultAt(1).activeFocus
                 + " search=" + search().activeFocus)

        verify(launcherFixture.publishNoResults())
        tryCompare(search(), "activeFocus", true)
    }

    function test_rightToLeftDirectionPreservesKeyboardAndAccessibility() {
        surface.layoutDirection = Qt.RightToLeft
        compare(surface.LayoutMirroring.enabled, true)
        compare(surface.Accessible.role, Accessible.Pane)
        compare(surface.Accessible.name, "Applications")
        compare(search().Accessible.role, Accessible.EditableText)
        compare(search().Accessible.name, "Search applications")
        surface.focusSearch()
        keyClick(Qt.Key_Down)
        tryCompare(surface.resultAt(0), "activeFocus", true)
        verify(surface.resultAt(0).Accessible.name.length > 0)
    }
}

// qmllint enable unqualified
