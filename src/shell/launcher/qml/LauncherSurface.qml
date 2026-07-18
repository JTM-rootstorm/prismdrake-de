pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root

    required property var themeGeneration
    required property var launcherModel
    property int layoutDirection: Qt.locale().textDirection
    property int pendingFocusIndex: -1
    property bool focusRecoveryReady: false

    readonly property var tokens: themeGeneration.launcher
    readonly property int resultCount: results.count
    readonly property string viewState: launcherModel.stateId
    readonly property bool opaqueFallbackActive: tokens.fallbackActive
    readonly property int motionDuration: tokens.reducedMotion ? 0 : tokens.fastMotionMs

    signal searchRequested(string query)
    signal dismissRequested
    signal focusExitForward
    signal focusExitBackward

    function resultAt(index) {
        return results.itemAtIndex(index)
    }

    function focusSearch() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    function submitSearch() {
        searchRequested(search.text)
    }

    function focusResult(index, backward) {
        if (index < 0) {
            search.forceActiveFocus(Qt.BacktabFocusReason)
            return
        }
        if (index >= results.count) {
            focusExitForward()
            return
        }
        results.currentIndex = index
        results.positionViewAtIndex(index, ListView.Contain)
        const candidate = results.itemAtIndex(index)
        if (candidate !== null)
            candidate.forceActiveFocus(backward ? Qt.BacktabFocusReason : Qt.TabFocusReason)
    }

    function captureFocusRecovery() {
        focusRecoveryReady = false
        pendingFocusIndex = -1
        for (let index = 0; index < results.count; ++index) {
            const candidate = results.itemAtIndex(index)
            if (candidate !== null && candidate.activeFocus) {
                pendingFocusIndex = index
                return
            }
        }
    }

    function clearFocusRecovery() {
        pendingFocusIndex = -1
    }

    function recoverFocus() {
        if (pendingFocusIndex < 0 || !focusRecoveryReady)
            return
        results.forceLayout()

        for (let index = 0; index < results.count; ++index) {
            const candidate = results.itemAtIndex(index)
            const resultButton = candidate as LauncherResultButton
            // A retained delegate keeps focus across a valid model move. A removed
            // delegate may briefly remain focused with a null presentation.
            if (resultButton !== null && resultButton.presentation !== null
                    && candidate.activeFocus) {
                clearFocusRecovery()
                return
            }
        }
        if (results.count > 0) {
            const index = Math.min(pendingFocusIndex, results.count - 1)
            results.positionViewAtIndex(index, ListView.Contain)
            results.forceLayout()
            const fallback = results.itemAtIndex(index)
            const fallbackButton = fallback as LauncherResultButton
            if (fallbackButton !== null && fallbackButton.presentation !== null) {
                clearFocusRecovery()
                fallback.forceActiveFocus(Qt.OtherFocusReason)
                return
            }
            // The positioned delegate may be incubating. Its completion retries recovery.
            return
        }
        clearFocusRecovery()
        search.forceActiveFocus(Qt.OtherFocusReason)
    }

    implicitWidth: 560
    implicitHeight: 620
    LayoutMirroring.enabled: layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Applications")
    Accessible.description: opaqueFallbackActive
                            ? qsTr("Application launcher. Opaque fallback active")
                            : qsTr("Application launcher")

    Connections {
        target: root.launcherModel
        function onPublicationReconciliationStarted() {
            root.captureFocusRecovery()
        }
        function onPublicationApplied() {
            // Reconcile ListView delegates after the model's synchronous move/remove signals.
            root.focusRecoveryReady = true
            Qt.callLater(root.recoverFocus)
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: root.tokens.tileRadius
        color: root.tokens.surfaceColor
        border.width: root.tokens.borderWidth
        border.color: root.tokens.borderColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.tokens.tilePadding
        spacing: root.tokens.tilePadding

        Label {
            objectName: "launcherHeading"
            Layout.fillWidth: true
            color: root.tokens.textPrimaryColor
            textFormat: Text.PlainText
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.titleFontPixels
            font.bold: true
            text: qsTr("Applications")
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        TextField {
            id: search
            objectName: "launcherSearchField"
            Layout.fillWidth: true
            Layout.minimumHeight: root.tokens.minimumTargetSize
            activeFocusOnTab: false
            maximumLength: 256
            selectByMouse: true
            placeholderText: qsTr("Search applications")
            color: root.tokens.textPrimaryColor
            placeholderTextColor: root.tokens.textMutedColor
            font.family: root.tokens.bodyFontFamily
            font.pixelSize: root.tokens.bodyFontPixels

            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Search applications")
            Accessible.description: qsTr("Enter up to 256 characters")
            Accessible.focusable: true
            Accessible.focused: activeFocus

            Keys.priority: Keys.BeforeItem
            Keys.onDownPressed: event => {
                event.accepted = true
                if (root.resultCount > 0)
                    root.focusResult(0, false)
                else
                    root.focusExitForward()
            }
            Keys.onTabPressed: event => {
                event.accepted = true
                if (root.resultCount > 0)
                    root.focusResult(0, false)
                else
                    root.focusExitForward()
            }
            Keys.onBacktabPressed: event => {
                event.accepted = true
                root.focusExitBackward()
            }
            Keys.onEscapePressed: event => {
                event.accepted = true
                root.dismissRequested()
            }
            onTextEdited: root.submitSearch()
            onAccepted: {
                root.submitSearch()
                if (root.resultCount > 0)
                    root.resultAt(0).presentation.requestLaunch()
            }

            background: Rectangle {
                radius: root.tokens.tileRadius
                color: root.tokens.controlColor
                border.width: search.activeFocus ? root.tokens.focusWidth
                                                 : root.tokens.tileBorderWidth
                border.color: search.activeFocus ? root.tokens.focusColor
                                                 : root.tokens.borderColor
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: results
                objectName: "launcherResults"
                anchors.fill: parent
                visible: root.viewState === "results"
                clip: true
                spacing: Math.max(2, root.tokens.tilePadding / 2)
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: false
                model: root.launcherModel

                delegate: LauncherResultButton {
                    required property int index
                    required property var result

                    width: ListView.view.width
                    presentation: result
                    tokens: root.tokens
                    onFocusPrevious: root.focusResult(index - 1, true)
                    onFocusNext: root.focusResult(index + 1, false)
                    onDismissRequested: root.dismissRequested()
                    Component.onCompleted: root.recoverFocus()
                    onPresentationChanged: Qt.callLater(root.recoverFocus)
                }

                ScrollBar.vertical: ScrollBar {}
            }

            Column {
                anchors.centerIn: parent
                width: parent.width
                spacing: root.tokens.tilePadding
                visible: root.viewState !== "results"

                BusyIndicator {
                    objectName: "launcherBusyIndicator"
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: root.viewState === "loading"
                    running: visible && !root.tokens.reducedMotion
                    Accessible.ignored: true
                }

                Text {
                    objectName: "launcherStateLabel"
                    width: parent.width
                    color: root.viewState === "error" ? root.tokens.dangerColor
                                                       : root.tokens.textMutedColor
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    font.family: root.tokens.bodyFontFamily
                    font.pixelSize: root.tokens.bodyFontPixels
                    font.bold: root.viewState === "error"
                    text: root.launcherModel.stateLabel
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
        }
    }
}
