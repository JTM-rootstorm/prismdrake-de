pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    required property string statusText
    required property bool transactionPending
    property int layoutDirection: Qt.locale().textDirection

    readonly property var tokens: themeGeneration.notification
    readonly property real maximumStatusHeight: Math.max(tokens.minimumTargetSize,
                                                          tokens.bodyFontPixels * 5)

    signal confirmRequested
    signal cancelRequested

    function focusFirstControl() {
        if (cancelButton.enabled)
            cancelButton.forceActiveFocus(Qt.OtherFocusReason)
        else if (confirmButton.enabled)
            confirmButton.forceActiveFocus(Qt.OtherFocusReason)
    }

    function focusOtherControl(current, reason, event) {
        event.accepted = true
        const candidate = current === cancelButton ? confirmButton : cancelButton
        if (candidate.enabled)
            candidate.forceActiveFocus(reason)
    }

    function activateControl(control, event) {
        event.accepted = true
        if (control.enabled)
            control.clicked()
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
    LayoutMirroring.enabled: layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    Rectangle {
        objectName: "logoutConfirmationBackground"
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
            objectName: "logoutHeading"
            Layout.fillWidth: true
            text: qsTr("Log out?")
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.titleFontPixels
            font.bold: true
        }

        Label {
            objectName: "logoutStatus"
            Layout.fillWidth: true
            text: root.statusText
            color: root.tokens.textMutedColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
            Layout.maximumHeight: root.maximumStatusHeight
        }

        RowLayout {
            layoutDirection: root.layoutDirection
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: false
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
                Keys.priority: Keys.BeforeItem

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Cancel logout")
                Accessible.description: qsTr("Return to the current Prismdrake session")
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.onTabPressed: event => {
                    root.focusOtherControl(cancelButton, Qt.TabFocusReason, event)
                }
                Keys.onBacktabPressed: event => {
                    root.focusOtherControl(cancelButton, Qt.BacktabFocusReason, event)
                }
                Keys.onLeftPressed: event => {
                    root.focusOtherControl(cancelButton, Qt.BacktabFocusReason, event)
                }
                Keys.onRightPressed: event => {
                    root.focusOtherControl(cancelButton, Qt.TabFocusReason, event)
                }
                Keys.onSpacePressed: event => root.activateControl(cancelButton, event)
                Keys.onReturnPressed: event => root.activateControl(cancelButton, event)
                Keys.onEnterPressed: event => root.activateControl(cancelButton, event)

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
                Keys.priority: Keys.BeforeItem

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Confirm logout")
                Accessible.description: qsTr("Close applications and end the Prismdrake session")
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.defaultButton: true
                Accessible.onPressAction: clicked()

                Keys.onTabPressed: event => {
                    root.focusOtherControl(confirmButton, Qt.TabFocusReason, event)
                }
                Keys.onBacktabPressed: event => {
                    root.focusOtherControl(confirmButton, Qt.BacktabFocusReason, event)
                }
                Keys.onLeftPressed: event => {
                    root.focusOtherControl(confirmButton, Qt.BacktabFocusReason, event)
                }
                Keys.onRightPressed: event => {
                    root.focusOtherControl(confirmButton, Qt.TabFocusReason, event)
                }
                Keys.onSpacePressed: event => root.activateControl(confirmButton, event)
                Keys.onReturnPressed: event => root.activateControl(confirmButton, event)
                Keys.onEnterPressed: event => root.activateControl(confirmButton, event)

                onClicked: root.confirmRequested()
            }
        }
    }
}
