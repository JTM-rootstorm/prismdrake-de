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
    property bool groupedTasks: false
    property bool explicitProductionAffordances: false
    property bool developmentDiagnostics: true

    Panel.PanelSurface {
        id: panel
        x: 10
        y: 10
        width: 1400
        height: implicitHeight
        themeGeneration: panelFixture.themeGeneration
        taskModel: panelFixture.taskModel
        groupedTaskModel: testCase.groupedTasks ? panelFixture.groupedTaskModel : null
        outputName: "DP-1"
        clockText: testCase.groupedTasks ? "1:05 PM" : ""
        notificationAffordanceVisible: true
        productionAffordancesVisible: testCase.groupedTasks
                                      || testCase.explicitProductionAffordances
        developmentDiagnosticsVisible: testCase.developmentDiagnostics
    }

    SignalSpy { id: forwardExit; target: panel; signalName: "focusExitForward" }
    SignalSpy { id: backwardExit; target: panel; signalName: "focusExitBackward" }
    SignalSpy { id: launcherRequest; target: panel; signalName: "launcherRequested" }
    SignalSpy { id: notificationCenterRequest; target: panel; signalName: "notificationCenterRequested" }
    SignalSpy { id: quickSettingsRequest; target: panel; signalName: "quickSettingsRequested" }
    SignalSpy { id: actionMenuOpened; target: panel; signalName: "actionMenuOpened" }
    SignalSpy { id: actionMenuClosed; target: panel; signalName: "actionMenuClosed" }
    SignalSpy {
        id: groupedLaunchRequested
        target: panelFixture.groupedTaskModel
        signalName: "launchRequested"
    }
    SignalSpy {
        id: groupedPinChangeRequested
        target: panelFixture.groupedTaskModel
        signalName: "pinChangeRequested"
    }

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

    function groupedContextMenu(task) {
        return findChild(task, "panelGroupedTaskContextMenu")
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
        testCase.width = 1420
        groupedTasks = false
        panel.width = 1400
        explicitProductionAffordances = false
        developmentDiagnostics = true
        panel.layoutDirection = Qt.LeftToRight
        verify(panelFixture.resetLustre())
        verify(panelFixture.publishRepresentativeTasks())
        forwardExit.clear()
        backwardExit.clear()
        launcherRequest.clear()
        actionMenuOpened.clear()
        actionMenuClosed.clear()
        groupedLaunchRequested.clear()
        groupedPinChangeRequested.clear()
        tryCompare(panel, "taskCount", 3)
    }

    function test_installedAffordancesRequireExplicitRuntimeFlags() {
        const notificationCenter = findChild(panel, "panelNotificationCenterButton")
        const quickSettings = findChild(panel, "panelQuickSettingsButton")
        compare(panel.groupedTaskModel, null)

        developmentDiagnostics = false
        compare(diagnostics().visible, false)
        compare(notificationCenter.visible, false)
        compare(quickSettings.visible, false)

        explicitProductionAffordances = true
        compare(notificationCenter.visible, true)
        compare(quickSettings.visible, true)
        compare(diagnostics().visible, false)
    }

    function test_focusQuickSettingsRestoresTheExactPanelControl() {
        developmentDiagnostics = false
        explicitProductionAffordances = true
        const quickSettings = findChild(panel, "panelQuickSettingsButton")

        panel.focusLauncher()
        compare(launcher().activeFocus, true)
        panel.focusQuickSettings()
        compare(quickSettings.activeFocus, true)
        compare(quickSettings.Accessible.focused, true)
    }

    function test_groupedTasksExposeProductionActionsAndOutputScopedAffordances() {
        groupedTasks = true
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)

        const editor = panel.taskAt(0)
        const terminal = panel.taskAt(1)
        verify(editor !== null)
        verify(terminal !== null)
        compare(editor.Accessible.role, Accessible.Button)
        compare(editor.Accessible.name, "Editor")
        compare(editor.presentation.windowCount, 2)
        compare(editor.presentation.pinned, true)
        compare(editor.presentation.urgent, true)
        compare(editor.presentation.modal, true)
        compare(editor.iconName, "application-x-executable")
        compare(editor.usesGenericApplicationIcon, true)
        verify(editor.Accessible.description.indexOf("Generic application icon.") === 0)
        compare(terminal.iconName, "utilities-terminal")
        compare(terminal.usesGenericApplicationIcon, false)
        verify(terminal.Accessible.description.indexOf("Generic application icon.") < 0)
        verify(editor.width >= panel.themeGeneration.panel.minimumTargetSize)
        verify(terminal.width >= panel.themeGeneration.panel.minimumTargetSize)

        editor.Accessible.pressAction()
        const groupPopup = findChild(editor, "panelTaskGroupPopup")
        tryCompare(groupPopup, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(groupPopup, "opened", false)

        const notificationCenter = findChild(panel, "panelNotificationCenterButton")
        const quickSettings = findChild(panel, "panelQuickSettingsButton")
        const clock = findChild(panel, "panelClock")
        verify(notificationCenter.visible)
        verify(quickSettings.visible)
        compare(clock.text, "1:05 PM")
        verify(clock.Accessible.name.indexOf("1:05 PM") >= 0)
        notificationCenter.Accessible.pressAction()
        quickSettings.Accessible.pressAction()
        tryCompare(notificationCenterRequest, "count", 1)
        tryCompare(quickSettingsRequest, "count", 1)
        compare(notificationCenterRequest.signalArguments[0][0], "DP-1")
        compare(quickSettingsRequest.signalArguments[0][0], "DP-1")
    }

    function test_pinnedOnlyTaskKeyboardAccessibilityAndPinStateAreExact() {
        groupedTasks = true
        verify(panelFixture.publishPinnedOnlyGroup())
        tryCompare(panel, "taskCount", 1)

        const pinned = panel.taskAt(0)
        verify(pinned !== null)
        compare(pinned.presentation.pinned, true)
        compare(pinned.presentation.running, false)
        compare(pinned.presentation.windowCount, 0)
        compare(pinned.presentation.active, false)
        compare(pinned.presentation.minimized, false)
        compare(pinned.presentation.urgent, false)
        compare(pinned.presentation.modal, false)
        compare(pinned.enabled, true)
        compare(pinned.Accessible.role, Accessible.Button)
        compare(pinned.Accessible.name, "Notes")
        verify(pinned.Accessible.description.indexOf("Pinned, not running") >= 0)
        compare(pinned.Accessible.focusable, true)
        compare(pinned.Accessible.checkable, true)
        compare(pinned.Accessible.checked, false)
        verify(pinned.width >= panel.themeGeneration.panel.minimumTargetSize)
        verify(pinned.height >= panel.themeGeneration.panel.minimumTargetSize)

        panel.focusLauncher()
        keyClick(Qt.Key_Tab)
        tryCompare(pinned, "activeFocus", true)
        compare(pinned.Accessible.focused, true)
        keyClick(Qt.Key_Backtab)
        tryCompare(launcher(), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(pinned, "activeFocus", true)
        keyClick(Qt.Key_Right)
        tryCompare(findChild(panel, "panelNotificationCenterButton"), "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(pinned, "activeFocus", true)
        keyClick(Qt.Key_Left)
        tryCompare(launcher(), "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(pinned, "activeFocus", true)

        keyClick(Qt.Key_Space)
        tryCompare(groupedLaunchRequested, "count", 1)
        keyClick(Qt.Key_Return)
        tryCompare(groupedLaunchRequested, "count", 2)

        keyClick(Qt.Key_Menu)
        const menu = groupedContextMenu(pinned)
        tryCompare(menu, "opened", true)
        const menuContent = menu.contentItem
        compare(menuContent.Accessible.role, Accessible.PopupMenu)
        compare(menuContent.Accessible.name, "Application actions for Notes")
        compare(menuContent.Accessible.description,
                "Open a new window or change the pinned state")

        const openNew = findChild(pinned, "panelGroupedTaskOpenNew")
        const pin = findChild(pinned, "panelGroupedTaskPinToggle")
        const minimize = findChild(pinned, "panelGroupedTaskMinimize")
        const close = findChild(pinned, "panelGroupedTaskClose")
        verify(openNew !== null)
        verify(pin !== null)
        verify(minimize !== null)
        verify(close !== null)
        compare(openNew.enabled, true)
        compare(openNew.Accessible.focusable, true)
        verify(openNew.width >= panel.themeGeneration.panel.minimumTargetSize)
        verify(openNew.height >= panel.themeGeneration.panel.minimumTargetSize)
        compare(pin.enabled, true)
        compare(pin.Accessible.role, Accessible.MenuItem)
        compare(pin.Accessible.name, "Unpin Notes")
        compare(pin.Accessible.description, "Change the shared panel pin order")
        compare(pin.Accessible.checkable, true)
        compare(pin.Accessible.checked, true)
        compare(pin.Accessible.focusable, true)
        verify(pin.width >= panel.themeGeneration.panel.minimumTargetSize)
        verify(pin.height >= panel.themeGeneration.panel.minimumTargetSize)
        compare(minimize.visible, false)
        compare(minimize.enabled, false)
        compare(minimize.Accessible.focusable, false)
        compare(close.visible, false)
        compare(close.enabled, false)
        compare(close.Accessible.focusable, false)

        pin.Accessible.pressAction()
        tryCompare(groupedPinChangeRequested, "count", 1)
        tryCompare(menu, "opened", false)
        tryCompare(pinned, "activeFocus", true)

        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        tryCompare(menu, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "opened", false)
        tryCompare(pinned, "activeFocus", true)

        panel.layoutDirection = Qt.RightToLeft
        compare(pinned.rightToLeft, true)
        keyClick(Qt.Key_Left)
        tryCompare(findChild(panel, "panelNotificationCenterButton"), "activeFocus", true)
        pinned.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Right)
        tryCompare(launcher(), "activeFocus", true)
    }

    function test_singleWindowGroupedTaskExposesAccessibleWindowActions() {
        groupedTasks = true
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)

        const editor = panel.taskAt(0)
        const terminal = panel.taskAt(1)
        compare(editor.presentation.windowCount, 2)
        compare(terminal.presentation.windowCount, 1)

        verify(editor.openActionMenu())
        const editorMinimize = findChild(editor, "panelGroupedTaskMinimize")
        const editorClose = findChild(editor, "panelGroupedTaskClose")
        compare(editorMinimize.visible, false)
        compare(editorClose.visible, false)
        editor.closeActionMenu(false)

        verify(terminal.openActionMenu())
        const terminalMinimize = findChild(terminal, "panelGroupedTaskMinimize")
        const terminalClose = findChild(terminal, "panelGroupedTaskClose")
        compare(terminalMinimize.visible, true)
        compare(terminalMinimize.enabled, true)
        compare(terminalMinimize.Accessible.role, Accessible.MenuItem)
        compare(terminalMinimize.Accessible.name, "Minimize Terminal")
        terminalMinimize.Accessible.pressAction()

        tryCompare(panelFixture, "minimizationCount", 1)
        compare(panelFixture.lastMinimizationTitle, "Terminal")
        compare(panelFixture.lastMinimizationGeneration, panelFixture.taskGeneration)

        verify(terminal.openActionMenu())
        compare(terminalClose.visible, true)
        compare(terminalClose.enabled, true)
        compare(terminalClose.Accessible.role, Accessible.MenuItem)
        compare(terminalClose.Accessible.name, "Close Terminal")
        terminalClose.Accessible.pressAction()

        tryCompare(panelFixture, "closeCount", 1)
        compare(panelFixture.lastCloseTitle, "Terminal")
        compare(panelFixture.lastCloseGeneration, panelFixture.taskGeneration)
    }

    function test_groupPopupKeyboardActionsAndAccessibilityAreDeterministic() {
        testCase.width = 420
        panel.width = 400
        groupedTasks = true
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)

        const editor = panel.taskAt(0)
        editor.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        const popup = findChild(editor, "panelTaskGroupPopup")
        tryCompare(popup, "opened", true)
        tryCompare(actionMenuOpened, "count", 1)
        compare(actionMenuOpened.signalArguments[0][0], "DP-1")
        const firstMember = popup.memberAt(0)
        const secondMember = popup.memberAt(1)
        const viewport = findChild(popup, "panelTaskGroupViewport")
        verify(firstMember !== null)
        verify(secondMember !== null)
        verify(viewport !== null)
        compare(viewport.Accessible.role, Accessible.PopupMenu)
        compare(viewport.Accessible.name, "Windows for Editor")
        compare(viewport.Accessible.description,
                "Choose a window, then activate, minimize, or close it")
        verify(popup.width <= testCase.width)
        verify(viewport.contentWidth > viewport.availableWidth)
        tryCompare(firstMember, "activeFocus", true)
        tryCompare(viewport.contentItem, "contentX", 0)
        compare(firstMember.Accessible.role, Accessible.ListItem)
        verify(firstMember.Accessible.description.indexOf("Press Enter") >= 0)

        keyClick(Qt.Key_Menu)
        const activate = findChild(firstMember, "panelTaskGroupActivate")
        const minimize = findChild(firstMember, "panelTaskGroupMinimize")
        const close = findChild(firstMember, "panelTaskGroupClose")
        tryCompare(activate, "activeFocus", true)
        compare(activate.Accessible.role, Accessible.MenuItem)
        verify(activate.width >= panel.themeGeneration.panel.minimumTargetSize)
        verify(activate.height >= panel.themeGeneration.panel.minimumTargetSize)
        keyClick(Qt.Key_Right)
        tryCompare(minimize, "activeFocus", true)
        keyClick(Qt.Key_Right)
        tryCompare(close, "activeFocus", true)
        keyClick(Qt.Key_Down)
        tryCompare(findChild(secondMember, "panelTaskGroupClose"), "activeFocus", true)
        tryVerify(() => viewport.contentItem.contentX > 0)
        keyClick(Qt.Key_Escape)
        tryCompare(popup, "opened", false)
        tryCompare(actionMenuClosed, "count", 1)
        tryCompare(editor, "activeFocus", true)

        keyClick(Qt.Key_Return)
        tryCompare(popup, "opened", true)
        tryCompare(firstMember, "activeFocus", true)
        keyClick(Qt.Key_Return)
        tryCompare(panelFixture, "activationCount", 1)
        tryCompare(popup, "opened", false)
        compare(panelFixture.lastActivationTitle, "Editor document")
    }

    function test_groupedTaskAndPopupExposeCompleteKeyboardStateTransitions() {
        groupedTasks = true
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)

        const editor = panel.taskAt(0)
        const terminal = panel.taskAt(1)
        compare(editor.activeState, true)
        compare(editor.urgentState, true)
        compare(editor.presentation.minimized, false)
        compare(editor.enabled, true)
        compare(editor.Accessible.checked, true)

        editor.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Tab)
        tryCompare(terminal, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(editor, "activeFocus", true)
        keyClick(Qt.Key_Right)
        tryCompare(terminal, "activeFocus", true)
        keyClick(Qt.Key_Left)
        tryCompare(editor, "activeFocus", true)

        keyClick(Qt.Key_Menu)
        const context = groupedContextMenu(editor)
        tryCompare(context, "opened", true)
        compare(findChild(editor, "panelGroupedTaskMinimize").enabled, false)
        keyClick(Qt.Key_Escape)
        tryCompare(context, "opened", false)
        tryCompare(editor, "activeFocus", true)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        tryCompare(context, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(context, "opened", false)
        tryCompare(editor, "activeFocus", true)

        keyClick(Qt.Key_Space)
        const popup = findChild(editor, "panelTaskGroupPopup")
        tryCompare(popup, "opened", true)
        verify(editor.Accessible.description.indexOf("Window list shown") >= 0)
        const firstMember = popup.memberAt(0)
        const secondMember = popup.memberAt(1)
        tryCompare(firstMember, "activeFocus", true)
        verify(firstMember.Accessible.description.indexOf("Active") >= 0)
        verify(firstMember.Accessible.description.indexOf("Minimized") < 0)
        keyClick(Qt.Key_Down)
        tryCompare(secondMember, "activeFocus", true)
        compare(secondMember.Accessible.selectable, true)
        compare(secondMember.Accessible.selected, true)
        compare(firstMember.Accessible.selected, false)
        verify(secondMember.Accessible.description.indexOf("Minimized") >= 0)
        verify(secondMember.Accessible.description.indexOf("Urgent") >= 0)
        const secondMinimize = findChild(secondMember, "panelTaskGroupMinimize")
        compare(secondMinimize.enabled, false)
        compare(secondMinimize.Accessible.focusable, false)
        keyClick(Qt.Key_Backtab)
        tryCompare(firstMember, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        const activate = findChild(firstMember, "panelTaskGroupActivate")
        tryCompare(activate, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(firstMember, "activeFocus", true)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        tryCompare(activate, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(popup, "opened", false)
        tryCompare(editor, "activeFocus", true)

        keyClick(Qt.Key_Return)
        tryCompare(popup, "opened", true)
        tryCompare(firstMember, "activeFocus", true)
        keyClick(Qt.Key_Space)
        tryCompare(panelFixture, "activationCount", 1)
        tryCompare(popup, "opened", false)
    }

    function test_pinnedSurfaceConsumesAccessibleGenerationTokens() {
        verify(panelFixture.resetAccessible())
        groupedTasks = true
        panel.layoutDirection = Qt.RightToLeft
        compare(panel.themeGeneration.highContrast, true)
        compare(panel.themeGeneration.reducedMotion, true)
        compare(panel.themeGeneration.transparencyDisabled, true)
        compare(panel.themeGeneration.panel.fallbackActive, true)
        verify(panel.themeGeneration.panel.bodyFontPixels > 14)
        compare(findChild(panel, "panelClock").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)

        verify(panelFixture.publishPinnedOnlyGroup())
        tryCompare(panel, "taskCount", 1)
        const pinned = panel.taskAt(0)
        compare(pinned.tokens, panel.themeGeneration.panel)
        compare(pinned.rightToLeft, true)
        compare(findChild(pinned, "panelGroupedTaskTitle").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)
        verify(pinned.height >= panel.themeGeneration.panel.minimumTargetSize)
        verify(pinned.width >= panel.themeGeneration.panel.minimumTargetSize)
    }

    function test_groupedSurfacesConsumeAccessibleGenerationTokens() {
        verify(panelFixture.resetAccessible())
        groupedTasks = true
        panel.layoutDirection = Qt.RightToLeft
        compare(panel.themeGeneration.highContrast, true)
        compare(panel.themeGeneration.reducedMotion, true)
        compare(panel.themeGeneration.transparencyDisabled, true)
        compare(panel.themeGeneration.panel.fallbackActive, true)
        verify(panel.themeGeneration.panel.bodyFontPixels > 14)
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)
        const editor = panel.taskAt(0)
        compare(editor.tokens, panel.themeGeneration.panel)
        compare(editor.rightToLeft, true)
        compare(findChild(editor, "panelGroupedTaskTitle").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)
        verify(editor.height >= panel.themeGeneration.panel.minimumTargetSize)
        editor.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        const popup = findChild(editor, "panelTaskGroupPopup")
        tryCompare(popup, "opened", true)
        compare(popup.tokens, panel.themeGeneration.panel)
        compare(popup.rightToLeft, true)
        compare(findChild(popup.memberAt(0), "panelTaskGroupMemberTitle").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)
        verify(popup.width <= testCase.width)
        verify(popup.memberAt(0).height >= panel.themeGeneration.panel.minimumTargetSize)
        keyClick(Qt.Key_Escape)
        tryCompare(popup, "opened", false)
    }

    function test_longRunningTaskRemainsBoundedAndScrollable() {
        verify(panelFixture.publishLongTasks())
        tryCompare(panel, "taskCount", 1)
        const strip = findChild(panel, "panelTaskStrip")
        const task = panel.taskAt(0)
        verify(task.Accessible.name.length > 900)
        verify(task.width > strip.width)
        tryVerify(() => strip.contentWidth > strip.width)
        compare(strip.clip, true)
        panel.focusTask(0, false)
        tryCompare(task, "activeFocus", true)
        tryCompare(strip, "contentX", 0)
        const position = task.mapToItem(strip.contentItem, 0, 0)
        verify(position.x < strip.contentX + strip.width)
        verify(position.x + task.width > strip.contentX)
    }

    function test_longPinnedTaskRemainsBoundedAndScrollable() {
        groupedTasks = true
        verify(panelFixture.publishLongPinnedGroup())
        tryCompare(panel, "taskCount", 1)
        const strip = findChild(panel, "panelTaskStrip")
        const pinned = panel.taskAt(0)
        verify(pinned.Accessible.name.length > 900)
        verify(pinned.width > strip.width)
        tryVerify(() => strip.contentWidth > strip.width)
        compare(strip.clip, true)
        panel.layoutDirection = Qt.RightToLeft
        panel.focusTask(0, false)
        tryCompare(pinned, "activeFocus", true)
        tryVerify(() => strip.contentX > 0, 5000,
                  "RTL pinned task was not revealed: contentX=" + strip.contentX
                  + ", contentWidth=" + strip.contentWidth + ", width=" + strip.width)
        const position = pinned.mapToItem(strip.contentItem, 0, 0)
        verify(position.x < strip.contentX + strip.width)
        verify(position.x + pinned.width > strip.contentX)
    }

    function test_longGroupedTaskAndMembersRemainBoundedAndScrollable() {
        groupedTasks = true
        verify(panelFixture.publishLongGroups())
        tryCompare(panel, "taskCount", 2)
        const strip = findChild(panel, "panelTaskStrip")
        const editor = panel.taskAt(0)
        const terminal = panel.taskAt(1)
        verify(editor.Accessible.name.length > 900)
        verify(editor.width > strip.width)
        tryVerify(() => strip.contentWidth > strip.width)
        compare(strip.clip, true)

        panel.focusTask(0, false)
        tryCompare(editor, "activeFocus", true)
        tryCompare(strip, "contentX", 0)
        keyClick(Qt.Key_Right)
        tryCompare(terminal, "activeFocus", true)
        tryVerify(() => strip.contentX > 0, 5000,
                  "LTR terminal was not revealed: contentX=" + strip.contentX
                  + ", terminalX=" + terminal.mapToItem(strip.contentItem, 0, 0).x
                  + ", contentWidth=" + strip.contentWidth + ", width=" + strip.width)
        let position = terminal.mapToItem(strip.contentItem, 0, 0)
        verify(position.x >= strip.contentX)
        verify(position.x + terminal.width <= strip.contentX + strip.width)

        panel.layoutDirection = Qt.RightToLeft
        panel.focusTask(0, false)
        tryCompare(editor, "activeFocus", true)
        tryVerify(() => strip.contentX > 0)
        keyClick(Qt.Key_Left)
        tryCompare(terminal, "activeFocus", true)
        tryCompare(strip, "contentX", 0)
        position = terminal.mapToItem(strip.contentItem, 0, 0)
        verify(position.x >= strip.contentX)
        verify(position.x + terminal.width <= strip.contentX + strip.width)

        panel.focusTask(0, false)
        keyClick(Qt.Key_Return)
        const popup = findChild(editor, "panelTaskGroupPopup")
        const viewport = findChild(popup, "panelTaskGroupViewport")
        tryCompare(popup, "opened", true)
        popup.maximumWidth = 400
        tryCompare(popup, "width", 400)
        verify(popup.width <= testCase.width)
        compare(viewport.clip, true)
        verify(viewport.contentWidth > viewport.availableWidth)
        verify(popup.memberAt(0).Accessible.name.length > 900)
        verify(popup.memberAt(0).Accessible.description.length > 900)
        tryCompare(popup.memberAt(0), "activeFocus", true)
        popup.revealMember(popup.memberAt(0))
        tryVerify(() => viewport.contentItem.contentX > 0)
        const firstMemberContentX = viewport.contentItem.contentX
        keyClick(Qt.Key_Down)
        tryCompare(popup.memberAt(1), "activeFocus", true)
        tryVerify(() => viewport.contentItem.contentX !== firstMemberContentX)
        keyClick(Qt.Key_Escape)
        tryCompare(popup, "opened", false)
    }

    function test_groupPopupOverflowRevealMirrorsForRightToLeft() {
        testCase.width = 420
        panel.width = 400
        panel.layoutDirection = Qt.RightToLeft
        groupedTasks = true
        verify(panelFixture.publishRepresentativeGroups())
        tryCompare(panel, "taskCount", 2)

        const editor = panel.taskAt(0)
        editor.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        const popup = findChild(editor, "panelTaskGroupPopup")
        const viewport = findChild(popup, "panelTaskGroupViewport")
        const firstMember = popup.memberAt(0)
        const secondMember = popup.memberAt(1)
        tryCompare(popup, "opened", true)
        tryCompare(firstMember, "activeFocus", true)
        tryVerify(() => viewport.contentItem.contentX > 0)

        keyClick(Qt.Key_Down)
        tryCompare(secondMember, "activeFocus", true)
        tryCompare(viewport.contentItem, "contentX", 0)
        keyClick(Qt.Key_Up)
        tryCompare(firstMember, "activeFocus", true)
        tryVerify(() => viewport.contentItem.contentX > 0)
        keyClick(Qt.Key_Escape)
        tryCompare(popup, "opened", false)
    }

    function test_taskArrowTraversalMirrorsForRightToLeft() {
        panel.taskAt(1).forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Left)
        tryCompare(panel.taskAt(0), "activeFocus", true)
        keyClick(Qt.Key_Right)
        tryCompare(panel.taskAt(1), "activeFocus", true)

        panel.layoutDirection = Qt.RightToLeft
        compare(panel.LayoutMirroring.enabled, true)
        keyClick(Qt.Key_Right)
        tryCompare(panel.taskAt(0), "activeFocus", true)
        keyClick(Qt.Key_Left)
        tryCompare(panel.taskAt(1), "activeFocus", true)
    }

    function test_accessibleLauncherPressEmitsExactlyOneRequest() {
        launcher().Accessible.pressAction()
        tryCompare(launcherRequest, "count", 1)
        wait(0)
        compare(launcherRequest.count, 1)
        launcher().forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        tryCompare(launcherRequest, "count", 2)
        keyClick(Qt.Key_Space)
        tryCompare(launcherRequest, "count", 3)
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
        verify(panel.themeGeneration.panel.bodyFontPixels > 14)
        compare(findChild(panel, "panelClock").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)
        verify(launcher().height >= 48)
        verify(panel.taskAt(0).height >= 48)
        verify(diagnostics().Accessible.description.indexOf("Opaque fallback active") >= 0)
        const first = panel.taskAt(0)
        compare(findChild(first, "panelTaskTitle").font.pixelSize,
                panel.themeGeneration.panel.bodyFontPixels)
        verify(first.openActionMenu())
        tryCompare(contextMenu(first), "opened", true)
        verify(contextMenu(first).height <= panel.height)
        verifyActionSurfaceFits(first)
        first.closeActionMenu(false)
    }
}

// qmllint enable unqualified
