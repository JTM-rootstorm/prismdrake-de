import QtQuick
import QtTest
import "../../qml" as QuickSettings

TestCase {
    id: testCase
    name: "LogoutConfirmationSurface"
    when: windowShown
    width: 520
    height: 320

    QtObject {
        id: notificationTokens
        property color surfaceColor: "#252a35"
        property color borderColor: "#9ba7ba"
        property color textPrimaryColor: "#ffffff"
        property color textMutedColor: "#d8deea"
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

    QuickSettings.LogoutConfirmationSurface {
        id: surface
        x: 40
        y: 40
        width: implicitWidth
        height: implicitHeight
        themeGeneration: themeGeneration
        statusText: "Close the session and all applications?"
        transactionPending: false
    }

    SignalSpy { id: confirmSpy; target: surface; signalName: "confirmRequested" }
    SignalSpy { id: cancelSpy; target: surface; signalName: "cancelRequested" }

    function init() {
        surface.transactionPending = false
        confirmSpy.clear()
        cancelSpy.clear()
    }

    function test_accessibleModalFocusAndActions() {
        compare(surface.Accessible.role, Accessible.Dialog)
        verify(surface.Accessible.name.length > 0)
        verify(surface.Accessible.description.length > 0)

        surface.focusFirstControl()
        const cancel = findChild(surface, "logoutCancel")
        const confirm = findChild(surface, "logoutConfirm")
        verify(cancel !== null)
        verify(confirm !== null)
        compare(cancel.activeFocus, true)
        verify(cancel.height >= notificationTokens.minimumTargetSize)
        verify(confirm.height >= notificationTokens.minimumTargetSize)
        verify(cancel.width >= notificationTokens.minimumTargetSize)
        verify(confirm.width >= notificationTokens.minimumTargetSize)
        verify(cancel.Accessible.description.length > 0)
        verify(confirm.Accessible.description.length > 0)

        confirm.clicked()
        compare(confirmSpy.count, 1)
        keyClick(Qt.Key_Escape)
        compare(cancelSpy.count, 1)
    }

    function test_pendingTransactionCannotDispatchAgain() {
        surface.transactionPending = true
        const cancel = findChild(surface, "logoutCancel")
        const confirm = findChild(surface, "logoutConfirm")
        compare(cancel.enabled, false)
        compare(confirm.enabled, false)
        keyClick(Qt.Key_Escape)
        compare(cancelSpy.count, 0)
        compare(confirmSpy.count, 0)
    }

    function test_rtlFocusOrderAndLongStatusWrapping() {
        surface.LayoutMirroring.enabled = true
        surface.LayoutMirroring.childrenInherit = true
        surface.statusText = "A deliberately long translated logout explanation that must wrap "
                + "inside the bounded confirmation surface without hiding either action."
        wait(0)

        surface.focusFirstControl()
        const cancel = findChild(surface, "logoutCancel")
        const confirm = findChild(surface, "logoutConfirm")
        compare(cancel.activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(confirm.activeFocus, true)
        verify(surface.implicitHeight > notificationTokens.minimumTargetSize * 2)
        verify(surface.LayoutMirroring.enabled)
        verify(cancel.x !== confirm.x)

        surface.LayoutMirroring.enabled = false
        surface.LayoutMirroring.childrenInherit = false
        surface.statusText = "Close the session and all applications?"
    }
}
