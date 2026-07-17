pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var presentationModel

    width: 960
    height: Math.max(280, 192 + root.presentationModel.minimumTargetPixels * 2)
    minimumWidth: 720
    minimumHeight: 220
    visible: false
    color: "transparent"
    title: "Prismdrake PD1 toolkit evidence spike"
    flags: Qt.FramelessWindowHint
    x: Screen.virtualX
    y: Screen.virtualY + Screen.height - height

    component SpikeButton: Button {
        id: control

        implicitHeight: root.presentationModel.minimumTargetPixels
        leftPadding: 14
        rightPadding: 14
        font.pixelSize: root.presentationModel.bodyFontPixels
        activeFocusOnTab: true
        Accessible.name: text
        Accessible.description: "Experimental Prismdrake shell control"
        Accessible.role: Accessible.Button
        Accessible.focused: activeFocus
        Accessible.checkable: checkable
        Accessible.checked: checked

        contentItem: Text {
            text: control.text
            color: root.presentationModel.textPrimaryColor
            font: control.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: control.checked || control.down
                ? root.presentationModel.borderActiveColor
                : root.presentationModel.elevatedColor
            radius: root.presentationModel.taskRadiusPixels
            border.width: control.activeFocus
                ? root.presentationModel.focusWidthPixels
                : root.presentationModel.borderWidthPixels
            border.color: control.activeFocus
                ? root.presentationModel.focusColor
                : root.presentationModel.borderInactiveColor

            Behavior on border.width {
                NumberAnimation { duration: root.presentationModel.motionDurationMs }
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.presentationModel.launcherVisible
        onActivated: root.presentationModel.dismissLauncher()
    }

    Rectangle {
        anchors.fill: parent
        color: root.presentationModel.panelColor
        border.width: root.presentationModel.borderWidthPixels
        border.color: root.presentationModel.borderInactiveColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Rectangle {
                id: launcherSample

                Layout.fillWidth: true
                Layout.preferredHeight: root.presentationModel.launcherVisible ? 94 : 0
                visible: opacity > 0
                opacity: root.presentationModel.launcherVisible ? 1 : 0
                color: root.presentationModel.elevatedColor
                radius: root.presentationModel.taskRadiusPixels
                border.width: root.presentationModel.borderWidthPixels
                border.color: root.presentationModel.borderActiveColor
                Accessible.name: "Launcher sample"
                Accessible.description: "Dismiss with Escape or the close button"
                Accessible.role: Accessible.Pane

                Behavior on opacity {
                    NumberAnimation { duration: root.presentationModel.motionDurationMs }
                }
                Behavior on Layout.preferredHeight {
                    NumberAnimation { duration: root.presentationModel.motionDurationMs }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12

                    ColumnLayout {
                        Layout.fillWidth: true

                        Text {
                            text: "Launcher sample"
                            color: root.presentationModel.textPrimaryColor
                            font.pixelSize: root.presentationModel.titleFontPixels
                            font.bold: true
                        }
                        Text {
                            text: "No applications are launched by this evidence-only surface."
                            color: root.presentationModel.textMutedColor
                            font.pixelSize: root.presentationModel.bodyFontPixels
                        }
                    }

                    SpikeButton {
                        id: closeLauncherButton
                        text: "Close launcher"
                        Accessible.description: "Dismiss the launcher sample"
                        KeyNavigation.tab: launcherButton
                        KeyNavigation.backtab: transparencyButton
                        onClicked: root.presentationModel.dismissLauncher()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                SpikeButton {
                    id: launcherButton
                    text: "Open launcher"
                    Accessible.description: "Open the non-functional launcher sample"
                    KeyNavigation.tab: taskRepeater.itemAt(0)
                    KeyNavigation.backtab: transparencyButton
                    onClicked: root.presentationModel.activateLauncher()
                }

                Repeater {
                    id: taskRepeater
                    model: root.presentationModel.tasks

                    delegate: SpikeButton {
                        required property int index
                        required property string modelData

                        text: modelData
                        checkable: true
                        checked: index === root.presentationModel.activeTask
                        Accessible.name: modelData + " task"
                        Accessible.description: checked
                            ? "Selected task; checked"
                            : "Task is not selected"
                        KeyNavigation.tab: index + 1 < taskRepeater.count
                            ? taskRepeater.itemAt(index + 1)
                            : profileButton
                        KeyNavigation.backtab: index > 0
                            ? taskRepeater.itemAt(index - 1)
                            : launcherButton
                        onClicked: root.presentationModel.activateTask(index)
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: "DPR " + Screen.devicePixelRatio.toFixed(2)
                    color: root.presentationModel.textMutedColor
                    font.pixelSize: root.presentationModel.bodyFontPixels
                    Accessible.name: "Device pixel ratio " + Screen.devicePixelRatio.toFixed(2)
                    Accessible.role: Accessible.StaticText
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                SpikeButton {
                    id: profileButton
                    text: "Profile: " + root.presentationModel.profileDisplayName
                    checkable: true
                    checked: root.presentationModel.profileId === "forge"
                    Accessible.name: "Switch visual profile"
                    Accessible.description: "Current profile " + root.presentationModel.profileDisplayName
                    KeyNavigation.tab: textScaleButton
                    KeyNavigation.backtab: taskRepeater.itemAt(taskRepeater.count - 1)
                    onClicked: root.presentationModel.toggleProfile()
                }

                SpikeButton {
                    id: textScaleButton
                    text: "Text: " + Math.round(root.presentationModel.textScale * 100) + "%"
                    Accessible.name: "Cycle text scale"
                    Accessible.description: "Current text scale "
                        + Math.round(root.presentationModel.textScale * 100) + " percent"
                    KeyNavigation.tab: motionButton
                    KeyNavigation.backtab: profileButton
                    onClicked: root.presentationModel.cycleTextScale()
                }

                SpikeButton {
                    id: motionButton
                    text: root.presentationModel.reducedMotion
                        ? "Reduced motion: on"
                        : "Reduced motion: off"
                    checkable: true
                    checked: root.presentationModel.reducedMotion
                    Accessible.name: "Reduced motion"
                    Accessible.description: checked ? "Enabled" : "Disabled"
                    KeyNavigation.tab: transparencyButton
                    KeyNavigation.backtab: textScaleButton
                    onClicked: root.presentationModel.setReducedMotion(!root.presentationModel.reducedMotion)
                }

                SpikeButton {
                    id: transparencyButton
                    text: root.presentationModel.transparencyDisabled
                        ? "Transparency: off"
                        : "Transparency: profile default"
                    checkable: true
                    checked: root.presentationModel.transparencyDisabled
                    Accessible.name: "Disable transparency"
                    Accessible.description: checked
                        ? "Opaque fallback material active"
                        : "Profile material active"
                    KeyNavigation.tab: root.presentationModel.launcherVisible
                        ? closeLauncherButton
                        : launcherButton
                    KeyNavigation.backtab: motionButton
                    onClicked: root.presentationModel.setTransparencyDisabled(
                        !root.presentationModel.transparencyDisabled)
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: root.presentationModel.statusMessage
                color: root.presentationModel.textMutedColor
                font.pixelSize: root.presentationModel.bodyFontPixels
                Accessible.name: "Status: " + root.presentationModel.statusMessage
                Accessible.role: Accessible.StaticText
            }
        }
    }

    Connections {
        target: root.presentationModel

        function onLauncherVisibleChanged() {
            if (root.presentationModel.launcherVisible) {
                closeLauncherButton.forceActiveFocus(Qt.TabFocusReason)
            } else if (closeLauncherButton.activeFocus) {
                launcherButton.forceActiveFocus(Qt.BacktabFocusReason)
            }
        }
    }

    onVisibleChanged: {
        if (visible) {
            launcherButton.forceActiveFocus(Qt.TabFocusReason)
        }
    }
}
