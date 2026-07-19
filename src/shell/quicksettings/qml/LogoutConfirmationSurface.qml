pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    required property string statusText
    required property bool transactionPending

    readonly property var tokens: themeGeneration.notification

    signal confirmRequested
    signal cancelRequested

    function focusFirstControl() {
        if (cancelButton.enabled)
            cancelButton.forceActiveFocus(Qt.OtherFocusReason)
        else if (confirmButton.enabled)
            confirmButton.forceActiveFocus(Qt.OtherFocusReason)
    }

    Keys.priority: Keys.BeforeItem
    Keys.onEscapePressed: event => {
        event.accepted = true
        if (cancelButton.enabled)
            root.cancelRequested()
    }

    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Log out of Prismdrake")
    Accessible.description: statusText

    implicitWidth: 420
    implicitHeight: contents.implicitHeight + 32

    Rectangle {
        anchors.fill: parent
        color: root.tokens.surfaceColor
        border.color: root.tokens.borderColor
        border.width: root.tokens.borderWidth
        radius: root.tokens.cardRadius
    }

    ColumnLayout {
        id: contents
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Log out?")
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.titleFontPixels
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: root.statusText
            color: root.tokens.textMutedColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                id: cancelButton
                objectName: "logoutCancel"
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                        implicitContentWidth + leftPadding + rightPadding)
                text: qsTr("Cancel")
                enabled: !root.transactionPending
                activeFocusOnTab: true

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Cancel logout")
                Accessible.description: qsTr("Return to the current Prismdrake session")
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.onTabPressed: event => {
                    event.accepted = true
                    confirmButton.forceActiveFocus(Qt.TabFocusReason)
                }

                onClicked: root.cancelRequested()
            }

            Button {
                id: confirmButton
                objectName: "logoutConfirm"
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                        implicitContentWidth + leftPadding + rightPadding)
                text: qsTr("Log out")
                enabled: !root.transactionPending
                activeFocusOnTab: true

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Confirm logout")
                Accessible.description: qsTr("Close applications and end the Prismdrake session")
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.onBacktabPressed: event => {
                    event.accepted = true
                    cancelButton.forceActiveFocus(Qt.BacktabFocusReason)
                }

                onClicked: root.confirmRequested()
            }
        }
    }
}
