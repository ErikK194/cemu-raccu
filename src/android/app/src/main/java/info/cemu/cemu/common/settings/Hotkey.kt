package info.cemu.cemu.common.settings

import kotlinx.serialization.Serializable

@Serializable
data class HotkeyCombo(
    val keys: Set<Int>
)

enum class HotkeyAction {
    QUIT,
    TOGGLE_MENU,
    SHOW_EMULATED_USB_DEVICES_DIALOG,
    TOGGLE_TEXTURE_PACK, // texture-pack fork: original <-> KTX2 texture pack, live
}