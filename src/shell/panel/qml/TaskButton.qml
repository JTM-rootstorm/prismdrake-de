pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Control {
    id: root

    required property var presentation
    required property var tokens

    readonly property string presentationTitle: presentation !== null ? presentation.title : ""
    readonly property string presentationApplicationId: presentation !== null
                                                               ? presentation.applicationId : ""
    readonly property string presentationFallbackIconName: presentation !== null
                                                                   ? presentation.fallbackIconName
                                                                   : ""
    readonly property string presentationStatusText: presentation !== null
                                                            ? presentation.statusText : ""
    readonly property bool presentationActive: presentation !== null && presentation.active
    readonly property bool presentationMinimized: presentation !== null && presentation.minimized
    readonly property bool presentationUrgent: presentation !== null && presentation.urgent
    readonly property bool urgentState: presentationUrgent
    readonly property bool down: taskPointerArea.pressed
                                 && (taskPointerArea.pressedButtons & Qt.LeftButton) !== 0
    readonly property string text: presentationTitle
    readonly property bool actionMenuOpen: taskActionMenu.opened
    readonly property var actionTarget: taskActionMenu.actionTarget
    readonly property string accessibleDescriptionBase: qsTr("Generic application icon. %1. %2")
                                                            .arg(presentationApplicationId)
                                                            .arg(presentationStatusText)
    readonly property string accessibleDescription: actionMenuOpen
                                                    ? qsTr("%1 Window actions shown.")
                                                        .arg(accessibleDescriptionBase)
                                                    : accessibleDescriptionBase

    signal focusExitForward
    signal focusExitBackward
    signal actionMenuOpening
    signal clicked

    function openActionMenu() {
        if (presentation === null)
            return false
        actionFocusRestoreTimer.stop()
        actionMenuOpening()
        taskActionMenu.actionTarget = presentation
        taskActionMenu.opened = true
        if (minimizeAction.enabled)
            minimizeAction.forceActiveFocus(Qt.OtherFocusReason)
        else
            closeAction.forceActiveFocus(Qt.OtherFocusReason)
        return true
    }

    function closeActionMenu(restoreFocus) {
        taskActionMenu.opened = false
        taskActionMenu.actionTarget = null
        if (restoreFocus)
            actionFocusRestoreTimer.restart()
        else
            actionFocusRestoreTimer.stop()
    }

    function restoreActionOriginFocus() {
        if (!taskActionMenu.opened && presentation !== null)
            forceActiveFocus(Qt.OtherFocusReason)
    }

    function requestMenuMinimization() {
        const target = taskActionMenu.actionTarget
        if (!taskActionMenu.opened || target === null || target !== presentation
                || target.minimized) {
            closeActionMenu(false)
            return false
        }
        const accepted = target.requestMinimization()
        closeActionMenu(true)
        return accepted
    }

    function requestMenuClose() {
        const target = taskActionMenu.actionTarget
        if (!taskActionMenu.opened || target === null || target !== presentation) {
            closeActionMenu(false)
            return false
        }
        const accepted = target.requestClose()
        closeActionMenu(true)
        return accepted
    }

    function handlePointerPress(button) {
        if (button === Qt.RightButton)
            return openActionMenu()
        return false
    }

    function moveActionFocus(current, reason) {
        if (!minimizeAction.enabled) {
            closeAction.forceActiveFocus(reason)
            return
        }
        if (current === minimizeAction)
            closeAction.forceActiveFocus(reason)
        else
            minimizeAction.forceActiveFocus(reason)
    }

    function handleActionKey(event, current) {
        if (event.key === Qt.Key_Escape) {
            closeActionMenu(true)
            event.accepted = true
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Right) {
            moveActionFocus(current, Qt.TabFocusReason)
            event.accepted = true
        } else if (event.key === Qt.Key_Backtab || event.key === Qt.Key_Left) {
            moveActionFocus(current, Qt.BacktabFocusReason)
            event.accepted = true
        } else if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                   || event.key === Qt.Key_Enter) {
            if (current === minimizeAction)
                requestMenuMinimization()
            else
                requestMenuClose()
            event.accepted = true
        }
    }

    objectName: "panelTaskButton"
    activeFocusOnTab: false
    hoverEnabled: true
    implicitWidth: Math.max(tokens.minimumTargetSize,
                            taskStack.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(tokens.minimumTargetSize, tokens.panelHeight)
    leftPadding: tokens.taskPadding
    rightPadding: tokens.taskPadding
    topPadding: 4
    bottomPadding: 4

    Accessible.role: Accessible.Button
    Accessible.name: presentationTitle
    Accessible.description: accessibleDescription
    Accessible.focusable: true
    Accessible.focused: activeFocus
    Accessible.checkable: true
    Accessible.checked: presentationActive
    Accessible.onPressAction: root.clicked()

    Keys.priority: Keys.BeforeItem
    Keys.onTabPressed: event => {
        event.accepted = true
        root.focusExitForward()
    }
    Keys.onBacktabPressed: event => {
        event.accepted = true
        root.focusExitBackward()
    }
    Keys.onSpacePressed: event => {
        event.accepted = true
        root.clicked()
    }
    Keys.onReturnPressed: event => {
        event.accepted = true
        root.clicked()
    }
    Keys.onEnterPressed: event => {
        event.accepted = true
        root.clicked()
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier) !== 0)) {
            event.accepted = root.openActionMenu()
        }
    }

    onClicked: {
        if (presentation !== null)
            presentation.requestActivation()
    }
    onPresentationChanged: closeActionMenu(false)

    Timer {
        id: actionFocusRestoreTimer
        interval: 1
        repeat: false
        onTriggered: root.restoreActionOriginFocus()
    }

    Item {
        id: taskStack
        implicitWidth: Math.max(taskContent.implicitWidth, actionRow.implicitWidth)
        implicitHeight: Math.max(taskContent.implicitHeight, actionRow.implicitHeight)

        Item {
            id: taskContent
            objectName: "panelTaskPresentationContent"
            anchors.fill: parent
            implicitWidth: root.tokens.iconSize
                           + Math.max(2, root.tokens.taskPadding / 2)
                           + Math.max(taskTitle.implicitWidth, taskState.implicitWidth)
            implicitHeight: presentationContent.implicitHeight
            visible: !root.actionMenuOpen

            Row {
                id: presentationContent
                anchors.fill: parent
                spacing: Math.max(2, root.tokens.taskPadding / 2)

                Item {
                    id: fallbackIcon
                    objectName: "panelTaskFallbackIcon"
                    property string iconName: root.presentationFallbackIconName
                    width: root.tokens.iconSize
                    height: root.tokens.iconSize
                    anchors.verticalCenter: parent.verticalCenter
                    Accessible.ignored: true

                    Rectangle {
                        anchors.fill: parent
                        radius: Math.max(1, root.tokens.taskRadius / 3)
                        color: "transparent"
                        border.width: Math.max(1, root.tokens.taskBorderWidth)
                        border.color: root.presentationUrgent
                                      ? root.tokens.warningColor : root.tokens.textMutedColor
                    }

                    Rectangle {
                        x: Math.max(2, root.tokens.taskBorderWidth * 2)
                        y: x
                        width: parent.width - (2 * x)
                        height: Math.max(2, root.tokens.taskBorderWidth * 2)
                        radius: height / 2
                        color: root.presentationUrgent
                               ? root.tokens.warningColor : root.tokens.textMutedColor
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: Math.max(2, root.tokens.taskBorderWidth * 2)
                        height: Math.max(3, parent.height / 4)
                        radius: Math.max(1, root.tokens.taskRadius / 4)
                        color: root.presentationActive
                               ? root.tokens.selectionColor : root.tokens.surfaceColor
                        border.width: Math.max(1, root.tokens.taskBorderWidth)
                        border.color: root.tokens.activeBorderColor
                    }
                }

                Column {
                    width: Math.max(0, root.availableWidth - fallbackIcon.width - parent.spacing)
                    spacing: 1
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        id: taskTitle
                        objectName: "panelTaskTitle"
                        width: parent.width
                        color: root.tokens.textPrimaryColor
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.family: root.tokens.bodyFontFamily
                        font.pixelSize: root.tokens.bodyFontPixels
                        font.bold: root.presentationActive || root.presentationUrgent
                        horizontalAlignment: Text.AlignHCenter
                        text: root.presentationTitle
                    }

                    Text {
                        id: taskState
                        objectName: "panelTaskStateLabel"
                        width: parent.width
                        color: root.presentationUrgent
                               ? root.tokens.warningColor : root.tokens.textMutedColor
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.family: root.tokens.bodyFontFamily
                        font.pixelSize: Math.max(10, root.tokens.bodyFontPixels - 2)
                        font.bold: root.presentationUrgent
                        horizontalAlignment: Text.AlignHCenter
                        text: root.presentationStatusText
                    }
                }
            }

            MouseArea {
                id: taskPointerArea
                objectName: "panelTaskPointerArea"
                anchors.fill: parent
                enabled: !root.actionMenuOpen
                acceptedButtons: Qt.LeftButton
                onClicked: event => {
                    root.clicked()
                    event.accepted = true
                }
            }
        }

        TapHandler {
            enabled: !root.actionMenuOpen
            acceptedButtons: Qt.RightButton
            onTapped: root.handlePointerPress(Qt.RightButton)
        }
    }

    contentItem: taskStack

    background: Rectangle {
        radius: root.urgentState ? Math.max(0, root.tokens.taskRadius / 2)
                                 : root.tokens.taskRadius
        color: root.down
               ? root.tokens.selectionColor
               : (root.presentationActive ? root.tokens.selectionColor : "transparent")
        border.width: root.activeFocus
                      ? root.tokens.focusWidth
                      : (root.presentationActive || root.presentationUrgent
                         ? Math.max(root.tokens.taskBorderWidth, root.tokens.borderWidth)
                         : root.tokens.taskBorderWidth)
        border.color: root.activeFocus
                      ? root.tokens.focusColor
                      : (root.presentationUrgent
                         ? root.tokens.warningColor
                         : (root.presentationActive
                            ? root.tokens.activeBorderColor
                            : root.tokens.inactiveBorderColor))

        Rectangle {
            objectName: "panelTaskUrgentMarker"
            visible: root.presentationUrgent
            width: Math.max(5, root.tokens.taskBorderWidth * 2)
            height: parent.height - (2 * root.tokens.taskPadding)
            anchors.left: parent.left
            anchors.leftMargin: root.tokens.taskPadding / 2
            anchors.verticalCenter: parent.verticalCenter
            radius: width / 2
            color: root.tokens.warningColor
        }
    }

    FocusScope {
        id: taskActionMenu
        objectName: "panelTaskContextMenu"
        parent: taskStack
        z: 20
        x: -root.leftPadding
        y: -root.topPadding
        width: root.width
        height: root.height
        visible: opened
        enabled: visible
        property bool opened: false
        property var actionTarget: null

        Accessible.role: Accessible.PopupMenu
        Accessible.name: qsTr("Window actions for %1").arg(root.presentationTitle)
        Accessible.description: qsTr("Minimize or close this window")

        Rectangle {
            anchors.fill: parent
            radius: root.tokens.menuItemRadius
            color: root.tokens.surfaceColor
            border.width: root.tokens.menuItemBorderWidth
            border.color: root.tokens.activeBorderColor
        }

        Row {
            id: actionRow
            objectName: "panelTaskActionRow"
            anchors.centerIn: parent
            spacing: root.tokens.menuItemPadding

            Button {
                id: minimizeAction
                objectName: "panelTaskMinimizeAction"
                enabled: root.actionTarget !== null
                         && root.actionTarget === root.presentation
                         && !root.actionTarget.minimized
                activeFocusOnTab: false
                text: qsTr("Minimize")
                implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                        contentItem.implicitWidth + leftPadding + rightPadding)
                implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                         contentItem.implicitHeight + topPadding + bottomPadding)
                leftPadding: root.tokens.menuItemPadding
                rightPadding: root.tokens.menuItemPadding
                topPadding: root.tokens.menuItemPadding
                bottomPadding: root.tokens.menuItemPadding

                Accessible.role: Accessible.MenuItem
                Accessible.name: qsTr("Minimize %1").arg(root.presentationTitle)
                Accessible.description: qsTr("Request that the window manager minimize %1")
                                            .arg(root.presentationTitle)
                Accessible.focusable: enabled
                Accessible.focused: activeFocus

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: event => root.handleActionKey(event, minimizeAction)
                onClicked: root.requestMenuMinimization()

                contentItem: Text {
                    color: minimizeAction.enabled
                           ? root.tokens.textPrimaryColor : root.tokens.textMutedColor
                    textFormat: Text.PlainText
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: minimizeAction.text
                }
                background: Rectangle {
                    radius: root.tokens.menuItemRadius
                    color: minimizeAction.down
                           ? root.tokens.selectionColor : root.tokens.surfaceColor
                    border.width: minimizeAction.activeFocus
                                  ? root.tokens.focusWidth : root.tokens.menuItemBorderWidth
                    border.color: minimizeAction.activeFocus
                                  ? root.tokens.focusColor : root.tokens.inactiveBorderColor
                }
            }

            Button {
                id: closeAction
                objectName: "panelTaskCloseAction"
                enabled: root.actionTarget !== null
                         && root.actionTarget === root.presentation
                activeFocusOnTab: false
                text: qsTr("Close")
                implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                        contentItem.implicitWidth + leftPadding + rightPadding)
                implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                         contentItem.implicitHeight + topPadding + bottomPadding)
                leftPadding: root.tokens.menuItemPadding
                rightPadding: root.tokens.menuItemPadding
                topPadding: root.tokens.menuItemPadding
                bottomPadding: root.tokens.menuItemPadding

                Accessible.role: Accessible.MenuItem
                Accessible.name: qsTr("Close %1").arg(root.presentationTitle)
                Accessible.description: qsTr("Request that the window manager close %1")
                                            .arg(root.presentationTitle)
                Accessible.focusable: enabled
                Accessible.focused: activeFocus

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: event => root.handleActionKey(event, closeAction)
                onClicked: root.requestMenuClose()

                contentItem: Text {
                    color: root.tokens.textPrimaryColor
                    textFormat: Text.PlainText
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: closeAction.text
                }
                background: Rectangle {
                    radius: root.tokens.menuItemRadius
                    color: closeAction.down
                           ? root.tokens.selectionColor : root.tokens.surfaceColor
                    border.width: closeAction.activeFocus
                                  ? root.tokens.focusWidth : root.tokens.menuItemBorderWidth
                    border.color: closeAction.activeFocus
                                  ? root.tokens.focusColor : root.tokens.inactiveBorderColor
                }
            }
        }
    }
}
