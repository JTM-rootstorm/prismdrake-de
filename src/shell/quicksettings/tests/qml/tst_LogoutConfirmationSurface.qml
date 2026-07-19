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
        surface.layoutDirection = Qt.LeftToRight
        surface.statusText = "Close the session and all applications?"
        notificationTokens.minimumTargetSize = 48
        notificationTokens.bodyFontPixels = 14
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
        compare(confirm.Accessible.defaultButton, true)

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
        compare(cancel.Accessible.focusable, false)
        compare(confirm.Accessible.focusable, false)
        keyClick(Qt.Key_Escape)
        compare(cancelSpy.count, 0)
        compare(confirmSpy.count, 0)
    }

    function test_completeTabBacktabCycleAndKeyboardActivation() {
        surface.focusFirstControl()
        const cancel = findChild(surface, "logoutCancel")
        const confirm = findChild(surface, "logoutConfirm")
        tryCompare(cancel, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(confirm, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(cancel, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(confirm, "activeFocus", true)
        keyClick(Qt.Key_Backtab)
        tryCompare(cancel, "activeFocus", true)

        keyClick(Qt.Key_Space)
        compare(cancelSpy.count, 1)
        confirm.forceActiveFocus(Qt.OtherFocusReason)
        keyClick(Qt.Key_Return)
        compare(confirmSpy.count, 1)
        keyClick(Qt.Key_Enter)
        compare(confirmSpy.count, 2)
    }

    function test_localeDrivenRtlAndPseudoLocalizedScaledStatus() {
        const cancel = findChild(surface, "logoutCancel")
        const confirm = findChild(surface, "logoutConfirm")
        const status = findChild(surface, "logoutStatus")
        verify(cancel !== null)
        verify(confirm !== null)
        verify(status !== null)
        verify(cancel.mapToItem(surface, 0, 0).x
               < confirm.mapToItem(surface, 0, 0).x)

        surface.layoutDirection = Qt.RightToLeft
        let pseudo = "[!! "
        for (let index = 0; index < 80; ++index)
            pseudo += "Dragon session explanation "
        pseudo += "!!]"
        surface.statusText = pseudo
        notificationTokens.minimumTargetSize = 56
        notificationTokens.bodyFontPixels = 22
        wait(0)

        surface.focusFirstControl()
        compare(cancel.activeFocus, true)
        keyClick(Qt.Key_Tab)
        compare(confirm.activeFocus, true)
        keyClick(Qt.Key_Backtab)
        compare(cancel.activeFocus, true)
        verify(surface.implicitHeight > notificationTokens.minimumTargetSize * 2)
        verify(surface.y + surface.implicitHeight <= testCase.height)
        verify(surface.LayoutMirroring.enabled)
        tryVerify(() => cancel.mapToItem(surface, 0, 0).x
                         > confirm.mapToItem(surface, 0, 0).x)
        compare(status.text, pseudo)
        compare(status.wrapMode, Text.Wrap)
        compare(status.maximumLineCount, 4)
        compare(status.truncated, true)
        compare(status.font.pixelSize, 22)
        compare(surface.Accessible.description, pseudo)
        verify(cancel.width >= 56)
        verify(confirm.width >= 56)
    }
}
