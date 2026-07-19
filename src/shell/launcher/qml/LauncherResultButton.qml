pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: root

    required property var presentation
    required property var tokens
    property bool selected: false
    property var actionMenuItems: []

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
        return index >= 0 && index < actionMenuItems.length ? actionMenuItems[index] : null
    }

    function pinMenuItem() {
        return pinItem
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
    Accessible.focusable: true
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
            actionsMenu.popup()
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
                actionsMenu.popup()
        }
    }

    Menu {
        id: actionsMenu
        objectName: "launcherActionsMenu"
        onOpened: root.actionMenuOpened()
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

        Instantiator {
            id: actionInstantiator
            model: root.presentation !== null ? root.presentation.actionCount : 0
            delegate: MenuItem {
                id: actionItem
                required property int index
                readonly property var actionPresentation: root.presentation.actionAt(index)
                objectName: "launcherActionMenuItem" + index
                text: actionPresentation !== null ? actionPresentation.name : ""
                implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                        contentItem.implicitWidth + leftPadding + rightPadding)
                implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                         contentItem.implicitHeight + topPadding + bottomPadding)
                width: implicitWidth
                height: implicitHeight
                Accessible.role: Accessible.MenuItem
                Accessible.name: text
                Accessible.description: qsTr("Launch the %1 action for %2")
                                            .arg(text).arg(root.presentationName)
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: actionItem.clicked()
                onTriggered: {
                    if (actionPresentation !== null)
                        actionPresentation.request()
                }
                contentItem: Text {
                    color: root.tokens.textPrimaryColor
                    textFormat: Text.PlainText
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
            onObjectAdded: (index, object) => {
                const items = root.actionMenuItems.slice()
                items.splice(index, 0, object)
                root.actionMenuItems = items
                actionsMenu.insertItem(index, object)
            }
            onObjectRemoved: (index, object) => {
                const items = root.actionMenuItems.slice()
                const itemIndex = items.indexOf(object)
                if (itemIndex >= 0)
                    items.splice(itemIndex, 1)
                root.actionMenuItems = items
                actionsMenu.removeItem(object)
            }
        }

        MenuSeparator {
            visible: root.presentation !== null && root.presentation.actionCount > 0
        }

        MenuItem {
            id: pinItem
            objectName: "launcherPinMenuItem"
            visible: root.applicationResult
            text: root.pinned ? qsTr("Unpin from panel") : qsTr("Pin to panel")
            implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                    contentItem.implicitWidth + leftPadding + rightPadding)
            implicitHeight: Math.max(root.tokens.minimumTargetSize,
                                     contentItem.implicitHeight + topPadding + bottomPadding)
            width: implicitWidth
            height: implicitHeight
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
            Accessible.onPressAction: pinItem.clicked()
            onTriggered: root.presentation.requestPinToggle()
            contentItem: Text {
                color: root.tokens.textPrimaryColor
                textFormat: Text.PlainText
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
