pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var quickSettingsModel
    required property var themeGeneration
    property int layoutDirection: Qt.locale().textDirection
    property int pendingProfileFocusRecovery: -1
    property bool pendingProfileVisualFocusRecovery: false
    property int pendingProfileFocusRecoverySerial: -1
    property bool pendingDoNotDisturbFocusRecovery: false
    property bool pendingDoNotDisturbVisualFocusRecovery: false
    property int pendingDoNotDisturbFocusRecoverySerial: -1
    property int pendingFocusAnchorOrigin: -1
    property int focusRecoverySerial: 0

    readonly property var tokens: themeGeneration.notification
    readonly property int focusableControlCount: 5
    readonly property bool opaqueFallbackActive: tokens.fallbackActive
    readonly property bool pendingFocusIndicatorVisible:
        pendingFocusAnchor.activeFocus && pendingFocusAnchorOrigin >= 0

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

    function pendingFocusOrigin() {
        return pendingFocusAnchorOrigin
    }

    function focusControl(index, reason) {
        cancelPendingFocusRecovery()
        const candidate = controlAt(index)
        if (candidate !== null && candidate.enabled) {
            if (candidate.activeFocus
                    && (reason === Qt.TabFocusReason
                        || reason === Qt.BacktabFocusReason)
                    && !candidate.visualFocus)
                candidate.focus = false
            candidate.forceActiveFocus(reason)
        }
    }

    function focusNextFrom(index, direction, reason) {
        for (let candidateIndex = index + direction;
             candidateIndex >= 0 && candidateIndex < focusableControlCount;
             candidateIndex += direction) {
            const candidate = controlAt(candidateIndex)
            if (candidate !== null && candidate.enabled) {
                focusControl(candidateIndex, reason)
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
        cancelPendingFocusRecovery()
        closeRequested()
    }

    function cancelPendingFocusRecovery() {
        ++focusRecoverySerial
        pendingProfileFocusRecovery = -1
        pendingProfileVisualFocusRecovery = false
        pendingProfileFocusRecoverySerial = -1
        pendingDoNotDisturbFocusRecovery = false
        pendingDoNotDisturbVisualFocusRecovery = false
        pendingDoNotDisturbFocusRecoverySerial = -1
        pendingFocusAnchorOrigin = -1
    }

    function activateButton(control, event) {
        event.accepted = true
        if (control.enabled)
            control.clicked()
    }

    function activateDoNotDisturb(event) {
        event.accepted = true
        if (doNotDisturb.enabled)
            requestDoNotDisturb(!doNotDisturb.checked)
    }

    function requestProfile(index, requestedProfile) {
        cancelPendingFocusRecovery()
        const recoverySerial = focusRecoverySerial
        const candidate = controlAt(index)
        const hadFocus = candidate !== null && candidate.activeFocus
        const hadVisualFocus = candidate !== null && candidate.visualFocus
        // Arm recovery before entering the control port.  A fast local
        // completion may publish the replacement generation reentrantly.
        pendingProfileFocusRecovery = hadFocus ? index : -1
        pendingProfileVisualFocusRecovery = hadFocus && hadVisualFocus
        pendingProfileFocusRecoverySerial = hadFocus ? recoverySerial : -1
        pendingFocusAnchorOrigin = hadFocus ? index : -1
        if (hadFocus) {
            pendingFocusAnchor.forceActiveFocus(Qt.OtherFocusReason)
        }
        const accepted = requestedProfile === "lustre"
            ? quickSettingsModel.requestLustreProfile()
            : quickSettingsModel.requestForgeProfile()
        if (!accepted) {
            // Also invalidates a deferred recovery queued by a synchronous
            // rejected publication.
            cancelPendingFocusRecovery()
            if (pendingFocusAnchor.activeFocus && candidate.enabled) {
                candidate.forceActiveFocus(hadVisualFocus
                                           ? Qt.TabFocusReason
                                           : Qt.OtherFocusReason)
            } else if (pendingFocusAnchor.activeFocus) {
                focusFallbackFrom(index, hadVisualFocus)
            }
            return
        }
        if (!quickSettingsModel.profile.pending
                && pendingProfileFocusRecoverySerial === recoverySerial) {
            // No pending transition and no completion signal consumed the
            // pre-armed recovery (for example, selecting the current profile).
            pendingProfileFocusRecovery = -1
            pendingProfileVisualFocusRecovery = false
            pendingProfileFocusRecoverySerial = -1
            if (pendingFocusAnchor.activeFocus && candidate.enabled) {
                candidate.forceActiveFocus(hadVisualFocus
                                           ? Qt.TabFocusReason
                                           : Qt.OtherFocusReason)
                pendingFocusAnchorOrigin = -1
            } else if (pendingFocusAnchor.activeFocus) {
                focusFallbackFrom(index, hadVisualFocus)
            }
        }
    }

    function requestDoNotDisturb(enabled) {
        cancelPendingFocusRecovery()
        const recoverySerial = focusRecoverySerial
        const hadFocus = doNotDisturb.activeFocus
        const hadVisualFocus = doNotDisturb.visualFocus
        pendingDoNotDisturbFocusRecovery = hadFocus
        pendingDoNotDisturbVisualFocusRecovery = hadFocus && hadVisualFocus
        pendingDoNotDisturbFocusRecoverySerial = hadFocus ? recoverySerial : -1
        pendingFocusAnchorOrigin = hadFocus ? 2 : -1
        if (hadFocus) {
            pendingFocusAnchor.forceActiveFocus(Qt.OtherFocusReason)
        }
        const accepted = quickSettingsModel.requestDoNotDisturb(enabled)
        if (!accepted) {
            cancelPendingFocusRecovery()
            if (pendingFocusAnchor.activeFocus && doNotDisturb.enabled) {
                doNotDisturb.forceActiveFocus(hadVisualFocus
                                              ? Qt.TabFocusReason
                                              : Qt.OtherFocusReason)
            } else if (pendingFocusAnchor.activeFocus) {
                focusFallbackFrom(2, hadVisualFocus)
            }
            return
        }
        if (!quickSettingsModel.doNotDisturb.pending
                && pendingDoNotDisturbFocusRecoverySerial === recoverySerial) {
            pendingDoNotDisturbFocusRecovery = false
            pendingDoNotDisturbVisualFocusRecovery = false
            pendingDoNotDisturbFocusRecoverySerial = -1
            if (pendingFocusAnchor.activeFocus && doNotDisturb.enabled) {
                doNotDisturb.forceActiveFocus(hadVisualFocus
                                              ? Qt.TabFocusReason
                                              : Qt.OtherFocusReason)
                pendingFocusAnchorOrigin = -1
            } else if (pendingFocusAnchor.activeFocus) {
                focusFallbackFrom(2, hadVisualFocus)
            }
        }
    }

    function focusFallbackFrom(index, visualFocus) {
        const reason = visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason
        for (let candidateIndex = index + 1;
             candidateIndex < focusableControlCount; ++candidateIndex) {
            const candidate = controlAt(candidateIndex)
            if (candidate !== null && candidate.enabled) {
                focusControl(candidateIndex, reason)
                return
            }
        }
        for (let candidateIndex = index - 1; candidateIndex >= 0; --candidateIndex) {
            const candidate = controlAt(candidateIndex)
            if (candidate !== null && candidate.enabled) {
                focusControl(candidateIndex, reason)
                return
            }
        }
        cancelPendingFocusRecovery()
        focusExitForward()
    }

    function restorePendingFocus(index, restoreVisualFocus, recoverySerial,
                                 attemptsRemaining) {
        Qt.callLater(function() {
            if (recoverySerial !== root.focusRecoverySerial)
                return
            if (root.Window.window === null || !root.Window.window.visible) {
                root.cancelPendingFocusRecovery()
                return
            }
            const candidate = controlAt(index)
            if (candidate === null || !candidate.enabled) {
                if (attemptsRemaining > 0)
                    root.restorePendingFocus(index, restoreVisualFocus,
                                             recoverySerial,
                                             attemptsRemaining - 1)
                else if (pendingFocusAnchor.activeFocus)
                    root.focusFallbackFrom(index, restoreVisualFocus)
                return
            }
            const activeFocusItem = root.Window.window.activeFocusItem
            if (!candidate.activeFocus && activeFocusItem !== pendingFocusAnchor) {
                if (root.pendingFocusAnchorOrigin === index)
                    root.pendingFocusAnchorOrigin = -1
                return
            }
            if (candidate.activeFocus && restoreVisualFocus
                    && !candidate.visualFocus)
                candidate.focus = false
            candidate.forceActiveFocus(restoreVisualFocus
                                       ? Qt.TabFocusReason
                                       : Qt.OtherFocusReason)
            if (root.pendingFocusAnchorOrigin === index)
                root.pendingFocusAnchorOrigin = -1
        })
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

    Item {
        id: pendingFocusAnchor
        width: 0
        height: 0
        Accessible.ignored: true

        Keys.priority: Keys.BeforeItem
        Keys.onTabPressed: event => root.handleForward(
            root.pendingFocusOrigin(), event)
        Keys.onBacktabPressed: event => root.handleBackward(
            root.pendingFocusOrigin(), event)
        Keys.onEscapePressed: event => root.closeFromEscape(event)
        Keys.onUpPressed: event => root.handleVertical(
            root.pendingFocusOrigin(), -1, event)
        Keys.onDownPressed: event => root.handleVertical(
            root.pendingFocusOrigin(), 1, event)
        Keys.onLeftPressed: event => {
            if (root.pendingFocusAnchorOrigin === 0
                    || root.pendingFocusAnchorOrigin === 1)
                root.handleHorizontal(root.pendingFocusOrigin(), -1, event)
            else
                event.accepted = true
        }
        Keys.onRightPressed: event => {
            if (root.pendingFocusAnchorOrigin === 0
                    || root.pendingFocusAnchorOrigin === 1)
                root.handleHorizontal(root.pendingFocusOrigin(), 1, event)
            else
                event.accepted = true
        }
    }

    Rectangle {
        id: pendingFocusIndicator
        readonly property Item target: root.pendingFocusAnchorOrigin >= 0
            ? root.controlAt(root.pendingFocusAnchorOrigin) : null
        x: target !== null ? target.mapToItem(root, 0, 0).x : 0
        y: target !== null ? target.mapToItem(root, 0, 0).y : 0
        width: target !== null ? target.width : 0
        height: target !== null ? target.height : 0
        z: 100
        visible: pendingFocusAnchor.activeFocus
                 && root.pendingFocusAnchorOrigin >= 0
        color: "transparent"
        border.color: root.tokens.focusColor
        border.width: Math.max(2, root.tokens.borderWidth)
        radius: root.tokens.cardRadius
        Accessible.ignored: true
    }

    onVisibleChanged: {
        if (!visible)
            cancelPendingFocusRecovery()
    }

    Connections {
        target: root.Window.window
        function onVisibleChanged() {
            if (root.Window.window === null || !root.Window.window.visible)
                root.cancelPendingFocusRecovery()
        }
    }

    Connections {
        target: root.quickSettingsModel.profile
        function onChanged() {
            if (!root.quickSettingsModel.profile.pending
                    && root.pendingProfileFocusRecovery >= 0) {
                const index = root.pendingProfileFocusRecovery
                const restoreVisualFocus = root.pendingProfileVisualFocusRecovery
                const recoverySerial = root.pendingProfileFocusRecoverySerial
                root.pendingProfileFocusRecovery = -1
                root.pendingProfileVisualFocusRecovery = false
                root.pendingProfileFocusRecoverySerial = -1
                // The private anchor owns focus across the disabled-control
                // gap; restore only while that lease remains intact.
                root.restorePendingFocus(index, restoreVisualFocus,
                                         recoverySerial, 2)
            }
        }
    }

    Connections {
        target: root.quickSettingsModel.doNotDisturb
        function onChanged() {
            if (!root.quickSettingsModel.doNotDisturb.pending
                    && root.pendingDoNotDisturbFocusRecovery) {
                const restoreVisualFocus =
                    root.pendingDoNotDisturbVisualFocusRecovery
                const recoverySerial =
                    root.pendingDoNotDisturbFocusRecoverySerial
                root.pendingDoNotDisturbFocusRecovery = false
                root.pendingDoNotDisturbVisualFocusRecovery = false
                root.pendingDoNotDisturbFocusRecoverySerial = -1
                root.restorePendingFocus(2, restoreVisualFocus,
                                         recoverySerial, 2)
            }
        }
    }

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
                Accessible.focusable: enabled || (pendingFocusAnchor.activeFocus
                                                    && pendingFocusAnchorOrigin === 0)
                Accessible.focused: activeFocus || (!enabled
                                                     && pendingFocusAnchor.activeFocus
                                                     && pendingFocusAnchorOrigin === 0)
                Accessible.onPressAction: {
                    if (enabled)
                        clicked()
                }

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
                onClicked: root.requestProfile(0, "lustre")
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
                Accessible.focusable: enabled || (pendingFocusAnchor.activeFocus
                                                    && pendingFocusAnchorOrigin === 1)
                Accessible.focused: activeFocus || (!enabled
                                                     && pendingFocusAnchor.activeFocus
                                                     && pendingFocusAnchorOrigin === 1)
                Accessible.onPressAction: {
                    if (enabled)
                        clicked()
                }

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
                onClicked: root.requestProfile(1, "forge")
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
            Accessible.focusable: enabled || (pendingFocusAnchor.activeFocus
                                                && pendingFocusAnchorOrigin === 2)
            Accessible.focused: activeFocus || (!enabled
                                                 && pendingFocusAnchor.activeFocus
                                                 && pendingFocusAnchorOrigin === 2)
            Accessible.onToggleAction: {
                if (enabled)
                    root.requestDoNotDisturb(!checked)
            }

            Keys.priority: Keys.BeforeItem
            Keys.onTabPressed: event => root.handleForward(2, event)
            Keys.onBacktabPressed: event => root.handleBackward(2, event)
            Keys.onEscapePressed: event => root.closeFromEscape(event)
            Keys.onUpPressed: event => root.handleVertical(2, -1, event)
            Keys.onDownPressed: event => root.handleVertical(2, 1, event)
            Keys.onReturnPressed: event => root.activateDoNotDisturb(event)
            Keys.onEnterPressed: event => root.activateDoNotDisturb(event)
            onClicked: root.requestDoNotDisturb(checked)
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
