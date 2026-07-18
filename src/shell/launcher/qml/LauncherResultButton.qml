pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: root

    required property var presentation
    required property var tokens

    readonly property string presentationName: presentation !== null ? presentation.name : ""
    readonly property string presentationGenericName: presentation !== null
                                                       ? presentation.genericName : ""
    readonly property string presentationComment: presentation !== null
                                                   ? presentation.comment : ""
    readonly property bool terminalRequired: presentation !== null
                                             && presentation.terminalRequired
    readonly property string terminalStatus: terminalRequired
                                             ? qsTr("Requires a terminal") : ""

    signal focusPrevious
    signal focusNext
    signal dismissRequested

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
    Accessible.description: qsTr("%1. %2. %3")
                                .arg(presentationGenericName)
                                .arg(presentationComment)
                                .arg(terminalStatus)
    Accessible.focusable: true
    Accessible.focused: activeFocus

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

    onClicked: {
        if (presentation !== null)
            presentation.requestLaunch()
    }

    contentItem: Column {
        spacing: Math.max(2, root.tokens.tilePadding / 4)

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
    }

    background: Rectangle {
        radius: root.tokens.tileRadius
        color: root.down || root.hovered ? root.tokens.selectionColor
                                         : root.tokens.controlColor
        border.width: root.activeFocus ? root.tokens.focusWidth
                                       : root.tokens.tileBorderWidth
        border.color: root.activeFocus ? root.tokens.focusColor : root.tokens.borderColor
    }
}
