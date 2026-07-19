pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var quickSettingsModel
    required property var themeGeneration
    property int layoutDirection: Qt.locale().textDirection

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

    function activateButton(control, event) {
        event.accepted = true
        if (control.enabled)
            control.clicked()
    }

    function activateDoNotDisturb(event) {
        event.accepted = true
        if (doNotDisturb.enabled)
            quickSettingsModel.requestDoNotDisturb(!doNotDisturb.checked)
    }

    function accessibleAdapterDescription(adapter) {
        return qsTr("%1 Current state: %2.")
            .arg(adapter.description.length > 0 ? adapter.description : qsTr("No description."))
            .arg(adapter.stateText.length > 0 ? adapter.stateText : qsTr("Unavailable"))
    }

    function handleVertical(index, direction, event) {
        event.accepted = true
        focusNextFrom(index, direction,
                      direction > 0 ? Qt.TabFocusReason : Qt.BacktabFocusReason)
    }

    function handleHorizontal(index, visualDirection, event) {
        const logicalDirection = layoutDirection === Qt.RightToLeft
            ? -visualDirection : visualDirection
        handleVertical(index, logicalDirection, event)
    }

    Keys.priority: Keys.BeforeItem
    Keys.onEscapePressed: event => closeFromEscape(event)

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Quick settings")
    Accessible.description: opaqueFallbackActive
        ? qsTr("Prismdrake profile, notifications, session, and diagnostics. Opaque fallback is active.")
        : qsTr("Prismdrake profile, notifications, session, and diagnostics.")

    implicitWidth: 380
    implicitHeight: contents.implicitHeight + 24
    LayoutMirroring.enabled: layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    Rectangle {
        objectName: "quickSettingsBackground"
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
            objectName: "quickSettingsHeading"
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
                Layout.minimumHeight: root.tokens.minimumTargetSize
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                text: qsTr("Lustre")
                checked: root.quickSettingsModel.profile.valueId === "lustre"
                enabled: root.quickSettingsModel.profile.interactive
                         && !root.quickSettingsModel.profile.pending
                activeFocusOnTab: false

                Accessible.role: Accessible.RadioButton
                Accessible.name: qsTr("Use Prismdrake Lustre")
                Accessible.description: root.accessibleAdapterDescription(
                                            root.quickSettingsModel.profile)
                Accessible.checked: checked
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: event => root.handleForward(0, event)
                Keys.onBacktabPressed: event => root.handleBackward(0, event)
                Keys.onEscapePressed: event => root.closeFromEscape(event)
                Keys.onUpPressed: event => root.handleVertical(0, -1, event)
                Keys.onDownPressed: event => root.handleVertical(0, 1, event)
                Keys.onLeftPressed: event => root.handleHorizontal(0, -1, event)
                Keys.onRightPressed: event => root.handleHorizontal(0, 1, event)
                Keys.onReturnPressed: event => root.activateButton(lustreProfile, event)
                Keys.onEnterPressed: event => root.activateButton(lustreProfile, event)
                onClicked: root.quickSettingsModel.requestLustreProfile()
            }

            RadioButton {
                id: forgeProfile
                objectName: "quickSettingsForgeProfile"
                Layout.fillWidth: true
                Layout.minimumHeight: root.tokens.minimumTargetSize
                implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
                text: qsTr("Forge")
                checked: root.quickSettingsModel.profile.valueId === "forge"
                enabled: root.quickSettingsModel.profile.interactive
                         && !root.quickSettingsModel.profile.pending
                activeFocusOnTab: false

                Accessible.role: Accessible.RadioButton
                Accessible.name: qsTr("Use Prismdrake Forge")
                Accessible.description: root.accessibleAdapterDescription(
                                            root.quickSettingsModel.profile)
                Accessible.checked: checked
                Accessible.focusable: enabled
                Accessible.focused: activeFocus
                Accessible.onPressAction: clicked()

                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: event => root.handleForward(1, event)
                Keys.onBacktabPressed: event => root.handleBackward(1, event)
                Keys.onEscapePressed: event => root.closeFromEscape(event)
                Keys.onUpPressed: event => root.handleVertical(1, -1, event)
                Keys.onDownPressed: event => root.handleVertical(1, 1, event)
                Keys.onLeftPressed: event => root.handleHorizontal(1, -1, event)
                Keys.onRightPressed: event => root.handleHorizontal(1, 1, event)
                Keys.onReturnPressed: event => root.activateButton(forgeProfile, event)
                Keys.onEnterPressed: event => root.activateButton(forgeProfile, event)
                onClicked: root.quickSettingsModel.requestForgeProfile()
            }
        }

        Label {
            id: profileDescription
            objectName: "quickSettingsProfileDescription"
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
            Layout.minimumHeight: root.tokens.minimumTargetSize
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Do not disturb")
            checked: root.quickSettingsModel.doNotDisturb.checked
            enabled: root.quickSettingsModel.doNotDisturb.interactive
                     && !root.quickSettingsModel.doNotDisturb.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Do not disturb")
            Accessible.description: root.accessibleAdapterDescription(
                                        root.quickSettingsModel.doNotDisturb)
            Accessible.checked: checked
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onToggleAction: root.quickSettingsModel.requestDoNotDisturb(!checked)

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(2, event)
            Keys.onBacktabPressed: event => root.handleBackward(2, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            Keys.onUpPressed: event => root.handleVertical(2, -1, event)
            Keys.onDownPressed: event => root.handleVertical(2, 1, event)
            Keys.onReturnPressed: event => root.activateDoNotDisturb(event)
            Keys.onEnterPressed: event => root.activateDoNotDisturb(event)
            onClicked: root.quickSettingsModel.requestDoNotDisturb(checked)
        }

        Label {
            id: doNotDisturbDescription
            objectName: "quickSettingsDoNotDisturbDescription"
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
            Layout.minimumHeight: root.tokens.minimumTargetSize
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Open notification center")
            enabled: root.quickSettingsModel.notificationCenter.interactive
                     && !root.quickSettingsModel.notificationCenter.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.Button
            Accessible.name: text
            Accessible.description: root.accessibleAdapterDescription(
                                        root.quickSettingsModel.notificationCenter)
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onPressAction: clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(3, event)
            Keys.onBacktabPressed: event => root.handleBackward(3, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            Keys.onUpPressed: event => root.handleVertical(3, -1, event)
            Keys.onDownPressed: event => root.handleVertical(3, 1, event)
            Keys.onReturnPressed: event => root.activateButton(notificationCenter, event)
            Keys.onEnterPressed: event => root.activateButton(notificationCenter, event)
            onClicked: root.quickSettingsModel.requestNotificationCenter()
        }

        Button {
            id: logout
            objectName: "quickSettingsLogout"
            Layout.fillWidth: true
            Layout.minimumHeight: root.tokens.minimumTargetSize
            implicitHeight: Math.max(root.tokens.minimumTargetSize, implicitContentHeight)
            text: qsTr("Log out")
            enabled: root.quickSettingsModel.logout.interactive
                     && !root.quickSettingsModel.logout.pending
            activeFocusOnTab: false

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Request logout")
            Accessible.description: root.accessibleAdapterDescription(
                                        root.quickSettingsModel.logout)
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onPressAction: clicked()

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(4, event)
            Keys.onBacktabPressed: event => root.handleBackward(4, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            Keys.onUpPressed: event => root.handleVertical(4, -1, event)
            Keys.onDownPressed: event => root.handleVertical(4, 1, event)
            Keys.onReturnPressed: event => root.activateButton(logout, event)
            Keys.onEnterPressed: event => root.activateButton(logout, event)
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
            Accessible.description: root.accessibleAdapterDescription(
                                        root.quickSettingsModel.diagnostics)

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
