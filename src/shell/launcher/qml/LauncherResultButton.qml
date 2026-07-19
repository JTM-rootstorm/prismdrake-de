pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: root

    required property var presentation
    required property var tokens
    required property real actionMenuMaximumWidth
    required property real actionMenuMaximumHeight
    property bool selected: false

    readonly property string presentationName: presentation !== null ? presentation.name : ""
    readonly property string presentationGenericName: presentation !== null
                                                       ? presentation.genericName : ""
    readonly property string presentationComment: presentation !== null
                                                   ? presentation.comment : ""
    readonly property bool terminalRequired: presentation !== null
                                             && presentation.terminalRequired
    readonly property bool applicationResult: presentation !== null
                                              && presentation.kindId === "application"
    readonly property bool pinned: presentation !== null && presentation.pinned
    readonly property bool prismdrakeAccessibleExpandable: applicationResult
    readonly property bool prismdrakeAccessibleExpanded: actionsMenu.opened
    readonly property string terminalStatus: terminalRequired
                                             ? qsTr("Requires a terminal") : ""

    signal focusPrevious
    signal focusNext
    signal dismissRequested
    signal actionMenuOpened
    signal actionMenuClosed

    function closeActionMenu() {
        actionsMenu.close()
    }

    function actionMenuItemAt(index) {
        return index >= 0 && index < actionRepeater.count ? actionRepeater.itemAt(index) : null
    }

    function pinMenuItem() {
        return pinItem
    }

    function actionMenuViewport() {
        return actionViewport
    }

    objectName: "launcherResultButton"
    activeFocusOnTab: false
    hoverEnabled: true
    text: presentationName
    leftPadding: tokens.tilePadding
    rightPadding: tokens.tilePadding
    topPadding: tokens.tilePadding
    bottomPadding: tokens.tilePadding
    implicitWidth: Math.max(tokens.minimumTargetSize,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(tokens.minimumTargetSize,
                             contentItem.implicitHeight + topPadding + bottomPadding)

    Accessible.role: Accessible.Button
    Accessible.name: presentationName
    Accessible.description: qsTr("Generic name: %1. Description: %2. Terminal required: %3. Pinned: %4. Actions menu available: %5.")
                                .arg(presentationGenericName.length > 0
                                     ? presentationGenericName : qsTr("Not provided"))
                                .arg(presentationComment.length > 0
                                     ? presentationComment : qsTr("Not provided"))
                                .arg(terminalRequired ? qsTr("Yes") : qsTr("No"))
                                .arg(pinned ? qsTr("Yes") : qsTr("No"))
                                .arg(applicationResult ? qsTr("Yes") : qsTr("No"))
    Accessible.focusable: enabled
    Accessible.focused: activeFocus
    Accessible.selectable: true
    Accessible.selected: selected
    Accessible.pressed: down
    Accessible.onPressAction: root.clicked()

    Keys.priority: Keys.BeforeItem
    Keys.onUpPressed: event => {
        event.accepted = true
        root.focusPrevious()
    }
    Keys.onBacktabPressed: event => {
        event.accepted = true
        root.focusPrevious()
    }
    Keys.onDownPressed: event => {
        event.accepted = true
        root.focusNext()
    }
    Keys.onTabPressed: event => {
        event.accepted = true
        root.focusNext()
    }
    Keys.onEscapePressed: event => {
        event.accepted = true
        root.dismissRequested()
    }
    Keys.onPressed: event => {
        const menuKey = event.key === Qt.Key_Menu
        const alternateMenuKey = event.key === Qt.Key_F10
                                 && (event.modifiers & Qt.ShiftModifier)
        if (root.applicationResult && (menuKey || alternateMenuKey)) {
            event.accepted = true
            actionsMenu.open()
        }
    }

    onClicked: {
        if (presentation !== null)
            presentation.requestLaunch()
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: {
            if (root.applicationResult)
                actionsMenu.open()
        }
    }

    Popup {
        id: actionsMenu
        objectName: "launcherActionsMenu"
        readonly property real edgeMargin: root.tokens.tilePadding

        function focusMenuItem(item, reason) {
            if (item === null)
                return
            item.forceActiveFocus(reason)
            Qt.callLater(revealMenuItem, item)
        }

        function revealMenuItem(item) {
            if (item === null || actionViewport.contentItem === null)
                return
            const viewport = actionViewport.contentItem as Flickable
            if (viewport === null)
                return
            const position = item.mapToItem(actionColumn, 0, 0)
            const itemTop = position.y
            const itemBottom = itemTop + item.height
            if (itemTop < viewport.contentY) {
                viewport.contentY = Math.max(0, itemTop)
            } else if (itemBottom > viewport.contentY + viewport.height) {
                viewport.contentY = Math.min(
                            Math.max(0, actionColumn.implicitHeight - viewport.height),
                            itemBottom - viewport.height)
            }
        }

        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: root.tokens.tilePadding
        leftMargin: edgeMargin
        rightMargin: edgeMargin
        topMargin: edgeMargin
        bottomMargin: edgeMargin
        width: Math.max(root.tokens.minimumTargetSize, root.actionMenuMaximumWidth)
        height: Math.max(root.tokens.minimumTargetSize,
                         Math.min(actionColumn.implicitHeight + topPadding + bottomPadding,
                                  root.actionMenuMaximumHeight))
        onOpened: {
            root.actionMenuOpened()
            const first = actionRepeater.count > 0 ? actionRepeater.itemAt(0) : pinItem
            focusMenuItem(first, Qt.OtherFocusReason)
        }
        onClosed: {
            root.actionMenuClosed()
            root.forceActiveFocus(Qt.OtherFocusReason)
        }

        background: Rectangle {
            radius: root.tokens.tileRadius
            color: root.tokens.surfaceColor
            border.width: root.tokens.tileBorderWidth
            border.color: root.tokens.borderColor
        }

        contentItem: ScrollView {
            id: actionViewport
            objectName: "launcherActionsViewport"
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            Column {
                id: actionColumn
                width: actionViewport.availableWidth
                spacing: root.tokens.tilePadding
                LayoutMirroring.enabled: root.mirrored
                LayoutMirroring.childrenInherit: true
                Accessible.role: Accessible.PopupMenu
                Accessible.name: qsTr("Application actions for %1").arg(root.presentationName)
                Accessible.description: qsTr("Launch an application action or change its pinned state")

                Repeater {
                    id: actionRepeater
                    model: root.presentation !== null ? root.presentation.actionCount : 0
                    delegate: Button {
                        id: actionItem
                        required property int index
                        readonly property var actionPresentation: root.presentation !== null
                                                                  ? root.presentation.actionAt(index)
                                                                  : null
                        objectName: "launcherActionMenuItem" + index
                        width: actionColumn.width
                        text: actionPresentation !== null ? actionPresentation.name : ""
                        activeFocusOnTab: false
                        implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                                contentItem.implicitWidth + leftPadding + rightPadding)
                        implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                                 contentItem.implicitHeight + topPadding + bottomPadding)
                        Accessible.role: Accessible.MenuItem
                        Accessible.name: text
                        Accessible.description: qsTr("Launch the %1 action for %2")
                                                    .arg(text).arg(root.presentationName)
                        Accessible.focusable: enabled
                        Accessible.focused: activeFocus
                        Keys.onTabPressed: event => {
                            event.accepted = true
                            const next = index + 1 < actionRepeater.count
                                       ? actionRepeater.itemAt(index + 1) : pinItem
                            actionsMenu.focusMenuItem(next, Qt.TabFocusReason)
                        }
                        Keys.onBacktabPressed: event => {
                            event.accepted = true
                            const previous = index > 0 ? actionRepeater.itemAt(index - 1) : pinItem
                            actionsMenu.focusMenuItem(previous, Qt.BacktabFocusReason)
                        }
                        Keys.onUpPressed: event => {
                            event.accepted = true
                            const previous = index > 0 ? actionRepeater.itemAt(index - 1) : pinItem
                            actionsMenu.focusMenuItem(previous, Qt.BacktabFocusReason)
                        }
                        Keys.onDownPressed: event => {
                            event.accepted = true
                            const next = index + 1 < actionRepeater.count
                                       ? actionRepeater.itemAt(index + 1) : pinItem
                            actionsMenu.focusMenuItem(next, Qt.TabFocusReason)
                        }
                        onClicked: {
                            if (actionPresentation !== null)
                                actionPresentation.request()
                            actionsMenu.close()
                        }
                        contentItem: Text {
                            objectName: "launcherActionMenuLabel" + actionItem.index
                            width: actionItem.availableWidth
                            color: root.tokens.textPrimaryColor
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.family: root.tokens.bodyFontFamily
                            font.pixelSize: root.tokens.bodyFontPixels
                            text: actionItem.text
                        }
                        background: Rectangle {
                            radius: root.tokens.tileRadius
                            color: actionItem.down ? root.tokens.selectionColor
                                                   : root.tokens.controlColor
                            border.width: actionItem.activeFocus ? root.tokens.focusWidth
                                                                 : root.tokens.tileBorderWidth
                            border.color: actionItem.activeFocus ? root.tokens.focusColor
                                                                 : root.tokens.borderColor
                        }
                    }
                }

                MenuSeparator {
                    width: actionColumn.width
                    visible: root.presentation !== null && root.presentation.actionCount > 0
                }

                Button {
                    id: pinItem
                    objectName: "launcherPinMenuItem"
                    width: actionColumn.width
                    visible: root.applicationResult
                    text: root.pinned ? qsTr("Unpin from panel") : qsTr("Pin to panel")
                    activeFocusOnTab: false
                    implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                            contentItem.implicitWidth + leftPadding + rightPadding)
                    implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                             contentItem.implicitHeight + topPadding + bottomPadding)
                    Accessible.role: Accessible.MenuItem
                    Accessible.name: text
                    Accessible.description: root.pinned
                                            ? qsTr("Remove %1 from the shared panel pins")
                                                .arg(root.presentationName)
                                            : qsTr("Add %1 to the shared panel pins")
                                                .arg(root.presentationName)
                    Accessible.focusable: visible
                    Accessible.focused: activeFocus
                    Accessible.checkable: true
                    Accessible.checked: root.pinned
                    Keys.onTabPressed: event => {
                        event.accepted = true
                        const first = actionRepeater.count > 0 ? actionRepeater.itemAt(0) : pinItem
                        actionsMenu.focusMenuItem(first, Qt.TabFocusReason)
                    }
                    Keys.onBacktabPressed: event => {
                        event.accepted = true
                        const last = actionRepeater.count > 0
                                   ? actionRepeater.itemAt(actionRepeater.count - 1) : pinItem
                        actionsMenu.focusMenuItem(last, Qt.BacktabFocusReason)
                    }
                    Keys.onUpPressed: event => {
                        event.accepted = true
                        const last = actionRepeater.count > 0
                                   ? actionRepeater.itemAt(actionRepeater.count - 1) : pinItem
                        actionsMenu.focusMenuItem(last, Qt.BacktabFocusReason)
                    }
                    Keys.onDownPressed: event => {
                        event.accepted = true
                        const first = actionRepeater.count > 0 ? actionRepeater.itemAt(0) : pinItem
                        actionsMenu.focusMenuItem(first, Qt.TabFocusReason)
                    }
                    onClicked: {
                        root.presentation.requestPinToggle()
                        actionsMenu.close()
                    }
                    contentItem: Text {
                        width: pinItem.availableWidth
                        color: root.tokens.textPrimaryColor
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        font.family: root.tokens.bodyFontFamily
                        font.pixelSize: root.tokens.bodyFontPixels
                        text: pinItem.text
                    }
                    background: Rectangle {
                        radius: root.tokens.tileRadius
                        color: pinItem.down ? root.tokens.selectionColor : root.tokens.controlColor
                        border.width: pinItem.activeFocus ? root.tokens.focusWidth
                                                          : root.tokens.tileBorderWidth
                        border.color: pinItem.activeFocus ? root.tokens.focusColor
                                                          : root.tokens.borderColor
                    }
                }
            }
        }
    }

    contentItem: Column {
        spacing: Math.max(2, root.tokens.tilePadding / 4)

        Image {
            objectName: "launcherResultIcon"
            width: Math.max(24, root.tokens.minimumTargetSize / 2)
            height: width
            anchors.horizontalCenter: parent.horizontalCenter
            source: root.presentation !== null ? root.presentation.iconSource : ""
            sourceSize.width: width
            sourceSize.height: height
            fillMode: Image.PreserveAspectFit
            asynchronous: false
            Accessible.ignored: true
        }

        Text {
            id: title
            objectName: "launcherResultTitle"
            width: parent.width
            color: root.tokens.textPrimaryColor
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            font.bold: true
            text: root.presentationName
        }

        Text {
            objectName: "launcherResultGenericName"
            visible: text.length > 0
            width: parent.width
            color: root.tokens.textMutedColor
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            text: root.presentationGenericName
        }

        Text {
            objectName: "launcherResultComment"
            visible: text.length > 0
            width: parent.width
            color: root.tokens.textMutedColor
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            text: root.presentationComment
        }

        Text {
            objectName: "launcherResultTerminalState"
            visible: root.terminalRequired
            width: parent.width
            color: root.tokens.textPrimaryColor
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            font.bold: true
            text: root.terminalStatus
        }

        Text {
            objectName: "launcherResultPinnedState"
            visible: root.pinned
            width: parent.width
            color: root.tokens.textPrimaryColor
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            font.bold: true
            text: qsTr("Pinned")
        }
    }

    background: Rectangle {
        radius: root.tokens.tileRadius
        color: root.down || root.hovered || root.selected ? root.tokens.selectionColor
                                                          : root.tokens.controlColor
        border.width: root.activeFocus ? root.tokens.focusWidth
                                       : root.tokens.tileBorderWidth
        border.color: root.activeFocus ? root.tokens.focusColor : root.tokens.borderColor
    }
}
