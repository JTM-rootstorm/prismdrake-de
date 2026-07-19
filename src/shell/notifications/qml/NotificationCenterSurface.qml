pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var presentation
    required property var themeGeneration
    property real maximumHistoryHeight: 640
    readonly property bool serviceAvailable: presentation !== null
        && presentation !== undefined
        && presentation.serviceAvailable
    readonly property bool historyEnabled: serviceAvailable
        && presentation.historyModeId !== "disabled"
    readonly property bool canClear: historyEnabled && presentation.historyCount > 0
    readonly property string statusText: {
        if (!serviceAvailable)
            return qsTr("Notification history is unavailable.")
        if (!historyEnabled)
            return qsTr("Notification history is disabled.")
        if (presentation.historyCount === 0)
            return qsTr("No notifications in history.")
        return ""
    }

    signal closeRequested
    signal focusReturnRequested
    signal focusExitForward
    signal focusExitBackward

    function closeAndReturnFocus() {
        root.closeRequested()
        root.focusReturnRequested()
    }

    function focusFirstControl() {
        dndSwitch.forceActiveFocus(Qt.TabFocusReason)
    }

    function focusHistory(forward) {
        if (historySurface.count > 0) {
            if (forward)
                historySurface.focusFirstCard()
            else {
                const card = historySurface.cardAt(historySurface.count - 1)
                if (card !== null)
                    card.focusLastAvailable()
            }
        } else if (forward) {
            root.focusExitForward()
        } else if (clearButton.visible) {
            clearButton.forceActiveFocus(Qt.BacktabFocusReason)
        } else {
            dndSwitch.forceActiveFocus(Qt.BacktabFocusReason)
        }
    }

    implicitWidth: 460
    implicitHeight: Math.min(content.implicitHeight, maximumHistoryHeight)
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Notification center")
    Accessible.description: qsTr("Notification history and do not disturb controls")
    Accessible.focusable: true
    Accessible.focused: activeFocus

    Keys.onEscapePressed: function(event) {
        root.closeAndReturnFocus()
        event.accepted = true
    }
    Keys.onTabPressed: function(event) {
        root.focusFirstControl()
        event.accepted = true
    }
    Keys.onBacktabPressed: function(event) {
        root.focusHistory(false)
        event.accepted = true
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.visible
        onActivated: root.closeAndReturnFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: root.themeGeneration.notification.surfaceColor
        radius: root.themeGeneration.notification.cardRadius
        border.width: root.themeGeneration.notification.highContrast
            ? root.themeGeneration.notification.focusWidth
            : root.themeGeneration.notification.borderWidth
        border.color: root.themeGeneration.notification.borderColor
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: root.themeGeneration.notification.cardPadding
        spacing: Math.max(8, root.themeGeneration.notification.cardPadding / 2)

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: qsTr("Notifications")
                textFormat: Text.PlainText
                color: root.themeGeneration.notification.textPrimaryColor
                font.family: root.themeGeneration.notification.bodyFontFamily
                font.pixelSize: root.themeGeneration.notification.titleFontPixels
                font.bold: true
                Accessible.ignored: true
            }

            Switch {
                id: dndSwitch
                objectName: "notificationCenterDoNotDisturb"

                text: qsTr("Do not disturb")
                checked: root.serviceAvailable
                    && root.presentation.effectiveDoNotDisturb
                enabled: root.serviceAvailable
                activeFocusOnTab: false
                Accessible.role: Accessible.CheckBox
                Accessible.name: text
                Accessible.description: qsTr("Suppress non-critical transient notifications")
                Accessible.checked: checked
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onToggleAction: {
                    if (dndSwitch.enabled)
                        dndSwitch.toggle()
                }
                Keys.priority: Keys.BeforeItem
                onToggled: {
                    if (root.serviceAvailable
                            && checked !== root.presentation.effectiveDoNotDisturb)
                        root.presentation.requestDoNotDisturb(checked)
                }
                Keys.onTabPressed: function(event) {
                    if (clearButton.visible)
                        clearButton.forceActiveFocus(Qt.TabFocusReason)
                    else
                        root.focusHistory(true)
                    event.accepted = true
                }
                Keys.onBacktabPressed: function(event) {
                    root.focusExitBackward()
                    event.accepted = true
                }
                Keys.onEscapePressed: function(event) {
                    root.closeAndReturnFocus()
                    event.accepted = true
                }
            }

            Button {
                id: clearButton
                objectName: "notificationCenterClearHistory"

                visible: root.serviceAvailable
                    && root.presentation.historyCount > 0
                    && root.presentation.historyModeId !== "disabled"
                enabled: visible
                text: qsTr("Clear")
                activeFocusOnTab: false
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Clear notification history")
                Accessible.description: qsTr("Remove retained notifications while preserving live notifications")
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clearButton.clicked()
                onClicked: root.presentation.requestClearHistory()
                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: function(event) {
                    root.focusHistory(true)
                    event.accepted = true
                }
                Keys.onBacktabPressed: function(event) {
                    dndSwitch.forceActiveFocus(Qt.BacktabFocusReason)
                    event.accepted = true
                }
                Keys.onEscapePressed: function(event) {
                    root.closeAndReturnFocus()
                    event.accepted = true
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.serviceAvailable && root.presentation.effectiveDoNotDisturb
            text: qsTr("Do not disturb is active. Critical notifications may still appear.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.themeGeneration.notification.textMutedColor
            font.family: root.themeGeneration.notification.bodyFontFamily
            font.pixelSize: root.themeGeneration.notification.bodyFontPixels
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Text {
            id: emptyStatus
            objectName: "notificationCenterStatus"

            Layout.fillWidth: true
            Layout.minimumHeight: visible
                ? root.themeGeneration.notification.minimumTargetSize
                : 0
            visible: root.statusText.length > 0
            text: root.statusText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.themeGeneration.notification.textMutedColor
            font.family: root.themeGeneration.notification.bodyFontFamily
            font.pixelSize: root.themeGeneration.notification.bodyFontPixels
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ScrollView {
            id: historyScroll
            objectName: "notificationCenterHistoryScroll"

            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.serviceAvailable && root.presentation.historyCount > 0
            clip: true
            contentWidth: availableWidth
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Notification history")

            NotificationSurface {
                id: historySurface

                width: historyScroll.availableWidth
                presentationModel: root.presentation.historyModel
                themeGeneration: root.themeGeneration
                onFocusExitForward: root.focusExitForward()
                onFocusExitBackward: {
                    if (clearButton.visible)
                        clearButton.forceActiveFocus(Qt.BacktabFocusReason)
                    else
                        dndSwitch.forceActiveFocus(Qt.BacktabFocusReason)
                }
            }
        }
    }
}
