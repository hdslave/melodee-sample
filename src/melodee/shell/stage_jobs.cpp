// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Stage's jobs on the MIDI thread (pages/stage_page.cpp draws it): PRESET's
// sound (and a plug-in's PRESETS) loading once the turning stops, and an empty track becoming a synth,
// a kit or a MIDI track.
#include "melodee/shell/context.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/stage_map.h"
#include <algorithm>

namespace melodee {

void Context::preset_turn(int at) {
    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(track(), ui, list);
    if (!count || !track().used()) return;
    if (frozen_edit(UndoScope {UndoScope::Kind::Sound, uint8_t(ui.track % track_count), 0})) return; // freeze plan F2c
    ui.preset_cursor = uint16_t(std::clamp(at, 0, int(count) - 1));
    ui.preset_due = std::max<uint32_t>(now + stage_preset_ms, 1);
}

// The plug-in's presets as PRESETS lists them; the wheel and Minus / Plus walk from the one under the cursor (still
// turning) or the one it plays (none yet: to the first, either way), and the one turned to loads once the turning
// has stopped stage_preset_ms. Against an end of the list nothing moves, and nothing while the plug-in opens, or
// while nothing is listed yet as it looks for its presets (said). Nothing either for one that would not open, or that
// the page shows stopped (a bridge that died). False: an open plug-in that has no presets (its face).
bool Context::plugin_preset_turn(int delta) {
    const auto& t = track();
    if (!plugins || t.engine != EngineKind::Plugin || t.plugin >= max_plugins) return false;
    const UndoScope sound {UndoScope::Kind::Sound, uint8_t(ui.track % track_count), 0};
    const auto status = plugins->status(t.plugin);
    if (status == PluginStatus::None) { // let go by a deep freeze: thawed first, as any change of its sound (F2c)
        frozen_edit(sound);
        return true;
    }
    if (status != PluginStatus::Ready) return true; // opening, or would not open: no preset to walk to, nor the octave
    if (ui.plugin_panel.entry == t.plugin && ui.plugin_panel.status == PluginStatus::Crashed) return true; // STOPPED
    auto programs = plugins->programs(t.plugin);
    if (programs.list.empty()) {
        if (!programs.finding) return false;
        notice("Looking for presets");
        return true;
    }
    int at = -1;
    if (ui.preset_due && ui.presets_entry == t.plugin) at = ui.presets_cursor; // still turning
    else for (size_t n = 0; n < programs.list.size(); ++n) if (programs.list[n].index == programs.current) at = int(n);
    const int to = std::clamp(at + delta, 0, int(programs.list.size()) - 1);
    if (to == at) return true;
    if (frozen_edit(sound)) return true;
    ui.presets_entry = t.plugin;
    ui.presets_cursor = uint16_t(to);
    ui.preset_due = std::max<uint32_t>(now + stage_preset_ms, 1);
    return true;
}

void Context::stage_tick(uint32_t at) {
    if (!ui.preset_due || at < ui.preset_due) return;
    ui.preset_due = 0;
    if (track().engine == EngineKind::Plugin) { // PRESETS: the plug-in's own
        if (!plugins || track().plugin != ui.presets_entry) return;
        const auto programs = plugins->programs(ui.presets_entry);
        const size_t to = ui.presets_cursor;
        // Turned away and back to the one it plays: it stays as it was edited, as a synth's sound below.
        if (to < programs.list.size() && programs.list[to].index == programs.current) return;
        choose_plugin_program(int(to));
        return;
    }
    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(track(), ui, list);
    if (!count || !track().used()) return;
    unsigned sound = list[std::min<unsigned>(ui.preset_cursor, count - 1)];
    if (sound != track().preset) load_sound(ui.track, track().engine, sound);
}

void Context::stage_new() {
    auto& t = track();
    if (t.used()) return;
    const auto& page = stage_page_now(ui, t, false); // an empty track: no pad
    switch (page.kind) {
    case StageKind::NewSynth: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = new_engines(engines);
        if (!count) return;
        auto kind = engines[ui.new_engine % count];
        // A plug-in from the card first: the track turns PLUGIN only when one is picked (choose_plugin), so
        // CANCEL, or a card with no plug-ins, leaves it empty.
        if (kind == EngineKind::Plugin) { open_plugin_picker(UiState::PluginTarget::Instrument); return; }
        load_sound(ui.track, kind, std::min<unsigned>(ui.new_sound, std::max(engine_info(kind).sound_count, 1u) - 1));
        stage_go(ui, 0, 0); // PRESET
        break;
    }
    case StageKind::NewKit:
        load_sound(ui.track, EngineKind::Sampler, 0);
        t.name_from("KIT");
        stage_go(ui, 0, 0); // TRIM
        switch (ui.new_source % new_sources) {
        case 0: open(PageId::Samples); if (library) library->rescan(); break; // Sample Select
        default: open(PageId::Record); break;                                 // Sample Record
        }
        break;
    case StageKind::NewMidi:
        load_sound(ui.track, EngineKind::Midi, 0);
        set_param(ui.track, midi_track::Port, ui.new_port % midi_track::PortCount);
        set_param(ui.track, midi_track::Channel, std::clamp<int>(ui.new_channel, 1, 16));
        stage_go(ui, 0, 0); // DEVICE
        break;
    default: return;
    }
    dirty = true;
}

} // namespace melodee
