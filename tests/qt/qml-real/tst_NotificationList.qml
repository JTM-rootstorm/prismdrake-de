import QtQuick
import QtTest
import "../../../src/shell/notifications/qml" as Notifications

// The Quick Test setup publishes this test-only context fixture before loading the file.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "NotificationListRealModel"
    when: windowShown
    width: 680
    height: 720

    Notifications.NotificationPresentationTokens {
        id: tokens
        profileId: "forge"
        profileDisplayName: "Prismdrake Forge"
        surfaceColor: "#494237"
        borderColor: "#c7a965"
        textPrimaryColor: "#ffffff"
        textMutedColor: "#dfd5c2"
        focusColor: "#ffe08a"
        criticalColor: "#a72f46"
        controlColor: "#625747"
        pressedControlColor: "#7a6b56"
        cardRadius: 5
        cardPadding: 10
        borderWidth: 2
        focusWidth: 3
        minimumTargetSize: 48
        bodyFontPixels: 14
        titleFontPixels: 16
        motionDurationMs: 60
        reducedMotion: false
        opaqueFallbackActive: true
        highContrast: false
    }

    Notifications.NotificationList {
        id: list
        x: 20
        y: 20
        width: 620
        presentationModel: realNotificationFixture.presentationModel
        tokens: tokens
    }

    SignalSpy { id: forwardExit; target: list; signalName: "focusExitForward" }

    function init() {
        verify(realNotificationFixture.reset())
        forwardExit.clear()
        tryCompare(list, "count", 0)
    }

    function addThree() {
        verify(realNotificationFixture.addCard("First"))
        verify(realNotificationFixture.addCard("Second"))
        verify(realNotificationFixture.addCard("Third"))
        tryCompare(list, "count", 3)
    }

    function focusAction(cardIndex) {
        list.focusCard(cardIndex, false)
        tryCompare(list.cardAt(cardIndex), "cardHasFocus", true)
        keyClick(Qt.Key_Tab)
        const action = findChild(list.cardAt(cardIndex), "notificationAction0")
        verify(action !== null)
        tryCompare(action, "activeFocus", true)
        return action
    }

    function test_focusedRemovalSelectsNextThenPrevious() {
        addThree()
        list.focusCard(1, false)
        tryCompare(list.cardAt(1), "cardHasFocus", true)
        verify(realNotificationFixture.dismissCard(1))
        tryCompare(list, "count", 2)
        tryCompare(list.cardAt(1), "cardHasFocus", true)
        compare(list.cardAt(1).presentation.summary, "Third")

        verify(realNotificationFixture.dismissCard(1))
        tryCompare(list, "count", 1)
        tryCompare(list.cardAt(0), "cardHasFocus", true)
        compare(list.cardAt(0).presentation.summary, "First")
    }

    function test_timeoutRemovalSelectsNext() {
        verify(realNotificationFixture.addCard("First"))
        verify(realNotificationFixture.addCard("Expiring", 10))
        verify(realNotificationFixture.addCard("Third"))
        tryCompare(list, "count", 3)
        list.focusCard(1, false)
        tryCompare(list.cardAt(1), "cardHasFocus", true)

        verify(realNotificationFixture.advanceTimeouts(10))
        tryCompare(list, "count", 2)
        tryCompare(list.cardAt(1), "cardHasFocus", true)
        compare(list.cardAt(1).presentation.summary, "Third")
    }

    function test_replacementReturnsFocusToCard() {
        addThree()
        focusAction(1)
        verify(realNotificationFixture.replaceCard(1, "Second replaced"))
        tryCompare(list.cardAt(1), "cardHasFocus", true)
        compare(list.cardAt(1).presentation.summary, "Second replaced")
    }

    function test_unaffectedCardPreservesActionFocusAcrossEarlierRemoval() {
        addThree()
        focusAction(1)
        verify(realNotificationFixture.dismissCard(0))
        tryCompare(list, "count", 2)
        compare(list.cardAt(0).presentation.summary, "Second")
        const restoredAction = findChild(list.cardAt(0), "notificationAction0")
        verify(restoredAction !== null)
        tryCompare(restoredAction, "activeFocus", true)
    }

    function test_duplicateCardsPreserveFocusByPresentationIdentity() {
        verify(realNotificationFixture.addCard("Duplicate"))
        verify(realNotificationFixture.addCard("Duplicate"))
        verify(realNotificationFixture.addCard("Duplicate"))
        tryCompare(list, "count", 3)
        focusAction(2)

        verify(realNotificationFixture.dismissCard(0))
        tryCompare(list, "count", 2)
        const firstAction = findChild(list.cardAt(0), "notificationAction0")
        const focusedAction = findChild(list.cardAt(1), "notificationAction0")
        verify(firstAction !== null)
        verify(focusedAction !== null)
        tryCompare(focusedAction, "activeFocus", true)
        compare(firstAction.activeFocus, false)
    }

    function test_emptyFocusedListEmitsFocusExit() {
        verify(realNotificationFixture.addCard("Only"))
        tryCompare(list, "count", 1)
        list.focusCard(0, false)
        tryCompare(list.cardAt(0), "cardHasFocus", true)
        verify(realNotificationFixture.dismissCard(0))
        tryCompare(list, "count", 0)
        tryCompare(forwardExit, "count", 1)
    }
}

// qmllint enable unqualified
