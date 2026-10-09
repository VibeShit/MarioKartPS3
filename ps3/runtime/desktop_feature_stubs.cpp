// PS3 replacements for runtime features that only exist on desktop hosts.
//
// * settings_overlay: the F10 bar is an ImGui UI drawn through aurora's WebGPU
//   renderer. The PS3 build has no overlay; it only applies the persisted
//   Config.toml settings the overlay would otherwise apply at startup.
// * controller_mapping_wizard: part of the same ImGui UI.
// * DiscordPresence: talks to a local Discord client over a Unix socket.

#include "audio_backend.h"
#include "controller_mapping_wizard.h"
#include "discord_presence.h"
#include "game_graphics_options.h"
#include "input_bindings.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "settings_overlay.h"

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);

namespace settings_overlay {

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(RuntimeConfigFile::RumbleEnabled(true));
    InputBindings::Reload();
    AudioBackend::Instance().SetMasterVolume(RuntimeConfigFile::AudioVolume(1.0f));
    AudioBackend::Instance().SetMuted(RuntimeConfigFile::AudioMuted(false));
    MusicAttenuation::SetMusicVolume(RuntimeConfigFile::MusicVolume(1.0f));
    MusicAttenuation::SetSoundEffectsVolume(RuntimeConfigFile::SoundEffectsVolume(1.0f));
    MusicAttenuation::SetUiVolume(RuntimeConfigFile::UiVolume(1.0f));
    MusicAttenuation::SetVoicesVolume(RuntimeConfigFile::VoicesVolume(1.0f));
    MusicAttenuation::SetEnabled(false);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(
        RuntimeConfigFile::DisabledPostProcessingPaths(0));
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void HandleEvents(const AuroraEvent*) noexcept {}
void Draw() noexcept {}
bool StartupScreenVisible() noexcept { return false; }
void NotifyStrapInputAccepted() noexcept {}
void AdvancePresentedFrame() noexcept {}
void ReleaseControllers() noexcept {}

} // namespace settings_overlay

namespace controller_mapping_wizard {
void LoadPersistedMappings() {}
void HandleSdlEvent(const SDL_Event&) {}
void DrawSetupList() {}
void Draw() {}
bool IsActive() { return false; }
} // namespace controller_mapping_wizard

namespace DiscordPresence {
void Initialize(const std::string&, const std::string&) {}
void SetClient(const std::string&) {}
void SetActivity(Activity) {}
void Reset() {}
void Shutdown() {}
} // namespace DiscordPresence
