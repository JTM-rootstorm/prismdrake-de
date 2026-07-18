pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var presentation
    required property NotificationPresentationTokens tokens

    readonly property bool hasPresentation: presentation !== null
        && presentation !== undefined
    readonly property string presentationApplicationName: hasPresentation
        ? presentation.applicationName
        : ""
    readonly property string presentationBody: hasPresentation ? presentation.body : ""
    readonly property bool presentationDismissible: hasPresentation
        && presentation.dismissible
    readonly property string presentationSummary: hasPresentation
        ? presentation.summary
        : ""
    readonly property string presentationUrgencyId: hasPresentation
        ? presentation.urgencyId
        : "normal"
    readonly property var presentationActions: hasPresentation
        ? presentation.actions
        : null
    readonly property int effectiveMotionDuration: tokens.reducedMotion
        ? 0
        : Math.max(0, tokens.motionDurationMs)
    readonly property bool cardHasFocus: root.activeFocus && !root.childControlHasFocus()
    readonly property string urgencyLabel: {
        switch (presentationUrgencyId) {
        case "low":
            return qsTr("Low priority")
        case "critical":
            return qsTr("Critical")
        default:
            return qsTr("Normal priority")
        }
    }
    readonly property string accessibleDescription: {
        let parts = []
        if (presentationApplicationName.length > 0)
            parts.push(presentationApplicationName)
        parts.push(urgencyLabel)
        if (presentationBody.length > 0)
            parts.push(presentationBody)
        return parts.join(". ")
    }

    signal focusExitForward
    signal focusExitBackward

    function childControlHasFocus() {
        if (dismissButton.activeFocus)
            return true
        for (let index = 0; index < actionRepeater.count; ++index) {
            const candidate = actionRepeater.itemAt(index)
            if (candidate !== null && candidate.activeFocus)
                return true
        }
        return false
    }

    function hasFocusWithin() {
        return root.cardHasFocus || root.childControlHasFocus()
    }

    function focusState() {
        if (root.cardHasFocus)
            return { "kind": "card", "actionIndex": -1 }
        for (let index = 0; index < actionRepeater.count; ++index) {
            const candidate = actionRepeater.itemAt(index)
            if (candidate !== null && candidate.activeFocus)
                return { "kind": "action", "actionIndex": index }
        }
        if (dismissButton.activeFocus)
            return { "kind": "dismiss", "actionIndex": -1 }
        return { "kind": "none", "actionIndex": -1 }
    }

    function restoreFocus(state) {
        if (state.kind === "action") {
            const candidate = actionRepeater.itemAt(state.actionIndex)
            if (candidate !== null && candidate.enabled) {
                candidate.forceActiveFocus(Qt.TabFocusReason)
                return
            }
        } else if (state.kind === "dismiss" && presentationDismissible) {
            dismissButton.forceActiveFocus(Qt.TabFocusReason)
            return
        }
        root.focusCard()
    }

    function focusCard() {
        root.forceActiveFocus(Qt.TabFocusReason)
    }

    function focusLastAvailable() {
        if (presentationDismissible) {
            dismissButton.forceActiveFocus(Qt.BacktabFocusReason)
            return
        }
        for (let index = actionRepeater.count - 1; index >= 0; --index) {
            const candidate = actionRepeater.itemAt(index)
            if (candidate !== null && candidate.enabled) {
                candidate.forceActiveFocus(Qt.BacktabFocusReason)
                return
            }
        }
        root.forceActiveFocus(Qt.BacktabFocusReason)
    }

    function focusableControls() {
        let controls = []
        for (let index = 0; index < actionRepeater.count; ++index) {
            const candidate = actionRepeater.itemAt(index)
            if (candidate !== null && candidate.enabled)
                controls.push(candidate)
        }
        if (presentationDismissible)
            controls.push(dismissButton)
        return controls
    }

    function moveFocusFrom(item, direction) {
        const controls = focusableControls()
        if (item === root) {
            if (direction > 0 && controls.length > 0) {
                controls[0].forceActiveFocus(Qt.TabFocusReason)
                return
            }
            if (direction < 0) {
                root.focusExitBackward()
                return
            }
        } else {
            const current = controls.indexOf(item)
            const next = current + direction
            if (current >= 0 && next >= 0 && next < controls.length) {
                controls[next].forceActiveFocus(direction > 0
                                                ? Qt.TabFocusReason
                                                : Qt.BacktabFocusReason)
                return
            }
            if (direction < 0 && current === 0) {
                root.forceActiveFocus(Qt.BacktabFocusReason)
                return
            }
        }
        if (direction > 0)
            root.focusExitForward()
        else
            root.focusExitBackward()
    }

    implicitWidth: 420
    implicitHeight: content.implicitHeight + tokens.cardPadding * 2
    activeFocusOnTab: true
    Accessible.role: Accessible.Notification
    Accessible.name: presentationSummary
    Accessible.description: accessibleDescription
    Accessible.focusable: true
    Accessible.focused: cardHasFocus

    Keys.onTabPressed: function(event) {
        root.moveFocusFrom(root, 1)
        event.accepted = true
    }
    Keys.onBacktabPressed: function(event) {
        root.moveFocusFrom(root, -1)
        event.accepted = true
    }
    Keys.onEscapePressed: function(event) {
        if (presentationDismissible) {
            presentation.requestDismissal()
            event.accepted = true
        }
    }

    Rectangle {
        id: background
        objectName: "notificationCardBackground"

        anchors.fill: parent
        color: root.tokens.surfaceColor
        radius: root.tokens.cardRadius
        border.width: root.cardHasFocus ? root.tokens.focusWidth : root.tokens.borderWidth
        border.color: root.cardHasFocus ? root.tokens.focusColor : root.tokens.borderColor

        Behavior on opacity {
            NumberAnimation { duration: root.effectiveMotionDuration }
        }
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: root.tokens.cardPadding
        spacing: Math.max(6, root.tokens.cardPadding / 2)

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                id: applicationName
                objectName: "notificationApplicationName"

                Layout.fillWidth: true
                visible: text.length > 0
                text: root.presentationApplicationName
                textFormat: Text.PlainText
                color: root.tokens.textMutedColor
                font.family: root.tokens.bodyFontFamily
                font.pixelSize: root.tokens.bodyFontPixels
                elide: Text.ElideRight
                Accessible.ignored: true
            }

            Rectangle {
                id: urgencyShape
                objectName: "notificationUrgencyShape"

                Layout.minimumHeight: Math.max(root.tokens.minimumTargetSize / 2,
                                               urgencyText.implicitHeight + 8)
                Layout.minimumWidth: urgencyText.implicitWidth + 16
                radius: Math.min(root.tokens.cardRadius, height / 2)
                color: root.presentationUrgencyId === "critical"
                    ? root.tokens.criticalColor
                    : root.tokens.controlColor
                border.width: root.tokens.highContrast ? root.tokens.borderWidth : 0
                border.color: root.tokens.textPrimaryColor
                Accessible.ignored: true

                Text {
                    id: urgencyText
                    objectName: "notificationUrgencyText"

                    anchors.centerIn: parent
                    text: root.urgencyLabel
                    textFormat: Text.PlainText
                    color: root.tokens.textPrimaryColor
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    font.bold: root.presentationUrgencyId === "critical"
                    Accessible.ignored: true
                }
            }
        }

        Text {
            id: summary
            objectName: "notificationSummary"

            Layout.fillWidth: true
            text: root.presentationSummary
            textFormat: Text.PlainText
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.titleFontPixels
            font.bold: true
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        Text {
            id: body
            objectName: "notificationBody"

            Layout.fillWidth: true
            visible: text.length > 0
            text: root.presentationBody
            textFormat: Text.PlainText
            color: root.tokens.textPrimaryColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels
            wrapMode: Text.Wrap
            maximumLineCount: 6
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                id: actionRepeater
                model: root.presentationActions

                delegate: Button {
                    id: actionButton

                    required property int index
                    required property var model
                    property var actionPresentation: model.action
                    readonly property bool hasActionPresentation:
                        actionPresentation !== null && actionPresentation !== undefined

                    objectName: "notificationAction" + index
                    text: hasActionPresentation ? actionPresentation.label : ""
                    enabled: hasActionPresentation && actionPresentation.enabled
                    activeFocusOnTab: false
                    implicitHeight: root.tokens.minimumTargetSize
                    implicitWidth: Math.max(root.tokens.minimumTargetSize,
                                            contentItem.implicitWidth + leftPadding + rightPadding)
                    leftPadding: 12
                    rightPadding: 12
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    Accessible.description: qsTr("Notification action for %1").arg(root.presentationSummary)
                    Accessible.focusable: enabled
                    Accessible.focused: activeFocus
                    Accessible.onPressAction: {
                        if (actionButton.enabled)
                            actionButton.clicked()
                    }
                    onClicked: {
                        if (hasActionPresentation)
                            actionPresentation.requestActivation()
                    }
                    Keys.onTabPressed: function(event) {
                        root.moveFocusFrom(actionButton, 1)
                        event.accepted = true
                    }
                    Keys.onBacktabPressed: function(event) {
                        root.moveFocusFrom(actionButton, -1)
                        event.accepted = true
                    }
                    Keys.onEscapePressed: function(event) {
                        if (root.presentationDismissible) {
                            root.presentation.requestDismissal()
                            event.accepted = true
                        }
                    }

                    contentItem: Text {
                        text: actionButton.text
                        textFormat: Text.PlainText
                        color: root.tokens.textPrimaryColor
                        font.family: root.tokens.bodyFontFamily
                        font.pixelSize: root.tokens.bodyFontPixels
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    background: Rectangle {
                        color: actionButton.down
                            ? root.tokens.pressedControlColor
                            : root.tokens.controlColor
                        radius: Math.max(2, root.tokens.cardRadius / 2)
                        border.width: actionButton.activeFocus
                            ? root.tokens.focusWidth
                            : root.tokens.borderWidth
                        border.color: actionButton.activeFocus
                            ? root.tokens.focusColor
                            : root.tokens.borderColor
                        opacity: actionButton.enabled ? 1 : 0.55
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                id: dismissButton
                objectName: "notificationDismiss"

                visible: root.presentationDismissible
                text: qsTr("Dismiss")
                activeFocusOnTab: false
                implicitHeight: root.tokens.minimumTargetSize
                implicitWidth: Math.max(root.tokens.minimumTargetSize, contentItem.implicitWidth + 24)
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.description: qsTr("Dismiss %1").arg(root.presentationSummary)
                Accessible.focusable: visible
                Accessible.focused: activeFocus
                Accessible.onPressAction: dismissButton.clicked()
                onClicked: {
                    if (root.presentationDismissible)
                        root.presentation.requestDismissal()
                }
                Keys.onTabPressed: function(event) {
                    root.moveFocusFrom(dismissButton, 1)
                    event.accepted = true
                }
                Keys.onBacktabPressed: function(event) {
                    root.moveFocusFrom(dismissButton, -1)
                    event.accepted = true
                }
                Keys.onEscapePressed: function(event) {
                    if (root.presentationDismissible) {
                        root.presentation.requestDismissal()
                        event.accepted = true
                    }
                }

                contentItem: Text {
                    text: dismissButton.text
                    textFormat: Text.PlainText
                    color: root.tokens.textPrimaryColor
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    color: dismissButton.down
                        ? root.tokens.pressedControlColor
                        : root.tokens.controlColor
                    radius: Math.max(2, root.tokens.cardRadius / 2)
                    border.width: dismissButton.activeFocus
                        ? root.tokens.focusWidth
                        : root.tokens.borderWidth
                    border.color: dismissButton.activeFocus
                        ? root.tokens.focusColor
                        : root.tokens.borderColor
                }
            }
        }
    }
}
