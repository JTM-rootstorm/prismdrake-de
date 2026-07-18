import QtQuick
import QtTest
import "../../../src/shell/notifications/qml" as Notifications

// The Quick Test setup publishes this test-only context fixture before loading the file.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "NotificationSurfaceRealModel"
    when: windowShown
    width: 680
    height: 720

    QtObject {
        id: notificationTokens

        property color surfaceColor: "#252a35"
        property bool fallbackActive: true
        property color borderColor: "#9ba7ba"
        property color textPrimaryColor: "#ffffff"
        property color textMutedColor: "#d8deea"
        property color focusColor: "#6edcff"
        property color criticalColor: "#d84a68"
        property color controlColor: "#384154"
        property color pressedControlColor: "#4a5770"
        property real cardRadius: 9
        property real cardPadding: 12
        property real borderWidth: 1
        property real focusWidth: 3
        property real minimumTargetSize: 48
        property string bodyFontFamily: "sans-serif"
        property real bodyFontPixels: 14
        property real titleFontPixels: 17
        property int fastMotionMs: 80
        property bool reducedMotion: false
        property bool highContrast: false
    }

    QtObject {
        id: themeGeneration

        property string profileId: "lustre"
        property string profileDisplayName: "Prismdrake Lustre"
        property QtObject notification: notificationTokens
    }

    Notifications.NotificationSurface {
        id: surface
        x: 20
        y: 20
        presentationModel: realNotificationFixture.presentationModel
        themeGeneration: themeGeneration
    }

    function init() {
        verify(realNotificationFixture.reset())
        tryCompare(surface, "count", 0)
    }

    function test_projectsGenerationAndFocusesFixedSurfaceContent() {
        verify(realNotificationFixture.addCriticalCard("Installed shell fixture"))
        tryCompare(surface, "count", 1)
        verify(surface.width >= notificationTokens.minimumTargetSize)
        verify(surface.height >= notificationTokens.minimumTargetSize)

        surface.focusFirstCard()
        const card = surface.cardAt(0)
        verify(card !== null)
        tryCompare(card, "cardHasFocus", true)
        compare(card.presentation.summary, "Installed shell fixture")
    }
}

// qmllint enable unqualified
