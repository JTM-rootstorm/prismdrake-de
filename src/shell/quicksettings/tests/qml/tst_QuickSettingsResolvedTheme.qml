import QtQuick
import QtTest
import "../../qml" as QuickSettings

// qmllint disable unqualified
TestCase {
    id: testCase
    name: "QuickSettingsResolvedTheme"
    when: windowShown
    width: 1000
    height: 900

    property int profileRequests: 0
    property int cancelRequests: 0

    QtObject {
        id: profileAdapter
        signal changed
        property string valueId: "lustre"
        property string valueText: "Prismdrake Lustre"
        property string description: "Choose the active Prismdrake profile."
        property string stateText: "Available"
        property bool interactive: true
        property bool pending: false
        property bool failed: false
    }

    QtObject {
        id: dndAdapter
        signal changed
        property string description: "Normal notifications may be presented."
        property string stateText: "Disabled"
        property bool interactive: true
        property bool pending: false
        property bool failed: false
        property bool checked: false
    }

    QtObject {
        id: centerAdapter
        property string description: "Open notification history and controls."
        property string stateText: "Available"
        property bool interactive: true
        property bool pending: false
        property bool failed: false
    }

    QtObject {
        id: logoutAdapter
        property string description: "Request a confirmed session logout."
        property string stateText: "Available"
        property bool interactive: true
        property bool pending: false
        property bool failed: false
    }

    QtObject {
        id: diagnosticsAdapter
        property string description: "No active degradation reported."
        property string stateText: "Enabled"
        property bool failed: false
    }

    QtObject {
        id: model
        property QtObject profile: profileAdapter
        property QtObject doNotDisturb: dndAdapter
        property QtObject notificationCenter: centerAdapter
        property QtObject logout: logoutAdapter
        property QtObject diagnostics: diagnosticsAdapter

        function requestLustreProfile() {
            ++testCase.profileRequests
            return true
        }
        function requestForgeProfile() { return true }
        function requestDoNotDisturb(enabled) { return enabled }
        function requestNotificationCenter() { return true }
        function requestLogout() { return true }
    }

    QuickSettings.QuickSettingsSurface {
        id: quickSettings
        x: 20
        y: 20
        width: 440
        height: implicitHeight
        quickSettingsModel: model
        themeGeneration: quickSettingsThemeFixture.themeGeneration
    }

    QuickSettings.LogoutConfirmationSurface {
        id: logoutConfirmation
        x: 520
        y: 40
        width: 440
        height: implicitHeight
        themeGeneration: quickSettingsThemeFixture.themeGeneration
        statusText: "Close the session and all applications?"
        transactionPending: false
        onCancelRequested: ++testCase.cancelRequests
    }

    function linearChannel(channel) {
        return channel <= 0.04045 ? channel / 12.92
                                  : Math.pow((channel + 0.055) / 1.055, 2.4)
    }

    function relativeLuminance(color) {
        return 0.2126 * linearChannel(color.r)
             + 0.7152 * linearChannel(color.g)
             + 0.0722 * linearChannel(color.b)
    }

    function contrastRatio(first, second) {
        const lighter = Math.max(relativeLuminance(first), relativeLuminance(second))
        const darker = Math.min(relativeLuminance(first), relativeLuminance(second))
        return (lighter + 0.05) / (darker + 0.05)
    }

    function init() {
        verify(quickSettingsThemeFixture.resetLustre())
        tryCompare(quickSettingsThemeFixture.themeGeneration, "profileId", "lustre")
        profileRequests = 0
        cancelRequests = 0
        quickSettings.layoutDirection = Qt.LeftToRight
        logoutConfirmation.layoutDirection = Qt.LeftToRight
    }

    function test_accessibleGenerationCausallyControlsBothRenderedSurfaces() {
        verify(quickSettingsThemeFixture.resetAccessible())
        tryCompare(quickSettingsThemeFixture.themeGeneration, "highContrast", true)
        const generation = quickSettingsThemeFixture.themeGeneration
        const tokens = generation.notification
        compare(generation.reducedMotion, true)
        compare(generation.transparencyDisabled, true)
        compare(tokens.highContrast, true)
        compare(tokens.reducedMotion, true)
        compare(tokens.fastMotionMs, 0)
        compare(tokens.normalMotionMs, 0)
        compare(tokens.fallbackActive, true)
        compare(tokens.borderWidth, 3)

        compare(quickSettings.themeGeneration, generation)
        compare(logoutConfirmation.themeGeneration, generation)
        compare(quickSettings.tokens, tokens)
        compare(logoutConfirmation.tokens, tokens)

        const quickBackground = findChild(quickSettings, "quickSettingsBackground")
        const quickHeading = findChild(quickSettings, "quickSettingsHeading")
        const logoutBackground = findChild(logoutConfirmation,
                                           "logoutConfirmationBackground")
        const logoutHeading = findChild(logoutConfirmation, "logoutHeading")
        verify(quickBackground !== null)
        verify(quickHeading !== null)
        verify(logoutBackground !== null)
        verify(logoutHeading !== null)
        compare(quickBackground.color, tokens.surfaceColor)
        compare(quickBackground.border.color, tokens.borderColor)
        compare(quickBackground.border.width, tokens.borderWidth)
        compare(logoutBackground.color, tokens.surfaceColor)
        compare(logoutBackground.border.color, tokens.borderColor)
        compare(logoutBackground.border.width, tokens.borderWidth)
        compare(quickBackground.color.a, 1)
        compare(logoutBackground.color.a, 1)
        verify(contrastRatio(quickHeading.color, quickBackground.color) >= 7)
        verify(contrastRatio(logoutHeading.color, logoutBackground.color) >= 7)
        compare(quickHeading.font.pixelSize, Math.round(tokens.titleFontPixels))
        compare(logoutHeading.font.pixelSize, Math.round(tokens.titleFontPixels))
        tryVerify(() => quickSettings.controlAt(0).height >= tokens.minimumTargetSize)
        tryVerify(() => findChild(logoutConfirmation, "logoutCancel").height
                       >= tokens.minimumTargetSize)

        // These surfaces deliberately have no presentation animations. The real
        // reduced-motion generation publishes zero durations, and keyboard actions
        // affect the rendered controls synchronously without a timer or transition.
        quickSettings.focusFirstControl()
        tryCompare(quickSettings.controlAt(0), "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(profileRequests, 1)
        logoutConfirmation.focusFirstControl()
        tryCompare(findChild(logoutConfirmation, "logoutCancel"), "activeFocus", true)
        keyClick(Qt.Key_Space)
        compare(cancelRequests, 1)
    }

    function test_missingBlurFallbackIsNotMisclassifiedAsAccessibility() {
        const baseline = quickSettingsThemeFixture.themeGeneration
        compare(baseline.highContrast, false)
        compare(baseline.reducedMotion, false)
        compare(baseline.notification.fallbackActive, false)
        verify(baseline.notification.fastMotionMs > 0)
        verify(findChild(quickSettings, "quickSettingsBackground").color.a < 1)

        verify(quickSettingsThemeFixture.resetLustreMissingBlur())
        tryCompare(quickSettingsThemeFixture.themeGeneration.notification,
                   "fallbackActive", true)
        const fallback = quickSettingsThemeFixture.themeGeneration
        compare(fallback.highContrast, false)
        compare(fallback.reducedMotion, false)
        compare(fallback.transparencyDisabled, false)
        verify(fallback.notification.fastMotionMs > 0)
        compare(fallback.notification.borderWidth, 1)
        compare(findChild(quickSettings, "quickSettingsBackground").color.a, 1)
        compare(findChild(logoutConfirmation, "logoutConfirmationBackground").color.a, 1)
        compare(findChild(quickSettings, "quickSettingsBackground").border.width, 1)

        const retained = quickSettingsThemeFixture.themeGeneration
        verify(quickSettingsThemeFixture.rejectInvalidCandidate())
        compare(quickSettingsThemeFixture.themeGeneration, retained)
        compare(quickSettings.themeGeneration, retained)
        compare(logoutConfirmation.themeGeneration, retained)
    }
}
// qmllint enable unqualified
