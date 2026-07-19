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
    SignalSpy { id: refreshed; target: surface.launcherModel; signalName: "refreshRequested" }

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
        refreshed.clear()
        tryCompare(surface, "viewState", "results")
        tryCompare(surface, "resultCount", 3)
    }

    function test_explicitRefreshAndOpaqueIconPresentation() {
        const refresh = findChild(surface, "launcherRefreshButton")
        verify(refresh !== null)
        verify(refresh.width >= surface.tokens.minimumTargetSize)
        verify(refresh.height >= surface.tokens.minimumTargetSize)
        mouseClick(refresh)
        tryCompare(refreshed, "count", 1)
        refresh.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Space)
        tryCompare(refreshed, "count", 2)

        const result = surface.resultAt(0)
        verify(result !== null)
        const icon = findChild(result, "launcherResultIcon")
        verify(icon !== null)
        verify(icon.source.toString().indexOf("private") < 0)
        verify(result.presentation.iconKey.indexOf("desktop") < 0)
    }

    function test_applicationActionsAreKeyboardReachable() {
        launcherFixture.captureSearch("dragon editor")
        verify(launcherFixture.publishCapturedSearch())
        tryCompare(surface, "resultCount", 2)
        let result = null
        let resultIndex = -1
        for (let index = 0; index < surface.resultCount; ++index) {
            const candidate = surface.resultAt(index)
            if (candidate !== null && candidate.presentationName === "Dragon Editor") {
                result = candidate
                resultIndex = index
                break
            }
        }
        verify(result !== null)
        compare(result.presentation.actionCount, 1)
        compare(result.Accessible.selectable, true)
        surface.focusResult(resultIndex, false)
        compare(result.Accessible.selected, true)
        result.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)

        const menu = findChild(result, "launcherActionsMenu")
        verify(menu !== null)
        tryCompare(menu, "visible", true)
        const action = result.actionMenuItemAt(0)
        const pin = result.pinMenuItem()
        verify(action !== null)
        verify(pin !== null)
        compare(action.Accessible.role, Accessible.MenuItem)
        verify(action.Accessible.name.length > 0)
        verify(action.Accessible.description.indexOf(result.presentationName) >= 0)
        verify(action.width >= surface.tokens.minimumTargetSize)
        verify(action.height >= surface.tokens.minimumTargetSize)
        compare(pin.Accessible.role, Accessible.MenuItem)
        compare(pin.Accessible.checkable, true)
        compare(pin.Accessible.checked, result.pinned)
        verify(pin.Accessible.description.length > 0)
        tryCompare(action, "activeFocus", true)
        keyClick(Qt.Key_Down)
        tryCompare(pin, "activeFocus", true)
        keyClick(Qt.Key_Up)
        tryCompare(action, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(pin, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(action, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
        tryCompare(result, "activeFocus", true)
        keyClick(Qt.Key_Menu)
        tryCompare(menu, "visible", true)
        tryCompare(action, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
        tryCompare(result, "activeFocus", true)
    }

    function test_actionMenuBoundsLongLabelsAndScrollsInRightToLeft() {
        surface.layoutDirection = Qt.RightToLeft
        verify(launcherFixture.publishOverflowActions())
        tryCompare(surface, "resultCount", 1)
        const result = surface.resultAt(0)
        compare(result.presentation.actionCount, 64)
        result.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)

        const menu = findChild(result, "launcherActionsMenu")
        const viewport = result.actionMenuViewport()
        tryCompare(menu, "visible", true)
        verify(viewport !== null)
        verify(menu.width <= testCase.width - (2 * surface.tokens.tilePadding))
        verify(menu.height <= testCase.height - (2 * surface.tokens.tilePadding))
        const viewportOrigin = viewport.mapToItem(testCase, 0, 0)
        verify(viewportOrigin.x >= 0)
        verify(viewportOrigin.y >= 0)
        verify(viewportOrigin.x + viewport.width <= testCase.width)
        verify(viewportOrigin.y + viewport.height <= testCase.height)
        tryVerify(function() { return viewport.contentHeight > viewport.availableHeight })

        const first = result.actionMenuItemAt(0)
        const firstLabel = findChild(first, "launcherActionMenuLabel0")
        verify(first !== null)
        verify(firstLabel !== null)
        compare(first.Accessible.name.length, 65536)
        compare(firstLabel.text.length, first.Accessible.name.length)
        compare(firstLabel.textFormat, Text.PlainText)
        compare(firstLabel.elide, Text.ElideRight)
        verify(firstLabel.width <= viewport.availableWidth)
        tryCompare(firstLabel, "truncated", true)
        compare(first.LayoutMirroring.enabled, true)

        for (let index = 1; index < 64; ++index)
            keyClick(Qt.Key_Down)
        const last = result.actionMenuItemAt(63)
        tryCompare(last, "activeFocus", true)
        tryVerify(function() { return viewport.contentItem.contentY > 0 })
        keyClick(Qt.Key_Down)
        tryCompare(result.pinMenuItem(), "activeFocus", true)
        keyClick(Qt.Key_Down)
        tryCompare(first, "activeFocus", true)
        tryCompare(viewport.contentItem, "contentY", 0)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
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
        compare(surface.themeGeneration.highContrast, true)
        verify(surface.tokens.bodyFontPixels > 14)
        verify(surface.tokens.titleFontPixels > 17)
        compare(findChild(surface, "launcherHeading").font.pixelSize,
                Math.round(surface.tokens.titleFontPixels))
        compare(search().font.pixelSize, Math.round(surface.tokens.bodyFontPixels))
        verify(launcherFixture.publishLongResult())
        tryCompare(surface, "resultCount", 1)
        const result = surface.resultAt(0)
        verify(result !== null)
        verify(result.width >= surface.tokens.minimumTargetSize)
        verify(result.height >= surface.tokens.minimumTargetSize)
        verify(result.width <= surface.width)
        verify(result.mapToItem(surface, 0, 0).x >= 0)
        verify(result.mapToItem(surface, result.width, 0).x <= surface.width)
        const title = findChild(result, "launcherResultTitle")
        verify(title !== null)
        verify(title.width <= result.width)
        compare(title.truncated, true)
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
        compare(genericName.font.pixelSize, surface.tokens.bodyFontPixels)
        verify(genericName.width <= result.width)
        compare(comment.textFormat, Text.PlainText)
        compare(comment.text, "Untrusted <script>plain text</script> & metadata")
        compare(comment.font.pixelSize, surface.tokens.bodyFontPixels)
        verify(comment.width <= result.width)
        compare(terminal.visible, true)
        compare(terminal.text, "Requires a terminal")
        verify(result.Accessible.description.indexOf("Terminal required: Yes") >= 0)
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
        search().forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        compare(launcherFixture.launchCount, 0)

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
        field.text = ""
        field.forceActiveFocus(Qt.OtherFocusReason)
        const query = "dragon editor"
        for (let index = 0; index < query.length; ++index) {
            const character = query[index]
            keyClick(character === " " ? Qt.Key_Space
                                         : character.toUpperCase().charCodeAt(0))
        }
        tryCompare(launcherFixture, "capturedSearch", field.text)
        compare(surface.searchPending, true)
        keyClick(Qt.Key_Return)
        compare(launcherFixture.launchCount, 0)
        verify(launcherFixture.publishCapturedSearch())
        tryCompare(surface, "searchPending", false)
        tryCompare(surface, "resultCount", 2)
        compare(surface.resultAt(0).presentationName, "Dragon Editor")
        keyClick(Qt.Key_Return)
        tryCompare(launcherFixture, "launchCount", 1)
        compare(launcherFixture.lastLaunchedName, "Dragon Editor")
        surface.resultAt(0).Accessible.pressAction()
        tryCompare(launcherFixture, "launchCount", 2)
        compare(launcherFixture.lastLaunchedName, "Dragon Editor")
        surface.resultAt(0).forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Space)
        tryCompare(launcherFixture, "launchCount", 3)
        compare(launcherFixture.lastLaunchedName, "Dragon Editor")

        verify(launcherFixture.publishRepresentativeResults())
        tryCompare(surface, "resultCount", 3)
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
        const refresh = findChild(surface, "launcherRefreshButton")
        tryCompare(refresh, "activeFocus", true)
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
        compare(search().placeholderText,
                "Search applications (up to 256 characters)")
        compare(search().Accessible.description, search().placeholderText)
        surface.focusSearch()
        keyClick(Qt.Key_Down)
        tryCompare(surface.resultAt(0), "activeFocus", true)
        const result = surface.resultAt(0)
        verify(result.Accessible.name.length > 0)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        const menu = findChild(result, "launcherActionsMenu")
        tryCompare(menu, "visible", true)
        const action = result.actionMenuItemAt(0)
        const pin = result.pinMenuItem()
        tryCompare(action, "activeFocus", true)
        compare(action.LayoutMirroring.enabled, true)
        compare(pin.LayoutMirroring.enabled, true)
        keyClick(Qt.Key_Down)
        tryCompare(pin, "activeFocus", true)
        keyClick(Qt.Key_Up)
        tryCompare(action, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
    }
}

// qmllint enable unqualified
