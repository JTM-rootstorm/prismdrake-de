pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    required property var taskModel
    property bool notificationAffordanceVisible: false
    property var pendingFocusRecovery: null

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

    function taskAt(index: int): TaskButton {
        return taskRepeater.itemAt(index) as TaskButton
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
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null
                    && (candidate.activeFocus || candidate.actionMenuOpen)) {
                pendingFocusRecovery = {
                    "index": index,
                    "item": candidate
                }
                return
            }
        }
        pendingFocusRecovery = null
    }

    function prepareTaskActionMenu(originIndex) {
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null && index !== originIndex)
                candidate.closeActionMenu(false)
        }
    }

    function closeTaskActionMenus() {
        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null)
                candidate.closeActionMenu(false)
        }
    }

    function recoverFocus() {
        const recovery = pendingFocusRecovery
        pendingFocusRecovery = null
        if (recovery === null)
            return

        for (let index = 0; index < taskRepeater.count; ++index) {
            const candidate = root.taskAt(index)
            if (candidate !== null && candidate === recovery.item) {
                candidate.forceActiveFocus(Qt.OtherFocusReason)
                return
            }
        }

        if (taskRepeater.count > 0) {
            const fallback = root.taskAt(Math.min(recovery.index,
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
            root.closeTaskActionMenus()
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
            onClicked: root.launcherRequested()

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

                        height: tasks.height
                        presentation: task
                        tokens: root.themeGeneration.panel
                        onFocusExitForward: root.focusTask(index + 1, false)
                        onFocusExitBackward: root.focusTask(index - 1, true)
                        onActionMenuOpening: root.prepareTaskActionMenu(index)
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
            onClicked: root.diagnosticsRequested()

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
            onClicked: root.notificationRequested()

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
