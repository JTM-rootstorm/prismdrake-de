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

        verify(panelFixture.publishForge())
        tryCompare(panel.themeGeneration, "profileId", "forge")
        compare(panel, surface)
        compare(panel.taskAt(0), firstTask)
        compare(panel.generationLabel, "Prismdrake Forge, generation 2")
        compare(diagnostics().text, panel.generationLabel)
        compare(panel.opaqueFallbackActive, false)
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
        verify(first.Accessible.description.length > 0)
        verify(diagnostics().Accessible.name.indexOf("generation 1") >= 0)

        const firstState = findChild(first, "panelTaskStateLabel")
        const firstTitle = findChild(first, "panelTaskTitle")
        const secondState = findChild(second, "panelTaskStateLabel")
        const thirdState = findChild(third, "panelTaskStateLabel")
        const urgentMarker = findChild(third, "panelTaskUrgentMarker")
        verify(firstState !== null)
        verify(firstTitle !== null)
        verify(secondState !== null)
        verify(thirdState !== null)
        verify(urgentMarker !== null)
        compare(firstState.visible, true)
        compare(firstState.text, "Active")
        compare(firstTitle.textFormat, Text.PlainText)
        compare(secondState.visible, true)
        compare(secondState.text, "Inactive")
        compare(thirdState.visible, true)
        compare(thirdState.text, "Urgent, Modal")
        compare(urgentMarker.visible, true)
        verify(third.background.radius < second.background.radius)
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
        compare(panelFixture.activationCount, 0)
        first.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Space)
        tryCompare(panelFixture, "activationCount", 1)
        compare(panelFixture.lastActivationTitle, "Editor")
        compare(panelFixture.lastActivationGeneration, panelFixture.taskGeneration)
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
    }
}

// qmllint enable unqualified
