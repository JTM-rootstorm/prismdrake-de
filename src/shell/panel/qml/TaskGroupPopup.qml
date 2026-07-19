pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: root
    objectName: "panelTaskGroupPopup"

    required property var presentation
    required property var tokens
    required property real maximumWidth
    property bool rightToLeft: false
    property int pendingMemberFocus: 0
    property bool restoreFocusWhenClosed: true

    signal closedToOrigin

    function memberAt(index) {
        return memberRepeater.itemAt(index)
    }

    function revealMember(item) {
        if (item === null || memberViewport.contentItem === null)
            return
        const viewport = memberViewport.contentItem as Flickable
        if (viewport === null)
            return
        const position = item.mapToItem(memberRowStrip, 0, 0)
        const itemLeft = position.x
        const itemRight = itemLeft + item.width
        if (itemLeft < viewport.contentX) {
            viewport.contentX = Math.max(0, itemLeft)
        } else if (itemRight > viewport.contentX + viewport.width) {
            viewport.contentX = Math.min(
                        Math.max(0, memberRowStrip.implicitWidth - viewport.width),
                        itemRight - viewport.width)
        }
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
    padding: 0
    width: Math.min(memberRowStrip.implicitWidth,
                    Math.max(tokens.minimumTargetSize, maximumWidth))
    height: Math.max(tokens.minimumTargetSize, memberRowStrip.implicitHeight)

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
            if (candidate !== null) {
                candidate.forceActiveFocus(Qt.OtherFocusReason)
                Qt.callLater(root.revealMember, candidate)
            }
        }
    }

    background: Rectangle {
        color: root.tokens.surfaceColor
        radius: root.tokens.menuItemRadius
        border.width: root.tokens.menuItemBorderWidth
        border.color: root.tokens.activeBorderColor
    }

    contentItem: ScrollView {
        id: memberViewport
        objectName: "panelTaskGroupViewport"
        clip: true
        contentWidth: memberRowStrip.implicitWidth
        contentHeight: availableHeight
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AlwaysOff

        Accessible.role: Accessible.PopupMenu
        Accessible.name: qsTr("Windows for %1").arg(root.presentation !== null
                                                       ? root.presentation.displayName : "")
        Accessible.description: qsTr("Choose a window, then activate, minimize, or close it")

        Row {
            id: memberRowStrip
            height: memberViewport.availableHeight
            spacing: root.tokens.menuItemPadding
            LayoutMirroring.enabled: root.rightToLeft
            LayoutMirroring.childrenInherit: true

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

                function actionAt(actionIndex) {
                    switch (actionIndex) {
                    case 0: return activate
                    case 1: return minimize
                    case 2: return closeWindow
                    default: return null
                    }
                }

                function focusAction(actionIndex, direction, reason) {
                    for (let candidateIndex = actionIndex;
                         candidateIndex >= 0 && candidateIndex < 3;
                         candidateIndex += direction) {
                        const candidate = actionAt(candidateIndex)
                        if (candidate !== null && candidate.enabled) {
                            candidate.forceActiveFocus(reason)
                            return true
                        }
                    }
                    return false
                }

                function focusAdjacentMember(direction, actionIndex, reason) {
                    const candidate = root.memberAt((index + direction + memberRepeater.count)
                                                    % memberRepeater.count)
                    if (candidate === null)
                        return
                    if (actionIndex >= 0
                            && candidate.focusAction(actionIndex, direction > 0 ? 1 : -1,
                                                     reason)) {
                        root.revealMember(candidate)
                        return
                    }
                    candidate.forceActiveFocus(reason)
                    root.revealMember(candidate)
                }

                function moveFromAction(actionIndex, direction) {
                    const reason = direction > 0 ? Qt.TabFocusReason : Qt.BacktabFocusReason
                    if (focusAction(actionIndex + direction, direction, reason))
                        return
                    if (direction < 0) {
                        forceActiveFocus(reason)
                        return
                    }
                    focusAdjacentMember(1, -1, reason)
                }

                function moveActionHorizontally(actionIndex, visualDirection) {
                    const logicalDirection = root.rightToLeft ? -visualDirection : visualDirection
                    moveFromAction(actionIndex, logicalDirection)
                }

                Accessible.role: Accessible.ListItem
                Accessible.name: member !== null ? member.title : ""
                Accessible.description: member !== null
                    ? qsTr("Window %1. Status: %2. Press Enter to activate or Menu for actions.")
                        .arg(member.title).arg(member.statusText)
                    : ""
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
                    if (candidate !== null) {
                        candidate.forceActiveFocus(Qt.BacktabFocusReason)
                        root.revealMember(candidate)
                    }
                }
                Keys.onDownPressed: event => {
                    event.accepted = true
                    const candidate = root.memberAt((index + 1) % memberRepeater.count)
                    if (candidate !== null) {
                        candidate.forceActiveFocus(Qt.TabFocusReason)
                        root.revealMember(candidate)
                    }
                }
                Keys.onTabPressed: event => {
                    event.accepted = true
                    memberRow.focusAction(0, 1, Qt.TabFocusReason)
                }
                Keys.onBacktabPressed: event => {
                    event.accepted = true
                    memberRow.focusAdjacentMember(-1, -1, Qt.BacktabFocusReason)
                }
                Keys.onReturnPressed: event => {
                    event.accepted = true
                    activate.clicked()
                }
                Keys.onEnterPressed: event => {
                    event.accepted = true
                    activate.clicked()
                }
                Keys.onSpacePressed: event => {
                    event.accepted = true
                    activate.clicked()
                }
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Menu
                            || (event.key === Qt.Key_F10
                                && (event.modifiers & Qt.ShiftModifier))) {
                        event.accepted = true
                        memberRow.focusAction(0, 1, Qt.TabFocusReason)
                    }
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
                        activeFocusOnTab: false
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Activate %1").arg(memberRow.member !== null
                                                                   ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window manager activate this window")
                        Accessible.focusable: true
                        Accessible.focused: activeFocus
                        Accessible.pressed: down
                        Accessible.onPressAction: activate.clicked()
                        Keys.onTabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(0, 1)
                        }
                        Keys.onBacktabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(0, -1)
                        }
                        Keys.onLeftPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(0, -1)
                        }
                        Keys.onRightPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(0, 1)
                        }
                        Keys.onUpPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(-1, 0, Qt.BacktabFocusReason)
                        }
                        Keys.onDownPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(1, 0, Qt.TabFocusReason)
                        }
                        Keys.onEscapePressed: event => {
                            event.accepted = true
                            root.closeToOrigin()
                        }
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
                        activeFocusOnTab: false
                        enabled: memberRow.member !== null && !memberRow.member.minimized
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Minimize %1").arg(memberRow.member !== null
                                                                   ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window manager minimize this window")
                        Accessible.focusable: enabled
                        Accessible.focused: activeFocus
                        Accessible.pressed: down
                        Accessible.onPressAction: minimize.clicked()
                        Keys.onTabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(1, 1)
                        }
                        Keys.onBacktabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(1, -1)
                        }
                        Keys.onLeftPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(1, -1)
                        }
                        Keys.onRightPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(1, 1)
                        }
                        Keys.onUpPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(-1, 1, Qt.BacktabFocusReason)
                        }
                        Keys.onDownPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(1, 1, Qt.TabFocusReason)
                        }
                        Keys.onEscapePressed: event => {
                            event.accepted = true
                            root.closeToOrigin()
                        }
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
                        activeFocusOnTab: false
                        implicitWidth: root.tokens.minimumTargetSize
                        implicitHeight: root.tokens.minimumTargetSize
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: qsTr("Close %1").arg(memberRow.member !== null
                                                                ? memberRow.member.title : "")
                        Accessible.description: qsTr("Request that the window close")
                        Accessible.focusable: true
                        Accessible.focused: activeFocus
                        Accessible.pressed: down
                        Accessible.onPressAction: closeWindow.clicked()
                        Keys.onTabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(2, 1)
                        }
                        Keys.onBacktabPressed: event => {
                            event.accepted = true
                            memberRow.moveFromAction(2, -1)
                        }
                        Keys.onLeftPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(2, -1)
                        }
                        Keys.onRightPressed: event => {
                            event.accepted = true
                            memberRow.moveActionHorizontally(2, 1)
                        }
                        Keys.onUpPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(-1, 2, Qt.BacktabFocusReason)
                        }
                        Keys.onDownPressed: event => {
                            event.accepted = true
                            memberRow.focusAdjacentMember(1, 2, Qt.TabFocusReason)
                        }
                        Keys.onEscapePressed: event => {
                            event.accepted = true
                            root.closeToOrigin()
                        }
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
}
