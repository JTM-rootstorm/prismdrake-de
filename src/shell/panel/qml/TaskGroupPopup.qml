pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: root
    objectName: "panelTaskGroupPopup"

    required property var presentation
    required property var tokens
    property int pendingMemberFocus: 0
    property bool restoreFocusWhenClosed: true

    signal closedToOrigin

    function memberAt(index) {
        return memberRepeater.itemAt(index)
    }

    function openForMember(index) {
        if (presentation === null || presentation.members.length < 2)
            return false
        pendingMemberFocus = Math.max(0, Math.min(index, presentation.members.length - 1))
        restoreFocusWhenClosed = true
        open()
        focusTimer.restart()
        return true
    }

    function closeToOrigin() {
        restoreFocusWhenClosed = true
        close()
    }

    function dismiss() {
        restoreFocusWhenClosed = false
        close()
    }

    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: tokens.menuItemPadding

    onClosed: {
        if (restoreFocusWhenClosed)
            closedToOrigin()
        restoreFocusWhenClosed = true
    }

    Timer {
        id: focusTimer
        interval: 0
        repeat: false
        onTriggered: {
            const candidate = root.memberAt(root.pendingMemberFocus)
            if (candidate !== null)
                candidate.forceActiveFocus(Qt.OtherFocusReason)
        }
    }

    background: Rectangle {
        color: root.tokens.surfaceColor
        radius: root.tokens.menuItemRadius
        border.width: root.tokens.menuItemBorderWidth
        border.color: root.tokens.activeBorderColor
    }

    contentItem: Column {
        spacing: root.tokens.menuItemPadding

        Accessible.role: Accessible.PopupMenu
        Accessible.name: qsTr("Windows for %1").arg(root.presentation !== null
                                                       ? root.presentation.displayName : "")
        Accessible.description: qsTr("Choose a window, then activate, minimize, or close it")

        Repeater {
            id: memberRepeater
            model: root.presentation !== null ? root.presentation.members : []

            delegate: FocusScope {
                id: memberRow
                objectName: "panelTaskGroupMember"
                required property int index
                required property var modelData

                readonly property var member: modelData
                width: Math.max(320, memberActions.implicitWidth)
                height: Math.max(root.tokens.minimumTargetSize, memberActions.implicitHeight)

                Accessible.role: Accessible.ListItem
                Accessible.name: member !== null ? member.title : ""
                Accessible.description: member !== null ? member.statusText : ""
                Accessible.focusable: true
                Accessible.focused: activeFocus

                Keys.priority: Keys.BeforeItem
                Keys.onEscapePressed: event => {
                    event.accepted = true
                    root.closeToOrigin()
                }
                Keys.onUpPressed: event => {
                    event.accepted = true
                    const candidate = root.memberAt((index - 1 + memberRepeater.count)
                                                    % memberRepeater.count)
                    if (candidate !== null)
                        candidate.forceActiveFocus(Qt.BacktabFocusReason)
                }
                Keys.onDownPressed: event => {
                    event.accepted = true
                    const candidate = root.memberAt((index + 1) % memberRepeater.count)
                    if (candidate !== null)
                        candidate.forceActiveFocus(Qt.TabFocusReason)
                }

                Row {
                    id: memberActions
                    anchors.fill: parent
                    spacing: root.tokens.menuItemPadding

                    Label {
                        width: Math.max(120, parent.width
                                        - activate.width - minimize.width - closeWindow.width
                                        - (3 * parent.spacing))
                        height: parent.height
                        text: memberRow.member !== null ? memberRow.member.title : ""
                        color: root.tokens.textPrimaryColor
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignVCenter
                        font.family: root.tokens.bodyFontFamily
                        font.pixelSize: root.tokens.bodyFontPixels
                    }

                    Button {
                        id: activate
                        objectName: "panelTaskGroupActivate"
                        text: qsTr("Activate")
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Activate %1").arg(memberRow.member !== null
                                                                   ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window manager activate this window")
                        onClicked: {
                            if (memberRow.member !== null
                                    && memberRow.member.requestActivation())
                                root.closeToOrigin()
                        }
                    }

                    Button {
                        id: minimize
                        objectName: "panelTaskGroupMinimize"
                        text: qsTr("Minimize")
                        enabled: memberRow.member !== null && !memberRow.member.minimized
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Minimize %1").arg(memberRow.member !== null
                                                                   ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window manager minimize this window")
                        onClicked: {
                            if (memberRow.member !== null
                                    && memberRow.member.requestMinimization())
                                root.closeToOrigin()
                        }
                    }

                    Button {
                        id: closeWindow
                        objectName: "panelTaskGroupClose"
                        text: qsTr("Close")
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Close %1").arg(memberRow.member !== null
                                                                ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window close")
                        onClicked: {
                            if (memberRow.member !== null && memberRow.member.requestClose())
                                root.closeToOrigin()
                        }
                    }
                }
            }
        }
    }
}
