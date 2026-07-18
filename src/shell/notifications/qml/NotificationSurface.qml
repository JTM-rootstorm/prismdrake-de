pragma ComponentBehavior: Bound

import QtQuick

FocusScope {
    id: root

    required property var presentationModel
    required property var themeGeneration
    readonly property int count: list.count

    signal focusExitForward
    signal focusExitBackward

    function focusFirstCard() {
        list.focusCard(0, false)
    }

    function cardAt(index) {
        return list.cardAt(index)
    }

    implicitWidth: list.implicitWidth
    implicitHeight: Math.max(tokens.minimumTargetSize, list.implicitHeight)

    NotificationPresentationTokens {
        id: tokens

        profileId: root.themeGeneration.profileId
        profileDisplayName: root.themeGeneration.profileDisplayName
        surfaceColor: root.themeGeneration.notification.surfaceColor
        borderColor: root.themeGeneration.notification.borderColor
        textPrimaryColor: root.themeGeneration.notification.textPrimaryColor
        textMutedColor: root.themeGeneration.notification.textMutedColor
        focusColor: root.themeGeneration.notification.focusColor
        criticalColor: root.themeGeneration.notification.criticalColor
        controlColor: root.themeGeneration.notification.controlColor
        pressedControlColor: root.themeGeneration.notification.pressedControlColor
        cardRadius: root.themeGeneration.notification.cardRadius
        cardPadding: root.themeGeneration.notification.cardPadding
        borderWidth: root.themeGeneration.notification.borderWidth
        focusWidth: root.themeGeneration.notification.focusWidth
        minimumTargetSize: root.themeGeneration.notification.minimumTargetSize
        bodyFontFamily: root.themeGeneration.notification.bodyFontFamily
        bodyFontPixels: root.themeGeneration.notification.bodyFontPixels
        titleFontPixels: root.themeGeneration.notification.titleFontPixels
        motionDurationMs: root.themeGeneration.notification.reducedMotion
                          ? 0 : root.themeGeneration.notification.fastMotionMs
        reducedMotion: root.themeGeneration.notification.reducedMotion
        opaqueFallbackActive: root.themeGeneration.notification.fallbackActive
        highContrast: root.themeGeneration.notification.highContrast
    }

    NotificationList {
        id: list

        anchors.fill: parent
        presentationModel: root.presentationModel
        tokens: tokens
        onFocusExitForward: root.focusExitForward()
        onFocusExitBackward: root.focusExitBackward()
    }
}
