import QtQuick
import QtTest
import "../../../notifications/qml" as Notifications

// The Quick Test setup publishes this test-only context fixture before loading the file.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "NotificationCenterSurface"
    when: windowShown
    visible: true
    width: 1000
    height: 760

    QtObject {
        id: themeGeneration
        property string profileId: "forge"
        property string profileDisplayName: "Prismdrake Forge"
        property QtObject notification: QtObject {
            property color surfaceColor: "#494237"
            property color borderColor: "#c7a965"
            property color textPrimaryColor: "#ffffff"
            property color textMutedColor: "#dfd5c2"
            property color focusColor: "#ffe08a"
            property color criticalColor: "#a72f46"
            property color controlColor: "#625747"
            property color pressedControlColor: "#7a6b56"
            property real cardRadius: 5
            property real cardPadding: 10
            property real borderWidth: 2
            property real focusWidth: 3
            property real minimumTargetSize: 48
            property string bodyFontFamily: "sans-serif"
            property int bodyFontPixels: 14
            property int titleFontPixels: 16
            property int fastMotionMs: 60
            property bool reducedMotion: false
            property bool fallbackActive: true
            property bool highContrast: false
        }
    }

    Notifications.NotificationCenterSurface {
        id: center
        x: 20
        y: 20
        width: 600
        height: 700
        presentation: notificationCenterFixture.presentation
        themeGeneration: themeGeneration
    }

    Notifications.TransientNotificationStack {
        id: transients
        x: 640
        y: 20
        width: 340
        presentation: notificationCenterFixture.presentation
        themeGeneration: themeGeneration
    }

    SignalSpy { id: closeSpy; target: center; signalName: "closeRequested" }
    SignalSpy { id: returnSpy; target: center; signalName: "focusReturnRequested" }
    SignalSpy {
        id: dndRequestSpy
        target: notificationCenterFixture.presentation
        signalName: "doNotDisturbRequested"
    }

    function init() {
        notificationCenterFixture.reset()
        center.layoutDirection = Qt.LeftToRight
        transients.layoutDirection = Qt.LeftToRight
        themeGeneration.notification.minimumTargetSize = 48
        themeGeneration.notification.bodyFontPixels = 14
        themeGeneration.notification.highContrast = false
        themeGeneration.notification.reducedMotion = false
        themeGeneration.notification.fallbackActive = true
        closeSpy.clear()
        returnSpy.clear()
        dndRequestSpy.clear()
        tryCompare(transients, "count", 0)
    }

    function test_unavailableAndLiteralAccessibleContent() {
        const status = findChild(center, "notificationCenterStatus")
        const dnd = findChild(center, "notificationCenterDoNotDisturb")
        verify(status !== null)
        verify(dnd !== null)
        compare(status.text, "Notification history is unavailable.")
        compare(dnd.enabled, false)
        compare(dnd.Accessible.focusable, false)

        verify(notificationCenterFixture.publish(1))
        tryCompare(transients, "count", 1)
        const history = findChild(center, "notificationCenterHistoryScroll")
        verify(history !== null)
        const summary = findChild(history, "notificationSummary")
        verify(summary !== null)
        compare(summary.text, "<b>Literal summary</b>")
        compare(summary.textFormat, Text.PlainText)
        const card = findChild(history, "notificationCardBackground").parent
        compare(card.Accessible.name, "<b>Literal summary</b>")
    }

    function test_keyboardTraversalAndFocusReturn() {
        verify(notificationCenterFixture.publish(1))
        center.focusFirstControl()
        const dnd = findChild(center, "notificationCenterDoNotDisturb")
        const clear = findChild(center, "notificationCenterClearHistory")
        verify(dnd !== null)
        verify(clear !== null)
        verify(clear.visible)
        tryCompare(dnd, "activeFocus", true)
        compare(dnd.Accessible.name, "Do not disturb")
        keyClick(Qt.Key_Space)
        tryCompare(dndRequestSpy, "count", 1)
        compare(dndRequestSpy.signalArguments[0][0], true)

        keyClick(Qt.Key_Down)
        tryCompare(clear, "activeFocus", true)
        keyClick(Qt.Key_Up)
        tryCompare(dnd, "activeFocus", true)

        keyClick(Qt.Key_Tab)
        tryCompare(clear, "activeFocus", true)
        compare(clear.Accessible.name, "Clear notification history")
        keyClick(Qt.Key_Tab)
        const history = findChild(center, "notificationCenterHistoryScroll")
        const card = findChild(history, "notificationCardBackground").parent
        tryCompare(card, "activeFocus", true)

        keyClick(Qt.Key_Escape)
        tryCompare(closeSpy, "count", 1)
        tryCompare(returnSpy, "count", 1)
    }

    function test_transientStackIsBoundedToNewestFive() {
        verify(notificationCenterFixture.publish(8, true))
        tryCompare(transients, "count", 5)
        const dndStatus = findChild(center, "notificationCenterStatus")
        compare(dndStatus.visible, false)
        verify(center.Accessible.description.length > 0)
        compare(transients.Accessible.name, "Transient notifications")
    }

    function test_longNotificationContentWrapsInsideTheCenter() {
        verify(notificationCenterFixture.publishLong())
        const history = findChild(center, "notificationCenterHistoryScroll")
        const summary = findChild(history, "notificationSummary")
        const body = findChild(history, "notificationBody")
        const card = findChild(history, "notificationCardBackground").parent
        verify(summary !== null)
        verify(body !== null)
        verify(card !== null)
        verify(summary.text.length > 900)
        verify(body.text.length > 900)
        compare(summary.wrapMode, Text.Wrap)
        compare(body.wrapMode, Text.Wrap)
        verify(card.width <= center.width)
        verify(card.Accessible.name.length > 900)
        verify(card.Accessible.description.length > 900)
    }

    function test_rtlFallbackContrastAndScaledTargetsUsePublishedThemeInputs() {
        verify(notificationCenterFixture.publish(1))
        center.layoutDirection = Qt.RightToLeft
        transients.layoutDirection = Qt.RightToLeft
        themeGeneration.notification.highContrast = true
        themeGeneration.notification.reducedMotion = true
        themeGeneration.notification.minimumTargetSize = 56
        themeGeneration.notification.bodyFontPixels = 22

        compare(center.LayoutMirroring.enabled, true)
        compare(transients.LayoutMirroring.enabled, true)
        const dnd = findChild(center, "notificationCenterDoNotDisturb")
        const clear = findChild(center, "notificationCenterClearHistory")
        const card = findChild(center, "notificationCardBackground").parent
        tryVerify(() => dnd.height >= 56)
        tryVerify(() => clear.height >= 56)
        compare(card.effectiveMotionDuration, 0)
        compare(card.tokens.highContrast, true)
        compare(card.tokens.opaqueFallbackActive, true)
        compare(card.tokens.bodyFontPixels, 22)
    }
}

// qmllint enable unqualified
