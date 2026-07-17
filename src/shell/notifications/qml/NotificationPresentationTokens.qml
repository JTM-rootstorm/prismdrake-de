import QtQuick

QtObject {
    required property string profileId
    required property string profileDisplayName
    required property color surfaceColor
    required property color borderColor
    required property color textPrimaryColor
    required property color textMutedColor
    required property color focusColor
    required property color criticalColor
    required property color controlColor
    required property color pressedControlColor
    required property real cardRadius
    required property real cardPadding
    required property real borderWidth
    required property real focusWidth
    required property real minimumTargetSize
    required property int bodyFontPixels
    required property int titleFontPixels
    required property int motionDurationMs
    required property bool reducedMotion
    required property bool opaqueFallbackActive
    required property bool highContrast
}
