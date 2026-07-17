pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

FocusScope {
    id: root

    required property var presentationModel
    required property NotificationPresentationTokens tokens
    property real cardWidth: 420
    property var pendingFocusRecovery: null
    readonly property int count: cardRepeater.count

    signal focusExitForward
    signal focusExitBackward

    function cardAt(index) {
        return cardRepeater.itemAt(index)
    }

    function focusCard(index, backward) {
        const candidate = cardRepeater.itemAt(index)
        if (candidate === null) {
            if (backward)
                root.focusExitBackward()
            else
                root.focusExitForward()
            return
        }
        // Repeater.itemAt() is statically a QQuickItem; every delegate is a NotificationCard.
        // qmllint disable
        if (backward)
            candidate["focusLastAvailable"]()
        else
            candidate["focusCard"]()
        // qmllint enable
    }

    function captureFocusRecovery() {
        // Repeater.itemAt() is statically a QQuickItem; every delegate is a NotificationCard.
        // qmllint disable
        for (let index = 0; index < cardRepeater.count; ++index) {
            const candidate = cardRepeater.itemAt(index)
            if (candidate !== null && candidate["hasFocusWithin"]()) {
                pendingFocusRecovery = {
                    "index": index,
                    "presentation": candidate["presentation"],
                    "state": candidate["focusState"]()
                }
                return
            }
        }
        // qmllint enable
    }

    function recoverFocus() {
        // Repeater.itemAt() is statically a QQuickItem; every delegate is a NotificationCard.
        // qmllint disable
        const recovery = pendingFocusRecovery
        pendingFocusRecovery = null
        if (recovery === null)
            return
        if (cardRepeater.count === 0) {
            root.focusExitForward()
            return
        }

        for (let index = 0; index < cardRepeater.count; ++index) {
            const candidate = cardRepeater.itemAt(index)
            if (candidate !== null
                    && candidate["presentation"] === recovery.presentation) {
                candidate["restoreFocus"](recovery.state)
                return
            }
        }

        const fallbackIndex = Math.min(recovery.index, cardRepeater.count - 1)
        const fallback = cardRepeater.itemAt(fallbackIndex)
        if (fallback !== null)
            fallback["focusCard"]()
        else
            root.focusExitForward()
        // qmllint enable
    }

    implicitWidth: cardWidth
    implicitHeight: cards.implicitHeight

    Timer {
        id: focusRecoveryTimer
        interval: 0
        repeat: false
        onTriggered: root.recoverFocus()
    }

    Connections {
        target: root.presentationModel
        function onPublicationReconciliationStarted() {
            root.captureFocusRecovery()
        }
        function onPublicationApplied() {
            focusRecoveryTimer.restart()
        }
    }

    ColumnLayout {
        id: cards
        anchors.fill: parent
        spacing: 10

        Repeater {
            id: cardRepeater
            model: root.presentationModel

            delegate: NotificationCard {
                required property int index
                required property var card

                Layout.fillWidth: true
                presentation: card
                tokens: root.tokens
                onFocusExitForward: root.focusCard(index + 1, false)
                onFocusExitBackward: root.focusCard(index - 1, true)
            }
        }
    }
}
