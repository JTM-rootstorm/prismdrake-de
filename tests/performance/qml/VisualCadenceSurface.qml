import QtQuick
import "../../../src/shell/panel/qml" as Panel

Window {
    id: root
    width: 1420
    height: 180
    visible: true
    color: "black"
    property real cadencePhase: 0

    Panel.PanelSurface {
        id: panel
        x: 10 + root.cadencePhase
        y: 10
        width: 1400
        height: implicitHeight
        themeGeneration: panelFixture.themeGeneration
        taskModel: panelFixture.taskModel
    }
}
