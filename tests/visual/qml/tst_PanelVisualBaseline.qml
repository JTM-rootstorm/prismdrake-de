import QtQuick
import QtTest
import "../../../src/shell/panel/qml" as Panel

// The Quick Test setup publishes test-only real settings/theme/task fixtures and the recorder.
// qmllint disable unqualified

TestCase {
    id: testCase
    name: "PanelVisualBaseline"
    when: windowShown
    width: 1420
    height: 180
    visible: true
    property bool rightToLeft: false

    Panel.PanelSurface {
        id: panel
        x: 10
        y: 10
        width: 1400
        height: implicitHeight
        themeGeneration: panelFixture.themeGeneration
        taskModel: panelFixture.taskModel
        LayoutMirroring.enabled: testCase.rightToLeft
        LayoutMirroring.childrenInherit: true
    }

    function launcher() {
        return findChild(panel, "panelLauncherButton")
    }

    function diagnostics() {
        return findChild(panel, "panelDiagnosticsButton")
    }

    function capture(name, layoutDirection, blurAvailable) {
        let completed = false
        let recorded = false
        const accepted = panel.grabToImage(result => {
            const saved = result.saveToFile(baselineRecorder.imagePath(name))
            recorded = saved && baselineRecorder.record(
                name, panel.themeGeneration, panel.width, panel.height,
                panel.Screen.devicePixelRatio, layoutDirection, blurAvailable, false)
            completed = true
        })
        verify(accepted)
        tryVerify(() => completed, 5000)
        verify(recorded)
        verify(baselineRecorder.metadataComplete(name))
    }

    function initTestCase() {
        failOnWarning(/.*/)
        verify(baselineRecorder.expectedFontAvailable())
    }

    function init() {
        rightToLeft = false
        verify(panelFixture.resetLustre())
        verify(panelFixture.publishRepresentativeTasks())
        tryCompare(panel, "taskCount", 3)
    }

    function test_captureSharedProfileAndAccessibilityStates() {
        panel.focusLauncher()
        tryCompare(launcher(), "activeFocus", true)
        capture("panel-lustre", "ltr", true)
        capture("panel-lustre-repeat", "ltr", true)
        verify(baselineRecorder.imagesEqual("panel-lustre", "panel-lustre-repeat"))

        const sharedTree = panel
        verify(panelFixture.publishForge())
        tryCompare(panel.themeGeneration, "profileId", "forge")
        compare(panel, sharedTree)
        panel.taskAt(2).forceActiveFocus(Qt.OtherFocusReason)
        tryCompare(panel.taskAt(2), "activeFocus", true)
        capture("panel-forge", "ltr", true)

        verify(panelFixture.resetAccessible())
        verify(panelFixture.publishRepresentativeTasks())
        tryCompare(panel.themeGeneration, "highContrast", true)
        compare(panel.themeGeneration.reducedMotion, true)
        compare(panel.themeGeneration.transparencyDisabled, true)
        compare(panel.themeGeneration.textScale, 1.5)
        compare(panel.themeGeneration.panel.fallbackActive, true)
        panel.focusLauncher()
        capture("panel-accessible-fallback", "ltr", true)

        verify(panelFixture.resetLustreMissingBlur())
        verify(panelFixture.publishRepresentativeTasks())
        tryCompare(panel.themeGeneration.panel, "fallbackActive", true)
        capture("panel-lustre-missing-blur", "ltr", false)

        verify(panelFixture.publishForge())
        tryCompare(panel.themeGeneration, "profileId", "forge")
        rightToLeft = true
        tryVerify(() => launcher().x > panel.width / 2)
        verify(diagnostics().x < launcher().x)
        capture("panel-forge-rtl", "rtl", false)
    }
}

// qmllint enable unqualified
