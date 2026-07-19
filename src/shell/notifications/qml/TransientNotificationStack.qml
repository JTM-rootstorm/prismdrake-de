pragma ComponentBehavior: Bound

import QtQuick

FocusScope {
    id: root

    required property var presentation
    required property var themeGeneration
    property int layoutDirection: Qt.locale().textDirection
    readonly property int count: transientSurface.count

    signal focusExitForward
    signal focusExitBackward

    function focusFirstCard() {
        transientSurface.focusFirstCard()
    }

    implicitWidth: transientSurface.implicitWidth
    implicitHeight: transientSurface.implicitHeight
    activeFocusOnTab: false
    LayoutMirroring.enabled: layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    Accessible.role: Accessible.Notification
    Accessible.name: qsTr("Transient notifications")
    Accessible.description: qsTr("Newest notifications on the active output")

    NotificationSurface {
        id: transientSurface

        anchors.fill: parent
        presentationModel: root.presentation.transientModel
        themeGeneration: root.themeGeneration
        onFocusExitForward: root.focusExitForward()
        onFocusExitBackward: root.focusExitBackward()
    }
}
