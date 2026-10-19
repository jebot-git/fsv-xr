// SPDX-License-Identifier: LGPL-2.1-or-later
pragma Singleton
import QtQuick
QtObject {
    id: theme
    property int paletteRevision: 0
    function advancePalette() { paletteRevision++ }
    // Controls copy a Palette on assignment. Reassign after its role bindings
    // have settled, rather than leaving them with the startup colours.
    function controlPalette(_revision) { return palette }
    property Connections paletteUpdates: Connections {
        target: theme.colors
        function onPaletteChanged() { Qt.callLater(theme.advancePalette) }
    }
    property SystemPalette colors: SystemPalette { colorGroup: SystemPalette.Active }
    property SystemPalette disabledColors: SystemPalette { colorGroup: SystemPalette.Disabled }
    property Palette palette: Palette {
        window: colors.window
        windowText: colors.windowText
        base: colors.base
        text: colors.text
        button: colors.button
        buttonText: colors.buttonText
        highlight: colors.highlight
        highlightedText: colors.highlightedText
        placeholderText: colors.placeholderText
        mid: colors.mid
        accent: colors.accent
        disabled.windowText: disabledColors.windowText
        disabled.text: disabledColors.text
        disabled.buttonText: disabledColors.buttonText
    }
    readonly property color surface: typeof appearance !== "undefined" ? appearance.panelColor : colors.window
    readonly property color sceneBackground: typeof appearance !== "undefined" ? appearance.sceneBackground : colors.base
    readonly property color text: colors.windowText
    readonly property color muted: colors.placeholderText
    readonly property color accent: colors.accent
}
