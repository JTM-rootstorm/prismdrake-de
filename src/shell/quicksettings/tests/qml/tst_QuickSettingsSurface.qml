import QtQuick
import QtTest
import "../../qml" as QuickSettings

TestCase {
    id: testCase
    name: "QuickSettingsSurface"
    when: windowShown
    width: 500
    height: 900

    property int profileRequests: 0
    property int dndRequests: 0
    property int centerRequests: 0
    property int logoutRequests: 0
    property bool makeProfileRequestPending: false
    property bool completeProfileRequestSynchronously: false
    property bool forgeFocusedAtRequest: false
    property bool acceptProfileRequest: true
    property bool rejectProfileAsUnavailable: false
    property bool makeDndRequestPending: false
    property bool completeDndRequestSynchronously: false
    property bool dndFocusedAtRequest: false
    property bool acceptDndRequest: true
    property bool rejectDndAsUnavailable: false

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
            if (!testCase.acceptProfileRequest) {
                if (testCase.rejectProfileAsUnavailable)
                    profileAdapter.interactive = false
                profileAdapter.changed()
                return false
            }
            if (testCase.makeProfileRequestPending) {
                profileAdapter.pending = true
                profileAdapter.changed()
            }
            return true
        }
        function requestForgeProfile() {
            ++testCase.profileRequests
            testCase.forgeFocusedAtRequest = surface.controlAt(1).activeFocus
            if (!testCase.acceptProfileRequest) {
                if (testCase.rejectProfileAsUnavailable)
                    profileAdapter.interactive = false
                profileAdapter.changed()
                return false
            }
            if (testCase.makeProfileRequestPending) {
                profileAdapter.pending = true
                profileAdapter.changed()
                if (testCase.completeProfileRequestSynchronously) {
                    profileAdapter.valueId = "forge"
                    profileAdapter.pending = false
                    profileAdapter.changed()
                }
            }
            return true
        }
        function requestDoNotDisturb(enabled) {
            ++testCase.dndRequests
            testCase.dndFocusedAtRequest = surface.controlAt(2).activeFocus
            if (!testCase.acceptDndRequest) {
                if (testCase.rejectDndAsUnavailable)
                    dndAdapter.interactive = false
                dndAdapter.changed()
                return false
            }
            if (testCase.makeDndRequestPending) {
                dndAdapter.pending = true
                dndAdapter.changed()
                if (testCase.completeDndRequestSynchronously) {
                    dndAdapter.checked = enabled
                    dndAdapter.pending = false
                    dndAdapter.changed()
                }
            }
            return true
        }
        function requestNotificationCenter() { ++testCase.centerRequests; return true }
        function requestLogout() { ++testCase.logoutRequests; return true }
    }

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
        property real cardRadius: 9
        property real borderWidth: 1
        property real minimumTargetSize: 48
        property string bodyFontFamily: "sans-serif"
        property real bodyFontPixels: 14
        property real titleFontPixels: 17
    }

    QtObject {
        id: themeGeneration
        property QtObject notification: notificationTokens
    }

    QuickSettings.QuickSettingsSurface {
        id: surface
        x: 20
        y: 20
        width: 420
        height: implicitHeight
        quickSettingsModel: model
        themeGeneration: themeGeneration
    }

    SignalSpy { id: closeSpy; target: surface; signalName: "closeRequested" }
    SignalSpy { id: forwardSpy; target: surface; signalName: "focusExitForward" }
    SignalSpy { id: backwardSpy; target: surface; signalName: "focusExitBackward" }

    function init() {
        profileRequests = 0
        dndRequests = 0
        centerRequests = 0
        logoutRequests = 0
        makeProfileRequestPending = false
        completeProfileRequestSynchronously = false
        forgeFocusedAtRequest = false
        acceptProfileRequest = true
        rejectProfileAsUnavailable = false
        makeDndRequestPending = false
        completeDndRequestSynchronously = false
        dndFocusedAtRequest = false
        acceptDndRequest = true
        rejectDndAsUnavailable = false
        surface.visible = true
        profileAdapter.interactive = true
        profileAdapter.pending = false
        profileAdapter.failed = false
        profileAdapter.valueId = "lustre"
        profileAdapter.description = "Choose the active Prismdrake profile."
        profileAdapter.stateText = "Available"
        dndAdapter.interactive = true
        dndAdapter.pending = false
        dndAdapter.failed = false
        dndAdapter.checked = false
        dndAdapter.description = "Normal notifications may be presented."
        dndAdapter.stateText = "Disabled"
        centerAdapter.interactive = true
        centerAdapter.pending = false
        centerAdapter.failed = false
        centerAdapter.description = "Open notification history and controls."
        centerAdapter.stateText = "Available"
        logoutAdapter.interactive = true
        logoutAdapter.pending = false
        logoutAdapter.failed = false
        logoutAdapter.description = "Request a confirmed session logout."
        logoutAdapter.stateText = "Available"
        notificationTokens.minimumTargetSize = 48
        notificationTokens.bodyFontPixels = 14
        surface.layoutDirection = Qt.LeftToRight
        closeSpy.clear()
        forwardSpy.clear()
        backwardSpy.clear()
    }

    function test_keyboardTraversalAndEscapeRecovery() {
        surface.focusFirstControl()
        compare(surface.controlAt(0).activeFocus, true)

        keyClick(Qt.Key_Tab)
        compare(surface.controlAt(1).activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(surface.controlAt(2).activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(surface.controlAt(3).activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(surface.controlAt(4).activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(forwardSpy.count, 1)

        surface.focusFirstControl()
        keyClick(Qt.Key_Backtab)
        compare(backwardSpy.count, 1)

        surface.focusControl(2, Qt.OtherFocusReason)
        keyClick(Qt.Key_Escape)
        compare(closeSpy.count, 1)

        surface.focusControl(3, Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        compare(centerRequests, 1)
        surface.focusControl(4, Qt.OtherFocusReason)
        keyClick(Qt.Key_Enter)
        compare(logoutRequests, 1)
    }

    function test_accessibleNamesRolesStatesAndTargetSizes() {
        compare(surface.Accessible.role, Accessible.Pane)
        compare(surface.Accessible.name, "Quick settings")
        for (let index = 0; index < surface.focusableControlCount; ++index) {
            const control = surface.controlAt(index)
            verify(control.Accessible.name.length > 0)
            verify(control.Accessible.description.length > 0)
            verify(control.height >= notificationTokens.minimumTargetSize)
        }
        compare(surface.controlAt(0).Accessible.role, Accessible.RadioButton)
        compare(surface.controlAt(2).Accessible.role, Accessible.CheckBox)
        compare(surface.controlAt(3).Accessible.role, Accessible.Button)
        compare(surface.controlAt(4).Accessible.role, Accessible.Button)
    }

    function test_oneAdapterFailureDoesNotDisableOthers() {
        dndAdapter.failed = true
        dndAdapter.interactive = false
        dndAdapter.description = "The request was not authorized."
        compare(surface.controlAt(2).enabled, false)
        compare(surface.controlAt(0).enabled, true)
        compare(surface.controlAt(1).enabled, true)
        compare(surface.controlAt(3).enabled, true)
        compare(surface.controlAt(4).enabled, true)

        surface.controlAt(3).clicked()
        compare(centerRequests, 1)
        surface.controlAt(4).clicked()
        compare(logoutRequests, 1)
    }

    function test_pendingAndUnavailableAdaptersRenderDisabled() {
        profileAdapter.pending = true
        centerAdapter.interactive = false
        compare(surface.controlAt(0).enabled, false)
        compare(surface.controlAt(1).enabled, false)
        compare(surface.controlAt(3).enabled, false)
        compare(surface.controlAt(4).enabled, true)
    }

    function test_traversalSkipsDisabledControlsAndCannotTrapFocus() {
        profileAdapter.interactive = false
        dndAdapter.interactive = false
        centerAdapter.interactive = true
        logoutAdapter.interactive = true
        surface.focusFirstControl()
        compare(surface.controlAt(3).activeFocus, true)
        keyClick(Qt.Key_Backtab)
        compare(backwardSpy.count, 1)

        centerAdapter.interactive = false
        logoutAdapter.interactive = false
        forwardSpy.clear()
        surface.focusFirstControl()
        compare(forwardSpy.count, 1)
    }

    function test_programmaticDndPublicationDoesNotRequestMutation() {
        compare(dndRequests, 0)
        dndAdapter.checked = true
        wait(0)
        compare(dndRequests, 0)
        dndAdapter.checked = false
        wait(0)
        compare(dndRequests, 0)
    }

    function test_pendingProfileActivationRestoresFocusAfterPublication() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        compare(surface.controlAt(1).activeFocus, true)
        compare(surface.controlAt(1).visualFocus, true)
        compare(surface.activeFocus, true)
        compare(surface.Window.window.visible, true)
        compare(surface.Window.window.active, true)

        keyClick(Qt.Key_Space)
        compare(profileRequests, 1)
        compare(profileAdapter.pending, true)
        compare(surface.controlAt(1).activeFocus, false)
        compare(surface.pendingProfileFocusRecovery, 1)
        compare(surface.pendingProfileVisualFocusRecovery, true)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        compare(surface.controlAt(1).enabled, true)
        compare(surface.controlAt(1).activeFocus, false)
        compare(surface.controlAt(1).Accessible.focused, false)
        compare(surface.pendingFocusIndicatorVisible, true)
        tryCompare(surface.controlAt(1), "activeFocus", true)
        tryCompare(surface.controlAt(1), "visualFocus", true)
        compare(surface.controlAt(1).Accessible.focused, true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_pendingProfileActivationSurvivesSynchronousCompletion() {
        makeProfileRequestPending = true
        completeProfileRequestSynchronously = true
        surface.focusControl(1, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(profileRequests, 1)
        compare(profileAdapter.pending, false)
        compare(profileAdapter.valueId, "forge")
        tryCompare(surface.controlAt(1), "activeFocus", true)
        tryCompare(surface.controlAt(1), "visualFocus", true)
        compare(surface.controlAt(1).Accessible.focused, true)
    }

    function test_pendingProfileActivationAnchorsAutomaticFallback() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(profileAdapter.pending, true)
        compare(forgeFocusedAtRequest, false)
        compare(surface.activeFocus, true)
        compare(surface.controlAt(1).Accessible.focused, true)
        compare(surface.pendingFocusIndicatorVisible, true)
        verify(surface.Window.window.activeFocusItem !== surface.controlAt(0))
        verify(surface.Window.window.activeFocusItem !== surface.controlAt(1))

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        tryCompare(surface.controlAt(1), "activeFocus", true)
        tryCompare(surface.controlAt(1), "visualFocus", true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_rejectedProfileActivationRestoresOriginImmediately() {
        acceptProfileRequest = false
        surface.focusControl(1, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(profileRequests, 1)
        compare(forgeFocusedAtRequest, false)
        compare(surface.pendingProfileFocusRecovery, -1)
        compare(surface.controlAt(1).activeFocus, true)
        compare(surface.controlAt(1).visualFocus, true)
        wait(0)
        compare(surface.controlAt(1).activeFocus, true)
    }

    function test_unavailableRejectedProfileUsesNextEnabledControl() {
        acceptProfileRequest = false
        rejectProfileAsUnavailable = true
        surface.focusControl(1, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(surface.pendingProfileFocusRecovery, -1)
        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.controlAt(2).visualFocus, true)
        wait(0)
        compare(surface.controlAt(2).activeFocus, true)
    }

    function test_nonPendingProfileActivationRestoresOriginImmediately() {
        surface.focusControl(1, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(profileRequests, 1)
        compare(forgeFocusedAtRequest, false)
        compare(surface.pendingProfileFocusRecovery, -1)
        compare(surface.controlAt(1).activeFocus, true)
        compare(surface.controlAt(1).visualFocus, true)
    }

    function test_pendingProfileAnchorNavigationCancelsRecovery() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        keyClick(Qt.Key_Tab)

        compare(surface.pendingProfileFocusRecovery, -1)
        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.controlAt(2).visualFocus, true)
    }

    function test_completionWithDisabledProfileUsesNextEnabledControl() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        profileAdapter.interactive = false
        profileAdapter.pending = false
        profileAdapter.changed()

        tryCompare(surface.controlAt(2), "activeFocus", true)
        tryCompare(surface.controlAt(2), "visualFocus", true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_navigationBeforeDeferredRestoreUsesOriginalProfileIndex() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        keyClick(Qt.Key_Tab)

        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_horizontalNavigationBeforeDeferredRestoreUsesProfileOrigin() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        keyClick(Qt.Key_Left)

        compare(surface.controlAt(0).activeFocus, true)
        compare(surface.pendingFocusIndicatorVisible, false)

        surface.layoutDirection = Qt.RightToLeft
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        profileAdapter.pending = false
        profileAdapter.changed()
        keyClick(Qt.Key_Left)

        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_pendingProfileActivationDoesNotStealDirectSiblingFocus() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        profileAdapter.pending = false
        profileAdapter.valueId = "forge"
        profileAdapter.changed()

        // Direct focus movement is user intent; only the private pending-focus
        // anchor authorizes automatic recovery.
        surface.controlAt(0).forceActiveFocus(Qt.OtherFocusReason)
        compare(surface.controlAt(0).activeFocus, true)

        wait(0)
        compare(surface.controlAt(0).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_pendingFocusRecoveryDoesNotStealIntentionalFocus() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        surface.focusControl(3, Qt.TabFocusReason)
        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_deferredProfileRecoveryHonorsIntentionalSiblingNavigation() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        profileAdapter.pending = false
        profileAdapter.valueId = "forge"
        profileAdapter.changed()
        surface.focusControl(0, Qt.TabFocusReason)
        wait(0)

        compare(surface.controlAt(0).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_deferredFocusRecoveryDoesNotStealIntentionalFocus() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        surface.focusControl(3, Qt.TabFocusReason)
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_directFocusChangePreventsDeferredRecovery() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        surface.controlAt(3).forceActiveFocus(Qt.TabFocusReason)
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_newDomainActivationCancelsOlderRecovery() {
        makeProfileRequestPending = true
        makeDndRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)
        compare(surface.pendingProfileFocusRecovery, 1)

        surface.controlAt(2).forceActiveFocus(Qt.TabFocusReason)
        surface.requestDoNotDisturb(true)
        compare(dndAdapter.pending, true)
        compare(surface.pendingProfileFocusRecovery, -1)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        wait(0)
        compare(surface.controlAt(1).activeFocus, false)

        dndAdapter.checked = true
        dndAdapter.pending = false
        dndAdapter.changed()
        tryCompare(surface.controlAt(2), "activeFocus", true)
        tryCompare(surface.controlAt(2), "visualFocus", true)
    }

    function test_pendingFocusRecoveryDoesNotRefocusAfterDismissal() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        keyClick(Qt.Key_Escape)
        compare(closeSpy.count, 1)
        compare(surface.pendingProfileFocusRecovery, -1)
        surface.focusControl(3, Qt.TabFocusReason)
        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_windowHideDropsDeferredRecovery() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileAdapter.pending, true)

        profileAdapter.valueId = "forge"
        profileAdapter.pending = false
        profileAdapter.changed()
        surface.Window.window.visible = false
        wait(0)
        compare(surface.pendingFocusAnchorOrigin, -1)
        compare(surface.pendingFocusIndicatorVisible, false)

        surface.Window.window.visible = true
        surface.Window.window.requestActivate()
        surface.focusControl(3, Qt.TabFocusReason)
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(1).activeFocus, false)
    }

    function test_pendingDndActivationRestoresFocusAfterPublication() {
        makeDndRequestPending = true
        surface.focusControl(2, Qt.TabFocusReason)
        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.controlAt(2).visualFocus, true)

        keyClick(Qt.Key_Space)
        compare(dndRequests, 1)
        compare(dndAdapter.pending, true)
        compare(surface.controlAt(2).activeFocus, false)
        compare(surface.pendingDoNotDisturbFocusRecovery, true)
        compare(surface.pendingDoNotDisturbVisualFocusRecovery, true)

        dndAdapter.checked = true
        dndAdapter.pending = false
        dndAdapter.changed()
        compare(surface.controlAt(2).enabled, true)
        compare(surface.controlAt(2).activeFocus, false)
        compare(surface.controlAt(2).Accessible.focused, false)
        compare(surface.pendingFocusIndicatorVisible, true)
        tryCompare(surface.controlAt(2), "activeFocus", true)
        tryCompare(surface.controlAt(2), "visualFocus", true)
        compare(surface.controlAt(2).Accessible.focused, true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_pendingDndActivationSurvivesSynchronousCompletion() {
        makeDndRequestPending = true
        completeDndRequestSynchronously = true
        surface.focusControl(2, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(dndRequests, 1)
        compare(dndFocusedAtRequest, false)
        compare(dndAdapter.pending, false)
        compare(dndAdapter.checked, true)
        tryCompare(surface.controlAt(2), "activeFocus", true)
        tryCompare(surface.controlAt(2), "visualFocus", true)
    }

    function test_rejectedDndActivationRestoresOriginImmediately() {
        acceptDndRequest = false
        surface.focusControl(2, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(dndRequests, 1)
        compare(dndFocusedAtRequest, false)
        compare(surface.pendingDoNotDisturbFocusRecovery, false)
        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.controlAt(2).visualFocus, true)
        wait(0)
        compare(surface.controlAt(2).activeFocus, true)
    }

    function test_unavailableRejectedDndUsesNextEnabledControl() {
        acceptDndRequest = false
        rejectDndAsUnavailable = true
        surface.focusControl(2, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(surface.pendingDoNotDisturbFocusRecovery, false)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(3).visualFocus, true)
        wait(0)
        compare(surface.controlAt(3).activeFocus, true)
    }

    function test_nonPendingDndActivationRestoresOriginImmediately() {
        surface.focusControl(2, Qt.TabFocusReason)

        keyClick(Qt.Key_Space)

        compare(dndRequests, 1)
        compare(dndFocusedAtRequest, false)
        compare(surface.pendingDoNotDisturbFocusRecovery, false)
        compare(surface.controlAt(2).activeFocus, true)
        compare(surface.controlAt(2).visualFocus, true)
    }

    function test_pendingDndAnchorNavigationCancelsRecovery() {
        makeDndRequestPending = true
        surface.focusControl(2, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        keyClick(Qt.Key_Tab)

        compare(surface.pendingDoNotDisturbFocusRecovery, false)
        compare(surface.controlAt(3).activeFocus, true)
        compare(surface.controlAt(3).visualFocus, true)
    }

    function test_completionWithDisabledDndUsesNextEnabledControl() {
        makeDndRequestPending = true
        surface.focusControl(2, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        dndAdapter.interactive = false
        dndAdapter.pending = false
        dndAdapter.changed()

        tryCompare(surface.controlAt(3), "activeFocus", true)
        tryCompare(surface.controlAt(3), "visualFocus", true)
        compare(surface.pendingFocusIndicatorVisible, false)
    }

    function test_pendingDndAnchorPreservesHorizontalKeySemantics() {
        makeDndRequestPending = true
        surface.focusControl(2, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)

        keyClick(Qt.Key_Left)
        compare(surface.pendingDoNotDisturbFocusRecovery, true)
        compare(surface.pendingFocusIndicatorVisible, true)

        dndAdapter.checked = true
        dndAdapter.pending = false
        dndAdapter.changed()
        tryCompare(surface.controlAt(2), "activeFocus", true)
    }

    function test_pendingLogicalFocusRejectsDuplicateAccessibleActions() {
        makeProfileRequestPending = true
        surface.focusControl(1, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(profileRequests, 1)
        compare(surface.controlAt(1).Accessible.focused, true)

        surface.controlAt(1).Accessible.pressAction()
        compare(profileRequests, 1)

        makeDndRequestPending = true
        surface.focusControl(2, Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        compare(dndRequests, 1)
        compare(surface.controlAt(2).Accessible.focused, true)

        surface.controlAt(2).Accessible.toggleAction()
        compare(dndRequests, 1)
    }

    function test_arrowTraversalAndRightToLeftProfileOrder() {
        surface.focusFirstControl()
        keyClick(Qt.Key_Down)
        compare(surface.controlAt(1).activeFocus, true)
        keyClick(Qt.Key_Up)
        compare(surface.controlAt(0).activeFocus, true)
        keyClick(Qt.Key_Up)
        compare(backwardSpy.count, 1)

        surface.layoutDirection = Qt.RightToLeft
        compare(surface.LayoutMirroring.enabled, true)
        surface.focusFirstControl()
        keyClick(Qt.Key_Left)
        compare(surface.controlAt(1).activeFocus, true)
        keyClick(Qt.Key_Right)
        compare(surface.controlAt(0).activeFocus, true)
    }

    function test_pseudoLocalizedLongDescriptionAndScaledTargets() {
        let pseudo = "[!! "
        for (let index = 0; index < 80; ++index)
            pseudo += "Dragon "
        pseudo += "!!]"
        profileAdapter.description = pseudo
        logoutAdapter.description = pseudo
        logoutAdapter.stateText = "[!! Available !!]"
        notificationTokens.minimumTargetSize = 56
        notificationTokens.bodyFontPixels = 22
        wait(0)

        const logoutControl = surface.controlAt(4)
        verify(logoutControl.implicitHeight >= 56)
        verify(logoutControl.Accessible.description.indexOf(pseudo) >= 0)
        verify(logoutControl.Accessible.description.indexOf("[!! Available !!]") >= 0)
        const profileDescription = findChild(surface, "quickSettingsProfileDescription")
        compare(profileDescription.text, pseudo)
        compare(profileDescription.wrapMode, Text.Wrap)
        compare(profileDescription.font.pixelSize, 22)
        verify(surface.Accessible.description.indexOf("Opaque fallback") >= 0)
    }

}
