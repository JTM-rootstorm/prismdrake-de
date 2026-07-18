import QtQuick
import QtTest
import "../../../src/shell/notifications/qml" as Notifications

TestCase {
    id: testCase
    name: "NotificationCard"
    when: windowShown
    width: 640
    height: 520

    property int firstActionRequests: 0
    property int thirdActionRequests: 0
    property int dismissRequests: 0

    Notifications.NotificationPresentationTokens {
        id: tokens
        profileId: "lustre"
        profileDisplayName: "Prismdrake Lustre"
        surfaceColor: "#2c3956"
        borderColor: "#8da0bd"
        textPrimaryColor: "#ffffff"
        textMutedColor: "#d2d9e5"
        focusColor: "#8de2e9"
        criticalColor: "#b83252"
        controlColor: "#3b4c6b"
        pressedControlColor: "#526789"
        cardRadius: 12
        cardPadding: 14
        borderWidth: 1
        focusWidth: 3
        minimumTargetSize: 48
        bodyFontFamily: "sans-serif"
        bodyFontPixels: 14
        titleFontPixels: 16
        motionDurationMs: 120
        reducedMotion: false
        opaqueFallbackActive: false
        highContrast: false
    }

    QtObject {
        id: firstAction
        property string label: "<b>Open literally</b>"
        property bool enabled: true
        function requestActivation() {
            ++testCase.firstActionRequests
            return true
        }
    }

    QtObject {
        id: disabledAction
        property string label: "Unavailable"
        property bool enabled: false
        function requestActivation() {
            testCase.fail("A disabled action must not be requested")
            return false
        }
    }

    QtObject {
        id: thirdAction
        property string label: "Details"
        property bool enabled: true
        function requestActivation() {
            ++testCase.thirdActionRequests
            return true
        }
    }

    ListModel { id: actionModel; dynamicRoles: true }

    QtObject {
        id: cardFixture
        property string summary: "<b>Literal summary</b>"
        property string body: "Literal <script>body</script> & text"
        property string applicationName: "Fixture App"
        property string urgencyId: "critical"
        property bool dismissible: true
        property var actions: actionModel
        function requestDismissal() {
            ++testCase.dismissRequests
            return true
        }
    }

    Notifications.NotificationCard {
        id: card
        x: 20
        y: 20
        width: 580
        presentation: cardFixture
        tokens: tokens
    }

    function initTestCase() {
        actionModel.append({"action": firstAction, "label": firstAction.label})
        actionModel.append({"action": disabledAction, "label": disabledAction.label})
        actionModel.append({"action": thirdAction, "label": thirdAction.label})
        waitForRendering(card)
    }

    function init() {
        firstActionRequests = 0
        thirdActionRequests = 0
        dismissRequests = 0
        tokens.reducedMotion = false
        tokens.opaqueFallbackActive = false
        tokens.highContrast = false
        tokens.surfaceColor = "#2c3956"
        card.focusCard()
        tryCompare(card, "activeFocus", true)
    }

    function test_literalTextAndAccessibleNotificationMetadata() {
        const summary = findChild(card, "notificationSummary")
        const body = findChild(card, "notificationBody")
        const urgency = findChild(card, "notificationUrgencyText")
        verify(summary !== null)
        verify(body !== null)
        verify(urgency !== null)
        compare(summary.text, "<b>Literal summary</b>")
        compare(summary.textFormat, Text.PlainText)
        compare(body.text, "Literal <script>body</script> & text")
        compare(body.textFormat, Text.PlainText)
        compare(urgency.text, "Critical")
        verify(urgency.implicitWidth > 0)
        compare(card.Accessible.role, Accessible.Notification)
        compare(card.Accessible.name, "<b>Literal summary</b>")
        verify(card.Accessible.focused)
        verify(card.Accessible.description.indexOf("Fixture App") >= 0)
        verify(card.Accessible.description.indexOf("Critical") >= 0)
        verify(card.Accessible.description.indexOf("<script>") >= 0)
    }

    function test_reducedMotionOpaqueAndHighContrastInputs() {
        compare(card.effectiveMotionDuration, 120)
        tokens.reducedMotion = true
        compare(card.effectiveMotionDuration, 0)

        tokens.surfaceColor = "#141414"
        tokens.opaqueFallbackActive = true
        tokens.highContrast = true
        const background = findChild(card, "notificationCardBackground")
        const urgencyShape = findChild(card, "notificationUrgencyShape")
        compare(background.color.a, 1)
        compare(urgencyShape.border.width, tokens.borderWidth)
        compare(card.tokens.profileId, "lustre")
    }

    function test_keyboardOrderSkipsDisabledActionAndActivatesExactAffordance() {
        const action0 = findChild(card, "notificationAction0")
        const action1 = findChild(card, "notificationAction1")
        const action2 = findChild(card, "notificationAction2")
        const dismiss = findChild(card, "notificationDismiss")
        verify(action0 !== null)
        verify(action1 !== null)
        verify(action2 !== null)
        verify(dismiss !== null)
        verify(!action1.enabled)
        verify(action0.width >= tokens.minimumTargetSize)
        verify(action0.height >= tokens.minimumTargetSize)
        verify(dismiss.width >= tokens.minimumTargetSize)
        verify(dismiss.height >= tokens.minimumTargetSize)

        keyClick(Qt.Key_Tab)
        tryCompare(action0, "activeFocus", true)
        verify(!card.Accessible.focused)
        compare(action0.Accessible.role, Accessible.Button)
        compare(action0.Accessible.name, "<b>Open literally</b>")

        keyClick(Qt.Key_Tab)
        tryCompare(action2, "activeFocus", true)
        keyClick(Qt.Key_Space)
        compare(thirdActionRequests, 1)
        compare(firstActionRequests, 0)

        keyClick(Qt.Key_Tab)
        tryCompare(dismiss, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(action2, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        compare(dismissRequests, 1)
    }

    function test_sameComponentAcceptsForgeResolvedTokens() {
        tokens.profileId = "forge"
        tokens.profileDisplayName = "Prismdrake Forge"
        tokens.surfaceColor = "#494237"
        tokens.cardRadius = 5
        const background = findChild(card, "notificationCardBackground")
        compare(card.tokens.profileId, "forge")
        compare(background.color, tokens.surfaceColor)
        compare(background.radius, 5)
    }
}
