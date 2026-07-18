import QtQuick
import QtTest
import "../../../src/shell/panel/qml" as Panel

// The Quick Test setup publishes this test-only context fixture before loading the file.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "PanelSurfaceRealAdapters"
    when: windowShown
    width: 1420
    height: 180
    visible: true

    Panel.PanelSurface {
        id: panel
        x: 10
        y: 10
        width: 1400
        height: implicitHeight
        themeGeneration: panelFixture.themeGeneration
        taskModel: panelFixture.taskModel
        notificationAffordanceVisible: true
    }

    SignalSpy { id: forwardExit; target: panel; signalName: "focusExitForward" }
    SignalSpy { id: backwardExit; target: panel; signalName: "focusExitBackward" }
    SignalSpy { id: launcherRequest; target: panel; signalName: "launcherRequested" }

    function launcher() {
        return findChild(panel, "panelLauncherButton")
    }

    function diagnostics() {
        return findChild(panel, "panelDiagnosticsButton")
    }

    function notification() {
        return findChild(panel, "panelNotificationButton")
    }

    function contextMenu(task) {
        return findChild(task, "panelTaskContextMenu")
    }

    function minimizeAction(task) {
        return findChild(task, "panelTaskMinimizeAction")
    }

    function closeAction(task) {
        return findChild(task, "panelTaskCloseAction")
    }

    function verifyActionSurfaceFits(task) {
        const menu = contextMenu(task)
        const row = findChild(task, "panelTaskActionRow")
        verify(row.x >= 0)
        verify(row.y >= 0)
        verify(row.x + row.width <= menu.width)
        verify(row.y + row.height <= menu.height)
        verify(menu.width <= task.width)
        verify(menu.height <= task.height)
    }

    function initTestCase() {
        failOnWarning(/.*/)
    }

    function init() {
        verify(panelFixture.resetLustre())
        verify(panelFixture.publishRepresentativeTasks())
        forwardExit.clear()
        backwardExit.clear()
        launcherRequest.clear()
        tryCompare(panel, "taskCount", 3)
    }

    function test_accessibleLauncherPressEmitsExactlyOneRequest() {
        launcher().Accessible.pressAction()
        tryCompare(launcherRequest, "count", 1)
        wait(0)
        compare(launcherRequest.count, 1)
    }

    function test_profilesUseOneTreeAndCoherentGenerationLabel() {
        const surface = panel
        const firstTask = panel.taskAt(0)
        compare(panel.themeGeneration.profileId, "lustre")
        compare(panel.generationLabel, "Prismdrake Lustre, generation 1")
        compare(diagnostics().text, panel.generationLabel)
        compare(panel.opaqueFallbackActive, false)
        verify(firstTask.openActionMenu())
        tryCompare(contextMenu(firstTask), "opened", true)
        verify(contextMenu(firstTask).height <= panel.height)
        verifyActionSurfaceFits(firstTask)
        firstTask.closeActionMenu(false)

        verify(panelFixture.publishForge())
        tryCompare(panel.themeGeneration, "profileId", "forge")
        compare(panel, surface)
        compare(panel.taskAt(0), firstTask)
        compare(panel.generationLabel, "Prismdrake Forge, generation 2")
        compare(diagnostics().text, panel.generationLabel)
        compare(panel.opaqueFallbackActive, false)
        verify(firstTask.openActionMenu())
        tryCompare(contextMenu(firstTask), "opened", true)
        verify(contextMenu(firstTask).height <= panel.height)
        verifyActionSurfaceFits(firstTask)
        firstTask.closeActionMenu(false)
    }

    function test_targetsAccessibilityAndNonColorTaskStates() {
        const minimum = panel.themeGeneration.panel.minimumTargetSize
        const first = panel.taskAt(0)
        const second = panel.taskAt(1)
        const third = panel.taskAt(2)
        verify(launcher().width >= minimum)
        verify(launcher().height >= minimum)
        verify(first.width >= minimum)
        verify(first.height >= minimum)
        verify(diagnostics().width >= minimum)
        verify(notification().width >= minimum)

        compare(launcher().Accessible.role, Accessible.Button)
        compare(launcher().Accessible.name, "Open applications")
        compare(first.Accessible.role, Accessible.Button)
        compare(first.Accessible.name, "Editor")
        compare(first.Accessible.checked, true)
        compare(second.Accessible.checked, false)
        verify(first.Accessible.description.indexOf("Generic application icon.") === 0)
        verify(diagnostics().Accessible.name.indexOf("generation 1") >= 0)

        const fallbackIcon = findChild(first, "panelTaskFallbackIcon")
        const firstState = findChild(first, "panelTaskStateLabel")
        const firstTitle = findChild(first, "panelTaskTitle")
        const secondState = findChild(second, "panelTaskStateLabel")
        const thirdState = findChild(third, "panelTaskStateLabel")
        const urgentMarker = findChild(third, "panelTaskUrgentMarker")
        verify(fallbackIcon !== null)
        verify(firstState !== null)
        verify(firstTitle !== null)
        verify(secondState !== null)
        verify(thirdState !== null)
        verify(urgentMarker !== null)
        compare(fallbackIcon.width, panel.themeGeneration.panel.iconSize)
        compare(fallbackIcon.height, panel.themeGeneration.panel.iconSize)
        compare(fallbackIcon.iconName, "application-x-executable")
        compare(fallbackIcon.Accessible.ignored, true)
        compare(firstState.visible, true)
        compare(firstState.text, "Active")
        compare(firstTitle.textFormat, Text.PlainText)
        verify(firstTitle.width >= firstTitle.implicitWidth)
        compare(secondState.visible, true)
        compare(secondState.text, "Inactive")
        compare(thirdState.visible, true)
        compare(thirdState.text, "Urgent, Modal")
        compare(urgentMarker.visible, true)
        verify(third.background.radius < second.background.radius)
    }

    function test_pointerAndKeyboardOpenExactTargetActionMenu() {
        const first = panel.taskAt(0)
        const second = panel.taskAt(1)
        const firstMenu = contextMenu(first)
        const secondMenu = contextMenu(second)
        const pointerArea = findChild(first, "panelTaskPointerArea")

        compare(panelFixture.activationCount, 0)
        compare(pointerArea.width, first.contentItem.width)
        compare(pointerArea.height, first.contentItem.height)
        compare(pointerArea.acceptedButtons, Qt.LeftButton)
        mouseClick(pointerArea, 2, 2, Qt.RightButton)
        tryCompare(firstMenu, "opened", true)
        compare(first.actionTarget, first.presentation)
        compare(secondMenu.opened, false)
        compare(panelFixture.activationCount, 0)

        second.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(secondMenu, "opened", true)
        compare(firstMenu.opened, false)
        compare(first.actionTarget, null)
        compare(second.actionTarget, second.presentation)
        keyClick(Qt.Key_Escape)
        tryCompare(secondMenu, "opened", false)
        tryCompare(second, "activeFocus", true)

        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        tryCompare(secondMenu, "opened", true)
        compare(second.actionTarget, second.presentation)
        keyClick(Qt.Key_Escape)
        tryCompare(second, "activeFocus", true)
    }

    function test_actionMenuIsHorizontalAccessibleAndKeyboardDeterministic() {
        const first = panel.taskAt(0)
        const menu = contextMenu(first)
        const minimize = minimizeAction(first)
        const close = closeAction(first)
        const minimum = panel.themeGeneration.panel.minimumTargetSize

        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(menu, "opened", true)
        tryCompare(minimize, "activeFocus", true)
        compare(menu.Accessible.role, Accessible.PopupMenu)
        compare(menu.Accessible.name, "Window actions for Editor")
        compare(findChild(first, "panelTaskPresentationContent").visible, false)
        verify(first.Accessible.description.indexOf("Window actions shown.") >= 0)
        compare(minimize.Accessible.role, Accessible.MenuItem)
        compare(minimize.Accessible.name, "Minimize Editor")
        compare(close.Accessible.role, Accessible.MenuItem)
        compare(close.Accessible.name, "Close Editor")
        verify(minimize.Accessible.description.length > 0)
        verify(close.Accessible.description.length > 0)
        verify(minimize.width >= minimum)
        verify(minimize.height >= minimum)
        verify(close.width >= minimum)
        verify(close.height >= minimum)
        verify(menu.height <= panel.height)
        verify(minimize.x < close.x)
        compare(minimize.y, close.y)

        keyClick(Qt.Key_Right)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Right)
        tryCompare(minimize, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(minimize, "activeFocus", true)
        keyClick(Qt.Key_Left)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "opened", false)
        compare(findChild(first, "panelTaskPresentationContent").visible, true)
        verify(first.Accessible.description.indexOf("Window actions shown.") < 0)
        tryCompare(first, "activeFocus", true)
    }

    function test_pointerActivationDismissesOpenSurface() {
        const first = panel.taskAt(0)
        const second = panel.taskAt(1)
        const firstMenu = contextMenu(first)

        verify(first.openActionMenu())
        mouseClick(findChild(second, "panelTaskPointerArea"), 2, 2, Qt.LeftButton)
        tryCompare(firstMenu, "opened", false)
        tryCompare(panelFixture, "activationCount", 1)

        verify(first.openActionMenu())
        mouseClick(launcher(), 2, 2, Qt.LeftButton)
        tryCompare(firstMenu, "opened", false)

        verify(first.openActionMenu())
        mouseClick(diagnostics(), 2, 2, Qt.LeftButton)
        tryCompare(firstMenu, "opened", false)

        verify(first.openActionMenu())
        mouseClick(notification(), 2, 2, Qt.LeftButton)
        tryCompare(firstMenu, "opened", false)
        compare(panelFixture.minimizationCount, 0)
        compare(panelFixture.closeCount, 0)
    }

    function test_actionsForwardOnlyCapturedTypedTarget() {
        const first = panel.taskAt(0)
        const second = panel.taskAt(1)

        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(contextMenu(first), "opened", true)
        tryCompare(minimizeAction(first), "enabled", true)
        minimizeAction(first).click()
        tryCompare(panelFixture, "minimizationCount", 1)
        compare(panelFixture.lastMinimizationTitle, "Editor")
        compare(panelFixture.lastMinimizationGeneration, panelFixture.taskGeneration)
        compare(contextMenu(first).opened, false)
        compare(first.actionTarget, null)

        second.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(contextMenu(second), "opened", true)
        tryCompare(closeAction(second), "enabled", true)
        closeAction(second).click()
        tryCompare(panelFixture, "closeCount", 1)
        compare(panelFixture.lastCloseTitle, "Terminal")
        compare(panelFixture.lastCloseGeneration, panelFixture.taskGeneration)
        compare(contextMenu(second).opened, false)
        compare(second.actionTarget, null)
        compare(panelFixture.activationCount, 0)
    }

    function test_keyboardActionsForwardOnlyFocusedTarget() {
        const first = panel.taskAt(0)
        const second = panel.taskAt(1)

        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(minimizeAction(first), "activeFocus", true)
        keyClick(Qt.Key_Space)
        tryCompare(panelFixture, "minimizationCount", 1)
        compare(panelFixture.lastMinimizationTitle, "Editor")
        compare(contextMenu(first).opened, false)

        second.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        tryCompare(minimizeAction(second), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(closeAction(second), "activeFocus", true)
        keyClick(Qt.Key_Return)
        tryCompare(panelFixture, "closeCount", 1)
        compare(panelFixture.lastCloseTitle, "Terminal")
        compare(contextMenu(second).opened, false)
        compare(panelFixture.activationCount, 0)
    }

    function test_minimizedTargetDisablesMinimizeAndStartsOnClose() {
        verify(panelFixture.setTaskMinimized(1, true))
        tryCompare(panel.taskAt(1).presentation, "minimized", true)
        const second = panel.taskAt(1)
        const menu = contextMenu(second)
        const minimize = minimizeAction(second)
        const close = closeAction(second)

        second.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(menu, "opened", true)
        compare(minimize.enabled, false)
        compare(minimize.Accessible.focusable, false)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Left)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(second, "activeFocus", true)
    }

    function test_reconciliationReopensOnlyForSurvivingTaskDelegate() {
        const first = panel.taskAt(0)
        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Menu)
        tryCompare(contextMenu(first), "opened", true)
        compare(first.actionTarget, first.presentation)

        verify(panelFixture.swapFirstTwoTasks())
        tryCompare(contextMenu(first), "opened", true)
        compare(first.actionTarget, first.presentation)
        compare(panelFixture.closeCount, 0)
        tryCompare(minimizeAction(first), "activeFocus", true)
        compare(panel.taskAt(1), first)

        verify(panelFixture.setTaskMinimized(1, true))
        tryCompare(contextMenu(first), "opened", true)
        compare(first.actionTarget, first.presentation)
        compare(minimizeAction(first).enabled, false)
        tryCompare(closeAction(first), "activeFocus", true)
        compare(panelFixture.minimizationCount, 0)

        verify(panelFixture.removeTask(1))
        tryCompare(panel, "taskCount", 2)
        for (let index = 0; index < panel.taskCount; ++index)
            compare(contextMenu(panel.taskAt(index)).opened, false)
        compare(panelFixture.closeCount, 0)
    }

    function test_explicitTabAndBacktabTraversal() {
        panel.focusLauncher()
        tryCompare(launcher(), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(panel.taskAt(0), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(panel.taskAt(1), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(panel.taskAt(2), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(diagnostics(), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(notification(), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(forwardExit, "count", 1)

        notification().forceActiveFocus(Qt.BacktabFocusReason)
        keyClick(Qt.Key_Backtab)
        tryCompare(diagnostics(), "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(panel.taskAt(2), "activeFocus", true)
        panel.focusLauncher()
        keyClick(Qt.Key_Backtab)
        tryCompare(backwardExit, "count", 1)
    }

    function test_primaryActivationForwardsTypedCurrentIntent() {
        const first = panel.taskAt(0)
        const pointerArea = findChild(first, "panelTaskPointerArea")
        compare(panelFixture.activationCount, 0)
        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Space)
        tryCompare(panelFixture, "activationCount", 1)
        compare(panelFixture.lastActivationTitle, "Editor")
        compare(panelFixture.lastActivationGeneration, panelFixture.taskGeneration)

        mouseClick(pointerArea, 2, 2, Qt.LeftButton)
        tryCompare(panelFixture, "activationCount", 2)
        first.Accessible.pressAction()
        tryCompare(panelFixture, "activationCount", 3)
    }

    function test_focusedRemovalSelectsNextThenPrevious() {
        panel.taskAt(1).forceActiveFocus(Qt.OtherFocusReason)
        tryCompare(panel.taskAt(1), "activeFocus", true)
        verify(panelFixture.removeTask(1))
        tryCompare(panel, "taskCount", 2)
        tryCompare(panel.taskAt(1), "activeFocus", true)
        compare(panel.taskAt(1).presentation.title, "Urgent dialog")

        verify(panelFixture.removeTask(1))
        tryCompare(panel, "taskCount", 1)
        tryCompare(panel.taskAt(0), "activeFocus", true)
        compare(panel.taskAt(0).presentation.title, "Editor")
    }

    function test_focusedTaskSurvivesAuthoritativeReorder() {
        const focusedTask = panel.taskAt(0)
        focusedTask.forceActiveFocus(Qt.OtherFocusReason)
        tryCompare(focusedTask, "activeFocus", true)

        verify(panelFixture.swapFirstTwoTasks())
        tryCompare(panel.taskAt(1), "activeFocus", true)
        compare(panel.taskAt(1), focusedTask)
        compare(panel.taskAt(1).presentation.title, "Editor")
    }

    function test_accessibilityAndOpaqueFallbackComeFromOneRealGeneration() {
        verify(panelFixture.resetAccessible())
        verify(panelFixture.publishRepresentativeTasks())
        tryCompare(panel.themeGeneration, "highContrast", true)
        compare(panel.themeGeneration.reducedMotion, true)
        compare(panel.themeGeneration.transparencyDisabled, true)
        compare(panel.themeGeneration.panel.fallbackActive, true)
        compare(panel.themeGeneration.panel.fastMotionMs, 0)
        compare(panel.themeGeneration.panel.minimumTargetSize, 48)
        verify(launcher().height >= 48)
        verify(panel.taskAt(0).height >= 48)
        verify(diagnostics().Accessible.description.indexOf("Opaque fallback active") >= 0)
        const first = panel.taskAt(0)
        verify(first.openActionMenu())
        tryCompare(contextMenu(first), "opened", true)
        verify(contextMenu(first).height <= panel.height)
        verifyActionSurfaceFits(first)
        first.closeActionMenu(false)
    }
}

// qmllint enable unqualified
