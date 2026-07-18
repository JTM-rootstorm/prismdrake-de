pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    required property var taskModel
    property bool notificationAffordanceVisible: false
    property int pendingFocusIndex: -1
    property double pendingFocusSerial: 0
    property bool pendingActionMenuRecovery: false
    property double nextTaskFocusSerial: 1

    readonly property int taskCount: taskRepeater.count
    readonly property string generationLabel: qsTr("%1, generation %2")
                                                 .arg(themeGeneration.profileDisplayName)
                                                 .arg(themeGeneration.generationId)
    readonly property bool opaqueFallbackActive: themeGeneration.panel.fallbackActive

    signal launcherRequested
    signal diagnosticsRequested
    signal notificationRequested
    signal focusExitForward
    signal focusExitBackward

    function taskAt(index) {
        return taskRepeater.itemAt(index)
    }

    function focusLauncher() {
        launcher.forceActiveFocus(Qt.TabFocusReason)
    }

    function focusNotification() {
        if (notificationAffordanceVisible)
            notification.forceActiveFocus(Qt.OtherFocusReason)
        else
            diagnostics.forceActiveFocus(Qt.OtherFocusReason)
    }

    function focusTask(index, backward) {
        const candidate = root.taskAt(index)
        if (candidate !== null) {
            candidate.forceActiveFocus(backward ? Qt.BacktabFocusReason : Qt.TabFocusReason)
            return
        }
        if (backward)
            launcher.forceActiveFocus(Qt.BacktabFocusReason)
        else
            diagnostics.forceActiveFocus(Qt.TabFocusReason)
    }

    function captureFocusRecovery() {
        pendingFocusIndex = -1
        pendingFocusSerial = 0
        pendingActionMenuRecovery = false
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            // qmllint disable missing-property
            if (candidate !== null
                    && (candidate.activeFocus || candidate.actionMenuOpen)) {
                // qmllint enable missing-property
                pendingFocusIndex = index
                // qmllint disable missing-property
                pendingFocusSerial = candidate.focusRecoverySerial
                pendingActionMenuRecovery = candidate.actionMenuOpen
                // qmllint enable missing-property
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
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null && index !== originIndex) {
                // qmllint disable missing-property
                candidate.closeActionMenu(false)
                // qmllint enable missing-property
            }
        }
    }

    function closeTaskActionMenus() {
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null) {
                // qmllint disable missing-property
                candidate.closeActionMenu(false)
                // qmllint enable missing-property
            }
        }
    }

    function recoverFocus() {
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
                // qmllint disable missing-property
                if (recoverySerial !== 0
                        && candidate.focusRecoverySerial === recoverySerial) {
                    if (recoverActionMenu && candidate.openActionMenu())
                        return
                    candidate.forceActiveFocus(Qt.OtherFocusReason)
                    return
                }
                // qmllint enable missing-property
            }
        }

        if (taskRepeater.count > 0) {
            const fallback = root.taskAt(Math.min(recoveryIndex,
                                                  taskRepeater.count - 1))
            if (fallback !== null) {
                fallback.forceActiveFocus(Qt.OtherFocusReason)
                return
            }
        }
        diagnostics.forceActiveFocus(Qt.OtherFocusReason)
    }

    implicitWidth: 960
    implicitHeight: Math.max(themeGeneration.panel.minimumTargetSize,
                             themeGeneration.panel.panelHeight)

    Timer {
        id: focusRecoveryTimer
        interval: 0
        repeat: false
        onTriggered: root.recoverFocus()
    }

    Connections {
        target: root.taskModel
        function onPublicationReconciliationStarted() {
            root.captureFocusRecovery()
        }
        function onPublicationApplied() {
            focusRecoveryTimer.restart()
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
            contentWidth: tasks.width
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width

            Row {
                id: tasks
                height: parent.height
                spacing: 4

                Repeater {
                    id: taskRepeater
                    model: root.taskModel

                    delegate: TaskButton {
                        required property int index
                        required property var task
                        property double focusRecoverySerial: 0

                        height: tasks.height
                        presentation: task
                        tokens: root.themeGeneration.panel
                        Component.onCompleted: focusRecoverySerial = root.allocateTaskFocusSerial()
                        onFocusExitForward: root.focusTask(index + 1, false)
                        onFocusExitBackward: root.focusTask(index - 1, true)
                        onActionMenuOpening: root.prepareTaskActionMenu(index)
                        onClicked: root.closeTaskActionMenus()
                    }
                }
            }
        }

        Button {
            id: diagnostics
            objectName: "panelDiagnosticsButton"
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
            Accessible.focusable: true
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
                diagnostics.forceActiveFocus(Qt.BacktabFocusReason)
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
