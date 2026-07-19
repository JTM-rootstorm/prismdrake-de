pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var quickSettingsModel
    required property var themeGeneration

    readonly property var tokens: themeGeneration.notification
    readonly property int focusableControlCount: 5
    readonly property bool opaqueFallbackActive: tokens.fallbackActive

    signal closeRequested
    signal focusExitForward
    signal focusExitBackward

    function focusFirstControl() {
        focusNextFrom(-1, 1, Qt.TabFocusReason)
    }

    function controlAt(index) {
        switch (index) {
        case 0: return lustreProfile
        case 1: return forgeProfile
        case 2: return doNotDisturb
        case 3: return notificationCenter
        case 4: return logout
        default: return null
        }
    }

    function focusControl(index, reason) {
        const candidate = controlAt(index)
        if (candidate !== null && candidate.enabled)
            candidate.forceActiveFocus(reason)
    }

    function focusNextFrom(index, direction, reason) {
        for (let candidateIndex = index + direction;
             candidateIndex >= 0 && candidateIndex < focusableControlCount;
             candidateIndex += direction) {
            const candidate = controlAt(candidateIndex)
            if (candidate !== null && candidate.enabled) {
                candidate.forceActiveFocus(reason)
                return true
            }
        }
        if (direction > 0)
            focusExitForward()
        else
            focusExitBackward()
        return false
    }

    function handleForward(index, event) {
        event.accepted = true
        focusNextFrom(index, 1, Qt.TabFocusReason)
    }

    function handleBackward(index, event) {
        event.accepted = true
        focusNextFrom(index, -1, Qt.BacktabFocusReason)
    }

    function closeFromEscape(event) {
        event.accepted = true
        closeRequested()
    }

    Keys.priority: Keys.BeforeItem
    Keys.onEscapePressed: event => closeFromEscape(event)

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Quick settings")
    Accessible.description: qsTr("Prismdrake profile, notifications, session, and diagnostics")

    implicitWidth: 380
    implicitHeight: contents.implicitHeight + 24

    Rectangle {
        anchors.fill: parent
        color: root.tokens.surfaceColor
        border.color: root.tokens.borderColor
        border.width: root.tokens.borderWidth
        radius: root.tokens.cardRadius
    }

    ColumnLayout {
        id: contents
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("Quick settings")
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.titleFontPixels
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Desktop profile")
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            RadioButton {
                id: lustreProfile
                objectName: "quickSettingsLustreProfile"
                Layout.fillWidth: true
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                text: qsTr("Lustre")
                checked: root.quickSettingsModel.profile.valueId === "lustre"
                enabled: root.quickSettingsModel.profile.interactive
                         && !root.quickSettingsModel.profile.pending
                activeFocusOnTab: false

                Accessible.role: Accessible.RadioButton
                Accessible.name: qsTr("Use Prismdrake Lustre")
                Accessible.description: root.quickSettingsModel.profile.description
                Accessible.checked: checked
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: event => root.handleForward(0, event)
                Keys.onBacktabPressed: event => root.handleBackward(0, event)
                Keys.onEscapePressed: event => root.closeFromEscape(event)
                onClicked: root.quickSettingsModel.requestLustreProfile()
            }

            RadioButton {
                id: forgeProfile
                objectName: "quickSettingsForgeProfile"
                Layout.fillWidth: true
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                text: qsTr("Forge")
                checked: root.quickSettingsModel.profile.valueId === "forge"
                enabled: root.quickSettingsModel.profile.interactive
                         && !root.quickSettingsModel.profile.pending
                activeFocusOnTab: false

                Accessible.role: Accessible.RadioButton
                Accessible.name: qsTr("Use Prismdrake Forge")
                Accessible.description: root.quickSettingsModel.profile.description
                Accessible.checked: checked
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: event => root.handleForward(1, event)
                Keys.onBacktabPressed: event => root.handleBackward(1, event)
                Keys.onEscapePressed: event => root.closeFromEscape(event)
                onClicked: root.quickSettingsModel.requestForgeProfile()
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.quickSettingsModel.profile.description
            color: root.quickSettingsModel.profile.failed
                   ? root.tokens.criticalColor : root.tokens.textMutedColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            wrapMode: Text.Wrap
        }

        Switch {
            id: doNotDisturb
            objectName: "quickSettingsDoNotDisturb"
            Layout.fillWidth: true
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Do not disturb")
            checked: root.quickSettingsModel.doNotDisturb.checked
            enabled: root.quickSettingsModel.doNotDisturb.interactive
                     && !root.quickSettingsModel.doNotDisturb.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Do not disturb")
            Accessible.description: root.quickSettingsModel.doNotDisturb.description
            Accessible.checked: checked
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onToggleAction: root.quickSettingsModel.requestDoNotDisturb(!checked)

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(2, event)
            Keys.onBacktabPressed: event => root.handleBackward(2, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            onClicked: root.quickSettingsModel.requestDoNotDisturb(checked)
        }

        Label {
            Layout.fillWidth: true
            text: root.quickSettingsModel.doNotDisturb.description
            color: root.quickSettingsModel.doNotDisturb.failed
                   ? root.tokens.criticalColor : root.tokens.textMutedColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            wrapMode: Text.Wrap
        }

        Button {
            id: notificationCenter
            objectName: "quickSettingsNotificationCenter"
            Layout.fillWidth: true
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Open notification center")
            enabled: root.quickSettingsModel.notificationCenter.interactive
                     && !root.quickSettingsModel.notificationCenter.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.description: root.quickSettingsModel.notificationCenter.description
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onPressAction: clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(3, event)
            Keys.onBacktabPressed: event => root.handleBackward(3, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            onClicked: root.quickSettingsModel.requestNotificationCenter()
        }

        Button {
            id: logout
            objectName: "quickSettingsLogout"
            Layout.fillWidth: true
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Log out")
            enabled: root.quickSettingsModel.logout.interactive
                     && !root.quickSettingsModel.logout.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Request logout")
            Accessible.description: root.quickSettingsModel.logout.description
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onPressAction: clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(4, event)
            Keys.onBacktabPressed: event => root.handleBackward(4, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            onClicked: root.quickSettingsModel.requestLogout()
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: diagnosticsColumn.implicitHeight + 16
            color: root.tokens.controlColor
            border.color: root.quickSettingsModel.diagnostics.failed
                          ? root.tokens.criticalColor : root.tokens.borderColor
            border.width: root.tokens.borderWidth
            radius: root.tokens.cardRadius

            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Diagnostics summary: %1")
                                .arg(root.quickSettingsModel.diagnostics.stateText)
            Accessible.description: root.quickSettingsModel.diagnostics.description

            ColumnLayout {
                id: diagnosticsColumn
                anchors.fill: parent
                anchors.margins: 8
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Diagnostics")
                    color: root.tokens.textPrimaryColor
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: root.quickSettingsModel.diagnostics.description
                    color: root.tokens.textMutedColor
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
