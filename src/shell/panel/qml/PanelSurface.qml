pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    property var taskModel: null
    property var groupedTaskModel: null
    property string outputName: ""
    property string clockText: ""
    property int layoutDirection: Qt.locale().textDirection
    property bool notificationAffordanceVisible: false
    property bool productionAffordancesVisible: false
    property bool developmentDiagnosticsVisible: false
    property int pendingFocusIndex: -1
    property double pendingFocusSerial: 0
    property bool pendingActionMenuRecovery: false
    property double nextTaskFocusSerial: 1

    readonly property bool groupedTasksActive: groupedTaskModel !== null
    readonly property var activeTaskModel: groupedTasksActive ? groupedTaskModel : taskModel
    readonly property int taskCount: groupedTasksActive ? groupedTaskRepeater.count
                                                        : taskRepeater.count
    readonly property string generationLabel: qsTr("%1, generation %2")
                                                 .arg(themeGeneration.profileDisplayName)
                                                 .arg(themeGeneration.generationId)
    readonly property bool opaqueFallbackActive: themeGeneration.panel.fallbackActive

    signal launcherRequested
    signal diagnosticsRequested
    signal notificationRequested
    signal notificationCenterRequested(string outputName)
    signal quickSettingsRequested(string outputName)
    signal actionMenuOpened(string outputName)
    signal actionMenuClosed
    signal actionMenuDismissedToWindowManager
    signal focusExitForward
    signal focusExitBackward

    function taskAt(index) {
        return groupedTasksActive ? groupedTaskRepeater.itemAt(index)
                                  : taskRepeater.itemAt(index)
    }

    function focusLauncher() {
        launcher.forceActiveFocus(Qt.TabFocusReason)
    }

    function focusNotification() {
        if (productionAffordancesVisible)
            notificationCenter.forceActiveFocus(Qt.OtherFocusReason)
        else if (notificationAffordanceVisible)
            notification.forceActiveFocus(Qt.OtherFocusReason)
        else if (developmentDiagnosticsVisible)
            diagnostics.forceActiveFocus(Qt.OtherFocusReason)
        else
            launcher.forceActiveFocus(Qt.OtherFocusReason)
    }

    function focusQuickSettings() {
        if (productionAffordancesVisible)
            quickSettings.forceActiveFocus(Qt.BacktabFocusReason)
        else
            focusNotification()
    }

    function revealTask(item) {
        if (item === null || taskStrip.contentItem === null || taskStrip.width <= 0)
            return
        const position = item.mapToItem(taskStrip.contentItem, 0, 0)
        const itemLeft = position.x
        const itemRight = itemLeft + item.width
        const maximumContentX = Math.max(0, taskStrip.contentWidth - taskStrip.width)

        if (item.width >= taskStrip.width) {
            const leadingEdge = root.layoutDirection === Qt.RightToLeft
                    ? itemRight - taskStrip.width : itemLeft
            taskStrip.contentX = Math.max(0, Math.min(maximumContentX, leadingEdge))
        } else if (itemLeft < taskStrip.contentX) {
            taskStrip.contentX = Math.max(0, itemLeft)
        } else if (itemRight > taskStrip.contentX + taskStrip.width) {
            taskStrip.contentX = Math.min(maximumContentX, itemRight - taskStrip.width)
        }
    }

    function focusTask(index, backward) {
        const candidate = root.taskAt(index)
        if (candidate !== null) {
            candidate.forceActiveFocus(backward ? Qt.BacktabFocusReason : Qt.TabFocusReason)
            root.revealTask(candidate)
            return
        }
        if (backward)
            launcher.forceActiveFocus(Qt.BacktabFocusReason)
        else if (productionAffordancesVisible)
            notificationCenter.forceActiveFocus(Qt.TabFocusReason)
        else if (developmentDiagnosticsVisible)
            diagnostics.forceActiveFocus(Qt.TabFocusReason)
        else if (notificationAffordanceVisible)
            notification.forceActiveFocus(Qt.TabFocusReason)
        else
            focusExitForward()
    }

    function captureFocusRecovery() {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        if (groupedTasksActive) {
            for (let index = 0; index < groupedTaskRepeater.count; ++index) {
                const candidate = root.taskAt(index)
                if (candidate !== null
                        && (candidate.activeFocus || candidate.actionMenuOpen
                            || candidate.groupPopupOpen)) {
                    pendingFocusIndex = index
                    if (candidate.groupPopupOpen) {
                        groupedTaskModel.rememberGroupPopupFocus(candidate.focusTarget)
                    } else if (candidate.actionMenuOpen) {
                        groupedTaskModel.rememberContextMenuFocus(
                                    candidate.presentation,
                                    candidate.focusedContextActionIndex >= 0
                                    ? candidate.focusedContextActionIndex
                                    : candidate.pendingContextActionIndex)
                    } else {
                        groupedTaskModel.rememberTaskFocus(candidate.presentation)
                    }
                    return
                }
            }
            return
        }
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null
                    && (candidate.activeFocus || candidate.actionMenuOpen)) {
                pendingFocusIndex = index
                pendingFocusSerial = candidate.focusRecoverySerial
                pendingActionMenuRecovery = candidate.actionMenuOpen
                return
            }
        }
    }

    function allocateTaskFocusSerial() {
        const serial = nextTaskFocusSerial
        // JavaScript numbers represent every integer exactly through 2^53 - 1.
        // Exhaustion disables identity recovery instead of reusing a live serial.
        if (serial >= 9007199254740991)
            return 0
        nextTaskFocusSerial += 1
        return serial
    }

    function prepareTaskActionMenu(originIndex) {
        for (let index = 0; index < root.taskCount; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null && index !== originIndex) {
                candidate.closePopups !== undefined
                        ? candidate.closePopups(false) : candidate.closeActionMenu(false)
            }
        }
    }

    function closeTaskActionMenus() {
        for (let index = 0; index < root.taskCount; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null) {
                candidate.closePopups !== undefined
                        ? candidate.closePopups(false) : candidate.closeActionMenu(false)
            }
        }
    }

    function recoverFocus() {
        if (groupedTasksActive)
            return
        const recoveryIndex = pendingFocusIndex
        const recoverySerial = pendingFocusSerial
        const recoverActionMenu = pendingActionMenuRecovery
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        if (recoveryIndex < 0)
            return

        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null) {
                // A retained delegate keeps focus across a valid model move. If its
                // immutable presentation was replaced, reopen its surviving action
                // surface only to restore action focus and the current typed target.
                if (recoverySerial !== 0
                        && candidate.focusRecoverySerial === recoverySerial) {
                    root.revealTask(candidate)
                    if (recoverActionMenu && candidate.openActionMenu())
                        return
                    candidate.forceActiveFocus(Qt.OtherFocusReason)
                    return
                }
            }
        }

        if (taskRepeater.count > 0) {
            const fallback = root.taskAt(Math.min(recoveryIndex,
                                                  taskRepeater.count - 1))
            if (fallback !== null) {
                fallback.forceActiveFocus(Qt.OtherFocusReason)
                root.revealTask(fallback)
                return
            }
        }
        if (root.productionAffordancesVisible)
            notificationCenter.forceActiveFocus(Qt.OtherFocusReason)
        else if (root.developmentDiagnosticsVisible)
            diagnostics.forceActiveFocus(Qt.OtherFocusReason)
        else
            launcher.forceActiveFocus(Qt.OtherFocusReason)
    }

    function restoreActionMenuFocus() {
        if (pendingFocusIndex < 0)
            return true
        if (groupedTasksActive) {
            const candidate = taskAt(Math.min(pendingFocusIndex, taskCount - 1))
            pendingFocusIndex = -1
            if (candidate !== null) {
                candidate.forceActiveFocus(Qt.OtherFocusReason)
                root.revealTask(candidate)
                return true
            }
            return false
        }
        recoverFocus()
        return true
    }

    function recoverGroupedFallback(groupIndex) {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        if (root.taskCount > 0) {
            const fallback = root.taskAt(Math.min(groupIndex, root.taskCount - 1))
            fallback.forceActiveFocus(Qt.OtherFocusReason)
            root.revealTask(fallback)
        } else if (root.developmentDiagnosticsVisible) {
            diagnostics.forceActiveFocus(Qt.OtherFocusReason)
        } else if (root.productionAffordancesVisible) {
            notificationCenter.forceActiveFocus(Qt.OtherFocusReason)
        } else {
            launcher.forceActiveFocus(Qt.OtherFocusReason)
        }
    }

    function recoverGroupedTaskFocus(groupIndex) {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        const candidate = root.taskAt(groupIndex)
        if (candidate !== null) {
            root.revealTask(candidate)
            candidate.recoverTaskFocus()
            return
        }
        recoverGroupedFallback(groupIndex)
    }

    function recoverGroupedPopupFocus(groupIndex, memberIndex) {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        const candidate = root.taskAt(groupIndex)
        if (candidate !== null && candidate.recoverGroupPopupFocus(memberIndex)) {
            root.revealTask(candidate)
            return
        }
        recoverGroupedFallback(groupIndex)
    }

    function recoverGroupedContextMenuFocus(groupIndex, actionIndex) {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        const candidate = root.taskAt(groupIndex)
        if (candidate !== null && candidate.recoverContextMenuFocus(actionIndex)) {
            root.revealTask(candidate)
            return
        }
        recoverGroupedFallback(groupIndex)
    }

    implicitWidth: 960
    implicitHeight: Math.max(themeGeneration.panel.minimumTargetSize,
                             themeGeneration.panel.panelHeight)
    LayoutMirroring.enabled: layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    Timer {
        id: focusRecoveryTimer
        interval: 0
        repeat: false
        onTriggered: root.recoverFocus()
    }

    Timer {
        id: groupedFocusRecoveryTimer
        interval: 0
        repeat: false
        onTriggered: {
            if (root.groupedTasksActive && root.pendingFocusIndex >= 0)
                root.recoverGroupedFallback(root.pendingFocusIndex)
        }
    }

    Connections {
        target: root.groupedTasksActive ? null : root.taskModel
        function onPublicationReconciliationStarted() {
            root.captureFocusRecovery()
        }
        function onPublicationApplied() {
            focusRecoveryTimer.restart()
        }
    }

    Connections {
        target: root.groupedTasksActive ? root.groupedTaskModel : null
        function onPublicationReconciliationStarted() {
            root.captureFocusRecovery()
        }
        function onPublicationApplied() {
            groupedFocusRecoveryTimer.restart()
        }
        function onTaskFocusRecoveryRequested(groupIndex) {
            root.recoverGroupedTaskFocus(groupIndex)
        }
        function onGroupPopupFocusRecoveryRequested(groupIndex, memberIndex) {
            root.recoverGroupedPopupFocus(groupIndex, memberIndex)
        }
        function onContextMenuFocusRecoveryRequested(groupIndex, actionIndex) {
            root.recoverGroupedContextMenuFocus(groupIndex, actionIndex)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.themeGeneration.panel.surfaceColor
        border.width: root.themeGeneration.panel.borderWidth
        border.color: root.themeGeneration.panel.inactiveBorderColor
    }

    RowLayout {
        id: panelContents
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        spacing: 6

        Button {
            id: launcher
            objectName: "panelLauncherButton"
            activeFocusOnTab: false
            text: qsTr("Applications")
            implicitWidth: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                    contentItem.implicitWidth
                                    + (2 * root.themeGeneration.panel.launcherPadding))
            implicitHeight: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                     root.themeGeneration.panel.panelHeight)

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Open applications")
            Accessible.description: qsTr("Open the application launcher")
            Accessible.focusable: true
            Accessible.focused: activeFocus
            Accessible.onPressAction: launcher.clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => {
                event.accepted = true
                root.focusTask(0, false)
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                root.focusExitBackward()
            }
            Keys.onSpacePressed: event => {
                event.accepted = true
                launcher.clicked()
            }
            Keys.onReturnPressed: event => {
                event.accepted = true
                launcher.clicked()
            }
            Keys.onEnterPressed: event => {
                event.accepted = true
                launcher.clicked()
            }
            onClicked: {
                root.closeTaskActionMenus()
                root.launcherRequested()
            }

            contentItem: Text {
                color: root.themeGeneration.panel.textPrimaryColor
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: root.themeGeneration.panel.bodyFontFamily
                font.pixelSize: root.themeGeneration.panel.bodyFontPixels
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: launcher.text
            }
            background: Rectangle {
                radius: root.themeGeneration.panel.launcherRadius
                color: launcher.down ? root.themeGeneration.panel.selectionColor : "transparent"
                border.width: launcher.activeFocus
                              ? root.themeGeneration.panel.focusWidth
                              : root.themeGeneration.panel.launcherBorderWidth
                border.color: launcher.activeFocus
                              ? root.themeGeneration.panel.focusColor
                              : root.themeGeneration.panel.activeBorderColor
            }
        }

        Flickable {
            id: taskStrip
            objectName: "panelTaskStrip"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: root.groupedTasksActive ? groupedTasks.width : tasks.width
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width

            Row {
                id: tasks
                visible: !root.groupedTasksActive
                width: visible ? implicitWidth : 0
                height: parent.height
                spacing: 4

                Repeater {
                    id: taskRepeater
                    model: root.taskModel

                    delegate: TaskButton {
                        required property int index
                        required property var task
                        property double focusRecoverySerial: 0

                        height: Math.min(tasks.height, root.height)
                        presentation: task
                        tokens: root.themeGeneration.panel
                        rightToLeft: root.layoutDirection === Qt.RightToLeft
                        Component.onCompleted: focusRecoverySerial = root.allocateTaskFocusSerial()
                        onFocusExitForward: root.focusTask(index + 1, false)
                        onFocusExitBackward: root.focusTask(index - 1, true)
                        onActionMenuOpening: {
                            root.prepareTaskActionMenu(index)
                            root.actionMenuOpened(root.outputName)
                        }
                        onActionMenuClosed: root.actionMenuClosed()
                        onClicked: root.closeTaskActionMenus()
                    }
                }
            }

            Row {
                id: groupedTasks
                visible: root.groupedTasksActive
                width: visible ? implicitWidth : 0
                height: parent.height
                spacing: 4

                Repeater {
                    id: groupedTaskRepeater
                    model: root.groupedTaskModel

                    delegate: GroupedTaskButton {
                        required property int index
                        required property var group

                        height: Math.min(groupedTasks.height, root.height)
                        presentation: group
                        tokens: root.themeGeneration.panel
                        rightToLeft: root.layoutDirection === Qt.RightToLeft
                        onFocusExitForward: root.focusTask(index + 1, false)
                        onFocusExitBackward: root.focusTask(index - 1, true)
                        onActionMenuOpening: {
                            root.prepareTaskActionMenu(index)
                            root.actionMenuOpened(root.outputName)
                        }
                        onActionMenuClosed: root.actionMenuClosed()
                        onActionMenuDismissedToWindowManager:
                            root.actionMenuDismissedToWindowManager()
                        onClicked: root.closeTaskActionMenus()
                    }
                }
            }
        }

        Button {
            id: notificationCenter
            objectName: "panelNotificationCenterButton"
            visible: root.productionAffordancesVisible
            enabled: visible
            activeFocusOnTab: false
            text: qsTr("Notifications")
            implicitWidth: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                    contentItem.implicitWidth
                                    + (2 * root.themeGeneration.panel.taskPadding))
            implicitHeight: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                     root.themeGeneration.panel.panelHeight)

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Open notification center")
            Accessible.description: qsTr("Show notification history on this panel's output")
            Accessible.focusable: visible
            Accessible.focused: activeFocus
            Accessible.onPressAction: notificationCenter.clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => {
                event.accepted = true
                quickSettings.forceActiveFocus(Qt.TabFocusReason)
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                root.focusTask(root.taskCount - 1, true)
            }
            onClicked: {
                root.closeTaskActionMenus()
                root.notificationCenterRequested(root.outputName)
            }
        }

        Button {
            id: quickSettings
            objectName: "panelQuickSettingsButton"
            visible: root.productionAffordancesVisible
            enabled: visible
            activeFocusOnTab: false
            text: qsTr("Quick settings")
            implicitWidth: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                    contentItem.implicitWidth
                                    + (2 * root.themeGeneration.panel.taskPadding))
            implicitHeight: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                     root.themeGeneration.panel.panelHeight)

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Open quick settings")
            Accessible.description: qsTr("Show shell and notification controls on this panel's output")
            Accessible.focusable: visible
            Accessible.focused: activeFocus
            Accessible.onPressAction: quickSettings.clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => {
                event.accepted = true
                if (root.developmentDiagnosticsVisible)
                    diagnostics.forceActiveFocus(Qt.TabFocusReason)
                else if (root.notificationAffordanceVisible)
                    notification.forceActiveFocus(Qt.TabFocusReason)
                else
                    root.focusExitForward()
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                notificationCenter.forceActiveFocus(Qt.BacktabFocusReason)
            }
            onClicked: {
                root.closeTaskActionMenus()
                root.quickSettingsRequested(root.outputName)
            }
        }

        Label {
            id: clock
            objectName: "panelClock"
            visible: root.clockText.length > 0
            text: root.clockText
            color: root.themeGeneration.panel.textPrimaryColor
            textFormat: Text.PlainText
            font.family: root.themeGeneration.panel.bodyFontFamily
            font.pixelSize: root.themeGeneration.panel.bodyFontPixels
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Local time, %1").arg(root.clockText)
            Accessible.description: qsTr("Clock formatted with the configured locale and hour style")
        }

        Button {
            id: diagnostics
            objectName: "panelDiagnosticsButton"
            visible: root.developmentDiagnosticsVisible
            enabled: visible
            activeFocusOnTab: false
            text: root.generationLabel
            implicitWidth: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                    contentItem.implicitWidth
                                    + (2 * root.themeGeneration.panel.taskPadding))
            implicitHeight: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                     root.themeGeneration.panel.panelHeight)

            Accessible.role: Accessible.Button
            Accessible.name: root.generationLabel
            Accessible.description: root.opaqueFallbackActive
                                    ? qsTr("Theme diagnostics. Opaque fallback active")
                                    : qsTr("Theme diagnostics")
            Accessible.focusable: visible
            Accessible.focused: activeFocus

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => {
                event.accepted = true
                if (root.notificationAffordanceVisible)
                    notification.forceActiveFocus(Qt.TabFocusReason)
                else
                    root.focusExitForward()
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                if (root.productionAffordancesVisible)
                    quickSettings.forceActiveFocus(Qt.BacktabFocusReason)
                else
                    root.focusTask(root.taskCount - 1, true)
            }
            onClicked: {
                root.closeTaskActionMenus()
                root.diagnosticsRequested()
            }

            contentItem: Text {
                color: root.themeGeneration.panel.textPrimaryColor
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: root.themeGeneration.panel.bodyFontFamily
                font.pixelSize: root.themeGeneration.panel.bodyFontPixels
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: diagnostics.text
            }
            background: Rectangle {
                radius: root.themeGeneration.panel.taskRadius
                color: diagnostics.down ? root.themeGeneration.panel.selectionColor : "transparent"
                border.width: diagnostics.activeFocus
                              ? root.themeGeneration.panel.focusWidth
                              : root.themeGeneration.panel.taskBorderWidth
                border.color: diagnostics.activeFocus
                              ? root.themeGeneration.panel.focusColor
                              : root.themeGeneration.panel.inactiveBorderColor
            }
        }

        Button {
            id: notification
            objectName: "panelNotificationButton"
            visible: root.notificationAffordanceVisible
            enabled: visible
            activeFocusOnTab: false
            text: qsTr("Test notification")
            implicitWidth: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                    contentItem.implicitWidth
                                    + (2 * root.themeGeneration.panel.taskPadding))
            implicitHeight: Math.max(root.themeGeneration.panel.minimumTargetSize,
                                     root.themeGeneration.panel.panelHeight)

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Send test notification")
            Accessible.description: qsTr("Request one synthetic development notification")
            Accessible.focusable: visible
            Accessible.focused: activeFocus

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => {
                event.accepted = true
                root.focusExitForward()
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                if (root.developmentDiagnosticsVisible)
                    diagnostics.forceActiveFocus(Qt.BacktabFocusReason)
                else if (root.productionAffordancesVisible)
                    quickSettings.forceActiveFocus(Qt.BacktabFocusReason)
                else
                    root.focusTask(root.taskCount - 1, true)
            }
            onClicked: {
                root.closeTaskActionMenus()
                root.notificationRequested()
            }

            contentItem: Text {
                color: root.themeGeneration.panel.textPrimaryColor
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: root.themeGeneration.panel.bodyFontFamily
                font.pixelSize: root.themeGeneration.panel.bodyFontPixels
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: notification.text
            }
            background: Rectangle {
                radius: root.themeGeneration.panel.taskRadius
                color: notification.down
                       ? root.themeGeneration.panel.selectionColor : "transparent"
                border.width: notification.activeFocus
                              ? root.themeGeneration.panel.focusWidth
                              : root.themeGeneration.panel.taskBorderWidth
                border.color: notification.activeFocus
                              ? root.themeGeneration.panel.focusColor
                              : root.themeGeneration.panel.inactiveBorderColor
            }
        }
    }
}
