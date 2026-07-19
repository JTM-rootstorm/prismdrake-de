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

    QtObject {
        id: profileAdapter
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

        function requestLustreProfile() { ++testCase.profileRequests; return true }
        function requestForgeProfile() { ++testCase.profileRequests; return true }
        function requestDoNotDisturb(enabled) { ++testCase.dndRequests; return enabled }
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
