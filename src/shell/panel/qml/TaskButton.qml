pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: root

    required property var presentation
    required property var tokens

    readonly property string presentationTitle: presentation !== null ? presentation.title : ""
    readonly property string presentationApplicationId: presentation !== null
                                                               ? presentation.applicationId : ""
    readonly property string presentationStatusText: presentation !== null
                                                            ? presentation.statusText : ""
    readonly property bool presentationActive: presentation !== null && presentation.active
    readonly property bool presentationUrgent: presentation !== null && presentation.urgent
    readonly property bool urgentState: presentationUrgent

    signal focusExitForward
    signal focusExitBackward

    objectName: "panelTaskButton"
    activeFocusOnTab: false
    hoverEnabled: true
    text: presentationTitle
    implicitWidth: Math.max(tokens.minimumTargetSize,
                            Math.max(taskTitle.implicitWidth, taskState.implicitWidth)
                            + leftPadding + rightPadding)
    implicitHeight: Math.max(tokens.minimumTargetSize, tokens.panelHeight)
    leftPadding: tokens.taskPadding
    rightPadding: tokens.taskPadding
    topPadding: 4
    bottomPadding: 4

    Accessible.role: Accessible.Button
    Accessible.name: presentationTitle
    Accessible.description: qsTr("%1. %2")
                                .arg(presentationApplicationId)
                                .arg(presentationStatusText)
    Accessible.focusable: true
    Accessible.focused: activeFocus
    Accessible.checkable: true
    Accessible.checked: presentationActive

    Keys.priority: Keys.BeforeItem
    Keys.onTabPressed: event => {
        event.accepted = true
        root.focusExitForward()
    }
    Keys.onBacktabPressed: event => {
        event.accepted = true
        root.focusExitBackward()
    }

    onClicked: {
        if (presentation !== null)
            presentation.requestActivation()
    }

    contentItem: Column {
        id: taskContents
        spacing: 1

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
            id: urgentMarker
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
}
