pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Control {
    id: root

    required property var presentation
    required property var tokens
    property bool rightToLeft: false

    readonly property string title: presentation !== null ? presentation.displayName : ""
    readonly property string statusText: presentation !== null ? presentation.statusText : ""
    readonly property bool activeState: presentation !== null && presentation.active
    readonly property bool urgentState: presentation !== null && presentation.urgent
    readonly property bool down: primaryTap.pressed
    readonly property bool actionMenuOpen: contextMenu.opened
    readonly property bool groupPopupOpen: groupPopup.opened
    readonly property var focusTarget: groupPopup.opened && groupPopup.memberAt(0) !== null
                                       ? groupPopup.memberAt(0) : root

    signal focusExitForward
    signal focusExitBackward
    signal actionMenuOpening
    signal actionMenuClosed
    signal clicked

    function openActionMenu() {
        if (presentation === null)
            return false
        groupPopup.dismiss()
        actionMenuOpening()
        contextMenu.open()
        if (openNewAction.enabled)
            openNewAction.forceActiveFocus(Qt.OtherFocusReason)
        else
            pinAction.forceActiveFocus(Qt.OtherFocusReason)
        return true
    }

    function closeActionMenu(restoreFocus) {
        contextMenu.close()
        if (restoreFocus)
            focusRestoreTimer.restart()
    }

    function closePopups(restoreFocus) {
        groupPopup.dismiss()
        closeActionMenu(restoreFocus)
    }

    function recoverFocus(memberIndex, reopenGroup) {
        if (reopenGroup && presentation !== null && presentation.primaryOpensGroup)
            return groupPopup.openForMember(Math.max(0, memberIndex))
        forceActiveFocus(Qt.OtherFocusReason)
        return true
    }

    function requestPrimary() {
        if (presentation === null)
            return false
        clicked()
        const accepted = presentation.requestPrimaryAction()
        if (accepted && presentation.primaryOpensGroup)
            groupPopup.openForMember(0)
        return accepted
    }

    objectName: "panelGroupedTaskButton"
    activeFocusOnTab: false
    hoverEnabled: true
    implicitWidth: Math.max(tokens.minimumTargetSize,
                            taskContent.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(tokens.minimumTargetSize, tokens.panelHeight)
    leftPadding: tokens.taskPadding
    rightPadding: tokens.taskPadding
    topPadding: 4
    bottomPadding: 4

    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.description: groupPopupOpen
                            ? qsTr("Application group %1. Status: %2. Window list shown.")
                                .arg(title).arg(statusText)
                            : qsTr("Application group %1. Status: %2.")
                                .arg(title).arg(statusText)
    Accessible.focusable: true
    Accessible.focused: activeFocus
    Accessible.checkable: true
    Accessible.checked: activeState
    Accessible.pressed: down
    Accessible.onPressAction: root.requestPrimary()

    Keys.priority: Keys.BeforeItem
    Keys.onTabPressed: event => {
        event.accepted = true
        root.focusExitForward()
    }
    Keys.onBacktabPressed: event => {
        event.accepted = true
        root.focusExitBackward()
    }
    Keys.onLeftPressed: event => {
        event.accepted = true
        if (root.rightToLeft)
            root.focusExitForward()
        else
            root.focusExitBackward()
    }
    Keys.onRightPressed: event => {
        event.accepted = true
        if (root.rightToLeft)
            root.focusExitBackward()
        else
            root.focusExitForward()
    }
    Keys.onSpacePressed: event => {
        event.accepted = root.requestPrimary()
    }
    Keys.onReturnPressed: event => {
        event.accepted = root.requestPrimary()
    }
    Keys.onEnterPressed: event => {
        event.accepted = root.requestPrimary()
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier) !== 0)) {
            event.accepted = root.openActionMenu()
        }
    }

    onPresentationChanged: closePopups(false)

    Timer {
        id: focusRestoreTimer
        interval: 0
        repeat: false
        onTriggered: root.forceActiveFocus(Qt.OtherFocusReason)
    }

    contentItem: Row {
        id: taskContent
        spacing: Math.max(2, root.tokens.taskPadding / 2)

        Item {
            width: root.tokens.iconSize
            height: root.tokens.iconSize
            anchors.verticalCenter: parent.verticalCenter
            Accessible.ignored: true

            Rectangle {
                anchors.fill: parent
                radius: Math.max(1, root.tokens.taskRadius / 3)
                color: "transparent"
                border.width: Math.max(1, root.tokens.taskBorderWidth)
                border.color: root.urgentState ? root.tokens.warningColor
                                                : root.tokens.textMutedColor
            }

            Text {
                anchors.centerIn: parent
                color: root.tokens.textPrimaryColor
                text: root.presentation !== null && root.presentation.windowCount > 1
                      ? root.presentation.windowCount : ""
                font.bold: true
                textFormat: Text.PlainText
            }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter

            Text {
                color: root.tokens.textPrimaryColor
                text: root.title
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: root.tokens.bodyFontFamily
                font.pixelSize: root.tokens.bodyFontPixels
                font.bold: root.activeState || root.urgentState
            }

            Text {
                color: root.urgentState ? root.tokens.warningColor : root.tokens.textMutedColor
                text: root.statusText
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: root.tokens.bodyFontFamily
                font.pixelSize: Math.max(10, root.tokens.bodyFontPixels - 2)
                font.bold: root.urgentState
            }
        }
    }

    background: Rectangle {
        radius: root.urgentState ? Math.max(0, root.tokens.taskRadius / 2)
                                 : root.tokens.taskRadius
        color: root.down || root.activeState ? root.tokens.selectionColor : "transparent"
        border.width: root.activeFocus ? root.tokens.focusWidth
                                       : (root.activeState || root.urgentState
                                          ? Math.max(root.tokens.taskBorderWidth,
                                                     root.tokens.borderWidth)
                                          : root.tokens.taskBorderWidth)
        border.color: root.activeFocus ? root.tokens.focusColor
                                       : (root.urgentState ? root.tokens.warningColor
                                                           : root.tokens.inactiveBorderColor)

        Rectangle {
            visible: root.urgentState
            width: Math.max(5, root.tokens.taskBorderWidth * 2)
            height: parent.height - (2 * root.tokens.taskPadding)
            anchors.left: parent.left
            anchors.leftMargin: root.tokens.taskPadding / 2
            anchors.verticalCenter: parent.verticalCenter
            radius: width / 2
            color: root.tokens.warningColor
        }
    }

    TapHandler {
        id: primaryTap
        acceptedButtons: Qt.LeftButton
        onTapped: root.requestPrimary()
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.openActionMenu()
    }

    Popup {
        id: contextMenu
        objectName: "panelGroupedTaskContextMenu"
        focus: true
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: root.tokens.menuItemPadding
        onClosed: root.actionMenuClosed()

        background: Rectangle {
            color: root.tokens.surfaceColor
            radius: root.tokens.menuItemRadius
            border.width: root.tokens.menuItemBorderWidth
            border.color: root.tokens.activeBorderColor
        }

        contentItem: Row {
            spacing: root.tokens.menuItemPadding

            Accessible.role: Accessible.PopupMenu
            Accessible.name: qsTr("Application actions for %1").arg(root.title)
            Accessible.description: qsTr("Open a new window or change the pinned state")

            Button {
                id: openNewAction
                objectName: "panelGroupedTaskOpenNew"
                text: qsTr("Open new")
                enabled: root.presentation !== null && root.presentation.launchAvailable
                implicitWidth: root.tokens.minimumTargetSize
                implicitHeight: root.tokens.minimumTargetSize
                Accessible.role: Accessible.MenuItem
                Accessible.name: qsTr("Open a new %1 window").arg(root.title)
                Accessible.description: qsTr("Launch the associated desktop application")
                onClicked: {
                    if (root.presentation.requestOpenNew())
                        root.closeActionMenu(true)
                }
            }

            Button {
                id: pinAction
                objectName: "panelGroupedTaskPinToggle"
                text: root.presentation !== null && root.presentation.pinned
                      ? qsTr("Unpin") : qsTr("Pin")
                enabled: root.presentation !== null
                         && (root.presentation.pinned || root.presentation.launchAvailable)
                implicitWidth: root.tokens.minimumTargetSize
                implicitHeight: root.tokens.minimumTargetSize
                Accessible.role: Accessible.MenuItem
                Accessible.name: root.presentation !== null && root.presentation.pinned
                                 ? qsTr("Unpin %1").arg(root.title)
                                 : qsTr("Pin %1").arg(root.title)
                Accessible.description: qsTr("Change the shared panel pin order")
                onClicked: {
                    if (root.presentation.requestPinToggle())
                        root.closeActionMenu(true)
                }
            }

            Button {
                id: closeAction
                objectName: "panelGroupedTaskClose"
                text: qsTr("Close")
                visible: root.presentation !== null && root.presentation.windowCount === 1
                enabled: visible
                implicitWidth: root.tokens.minimumTargetSize
                implicitHeight: root.tokens.minimumTargetSize
                Accessible.role: Accessible.MenuItem
                Accessible.name: qsTr("Close %1").arg(root.title)
                Accessible.description: qsTr("Request that the only application window close")
                Accessible.onPressAction: closeAction.clicked()
                onClicked: {
                    if (root.presentation.requestOnlyWindowClose())
                        root.closeActionMenu(true)
                }
            }
        }
    }

    TaskGroupPopup {
        id: groupPopup
        presentation: root.presentation
        tokens: root.tokens
        rightToLeft: root.rightToLeft
        onClosedToOrigin: focusRestoreTimer.restart()
    }
}
