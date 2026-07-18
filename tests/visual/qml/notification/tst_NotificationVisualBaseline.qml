import QtQuick
import QtTest
import "../../../../src/shell/notifications/qml" as Notifications

// The Quick Test setup publishes real theme/model adapters and the recorder.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "NotificationVisualBaseline"
    when: windowShown
    width: 680
    height: 720
    visible: true

    readonly property var generation: visualThemeFixture.themeGeneration
    readonly property var notificationTokens: generation.notification

    Notifications.NotificationPresentationTokens {
        id: tokens
        profileId: testCase.generation.profileId
        profileDisplayName: testCase.generation.profileDisplayName
        surfaceColor: testCase.notificationTokens.surfaceColor
        borderColor: testCase.notificationTokens.borderColor
        textPrimaryColor: testCase.notificationTokens.textPrimaryColor
        textMutedColor: testCase.notificationTokens.textMutedColor
        focusColor: testCase.notificationTokens.focusColor
        criticalColor: testCase.notificationTokens.criticalColor
        controlColor: testCase.notificationTokens.controlColor
        pressedControlColor: testCase.notificationTokens.pressedControlColor
        cardRadius: testCase.notificationTokens.cardRadius
        cardPadding: testCase.notificationTokens.cardPadding
        borderWidth: testCase.notificationTokens.borderWidth
        focusWidth: testCase.notificationTokens.focusWidth
        minimumTargetSize: testCase.notificationTokens.minimumTargetSize
        bodyFontFamily: testCase.notificationTokens.bodyFontFamily
        bodyFontPixels: testCase.notificationTokens.bodyFontPixels
        titleFontPixels: testCase.notificationTokens.titleFontPixels
        motionDurationMs: testCase.notificationTokens.fastMotionMs
        reducedMotion: testCase.notificationTokens.reducedMotion
        opaqueFallbackActive: testCase.notificationTokens.fallbackActive
        highContrast: testCase.notificationTokens.highContrast
    }

    Notifications.NotificationList {
        id: list
        x: 20
        y: 20
        width: cardWidth
        height: implicitHeight
        presentationModel: notificationFixture.presentationModel
        tokens: tokens
    }

    function capture(name) {
        let completed = false
        let recorded = false
        const target = list.cardAt(0)
        verify(target !== null)
        tryVerify(() => target.width > 0 && target.height > 0)
        const captureWidth = Math.round(target.width)
        const captureHeight = Math.round(target.height)
        const accepted = target.grabToImage(result => {
            const saved = result.saveToFile(baselineRecorder.imagePath(name))
            recorded = saved && baselineRecorder.record(
                name, testCase.generation, captureWidth, captureHeight,
                target.Screen.devicePixelRatio, "ltr", true, false)
            completed = true
        })
        verify(accepted)
        tryVerify(() => completed, 5000)
        verify(recorded, baselineRecorder.lastError)
        verify(baselineRecorder.metadataComplete(name))
    }

    function initTestCase() {
        failOnWarning(/.*/)
        verify(baselineRecorder.expectedFontAvailable())
    }

    function test_captureSharedProfileUrgencyAndFallbackStates() {
        verify(visualThemeFixture.resetLustre())
        verify(notificationFixture.reset())
        verify(notificationFixture.addCard("Build completed"))
        tryCompare(list, "count", 1)
        list.focusCard(0, false)
        tryCompare(list.cardAt(0), "cardHasFocus", true)
        capture("notification-lustre-normal")

        const sharedTree = list
        verify(visualThemeFixture.publishForge())
        verify(notificationFixture.reset())
        verify(notificationFixture.addCriticalCard("Build requires attention"))
        tryCompare(testCase.generation, "profileId", "forge")
        tryCompare(list, "count", 1)
        compare(list, sharedTree)
        list.focusCard(0, false)
        compare(list.cardAt(0).presentation.urgencyId, "critical")
        capture("notification-forge-urgent")

        verify(visualThemeFixture.resetAccessible())
        verify(notificationFixture.reset())
        verify(notificationFixture.addCriticalCard("Accessible urgent notification"))
        tryCompare(testCase.generation, "highContrast", true)
        compare(testCase.generation.reducedMotion, true)
        compare(testCase.generation.transparencyDisabled, true)
        compare(tokens.opaqueFallbackActive, true)
        list.focusCard(0, false)
        capture("notification-accessible-urgent-fallback")
    }
}

// qmllint enable unqualified
