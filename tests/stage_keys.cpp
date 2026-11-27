// SPDX-License-Identifier: AGPL-3.0-only
// Stage's Minus / Plus on a PLUGIN track, pressed on a Shell with the rack and
// the fixture plug-in (tests/clap_fixture.cpp), as tests/test_plugin_hosts.py
// runs it: build/test-stage-keys CARD PLUGIN MODE.
//
// presets: three presets beside the plug-in. On PRESETS, Minus / Plus walk
//   them as the wheel does, stop at the list's ends, and leave the pads' keys
//   (the octave, the Drums layout's bank) alone. Nothing moves while the
//   plug-in opens; while nothing is listed yet a notice says so; a frozen
//   track asks first. Sample held, they are Keys' octave, and Sample's release
//   then leaves the page as it is. The fixture holds its open and its preset
//   listing while the files MELODEE_TEST_HOLD_OPEN / _PRESETS name are there.
// face: a plug-in with no presets. Minus / Plus are the octave, as before.
// broken: a plug-in that would not open. Nothing moves; Keys' octave still answers.
#include "melodee/engine/command.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/engine_info.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/features.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/stage_map.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " + #x); } while (false)

static int run(const std::string& card, const std::string& plugin, const std::string& mode) {
    const bool presets = mode == "presets", face = mode == "face", broken = mode == "broken";
    CHECK(presets || face || broken);
    const fs::path hold_open = fs::path(card).parent_path() / "hold-open";
    const fs::path hold_presets = fs::path(card).parent_path() / "hold-presets";
    auto hold = [](const fs::path& file) { std::ofstream out(file); };
    if (presets) {
        const auto folder = fs::path(plugin).parent_path() / "TestPresets";
        fs::create_directories(folder);
        std::ofstream(folder / "Bank.tsp") << "Low\t0.25\nMid\t0.5\nHigh\t0.75\n";
        hold(hold_open); // the fixture opens, then says where its presets are, once each file is gone
        hold(hold_presets);
        setenv("MELODEE_TEST_HOLD_OPEN", hold_open.c_str(), 1);
        setenv("MELODEE_TEST_HOLD_PRESETS", hold_presets.c_str(), 1);
    }
    const unsigned entry = 0;
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host);
    auto shell = std::make_unique<Shell>(*host);
    struct Unhold { // gone before the shell and the rack, even when a CHECK throws: the rack's thread is let go
        fs::path a, b;
        ~Unhold() { std::error_code e; fs::remove(a, e); fs::remove(b, e); }
    } unhold {hold_open, hold_presets};
    auto& c = shell->context();
    auto& ui = shell->ui();
    auto& project = shell->project();
    auto& tr = project.tracks[0];
    auto& ref = project.plugins[entry];
    rack->set_card(card);
    shell->set_plugins(rack.get());
    std::snprintf(ref.path, sizeof(ref.path), "%s", (broken ? plugin + ".gone" : plugin).c_str()); // not on the card
    load_sound(tr, EngineKind::Plugin, 0);
    tr.plugin = uint8_t(entry);
    host->load(project, shell->patterns());
    host->set_audio_format(48000, 384);
    Command reload {Command::Type::Reload};
    reload.pack = new ProjectPack(project, shell->patterns());
    host->send(reload);
    ui.track = 0;

    uint32_t now = 1000;
    std::array<int32_t, EngineHost::block * 2> audio {};
    auto tick = [&] {
        c.plugins_tick(now += 5);
        host->render(audio.data(), EngineHost::block, 0.7f);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto wait = [&](auto predicate) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!predicate() && std::chrono::steady_clock::now() < until) tick();
        CHECK(predicate());
    };
    auto button = [&](SurfaceButton b, bool down) {
        SurfaceEvent event;
        event.input.kind = SurfaceInput::Kind::Button;
        event.input.index = uint8_t(b);
        event.input.pressed = down;
        shell->surface(event, ++now);
    };
    auto tap = [&](SurfaceButton b) { button(b, true); button(b, false); };
    auto wheel = [&](int detents) {
        SurfaceEvent event;
        event.input.kind = SurfaceInput::Kind::Control;
        event.input.index = uint8_t(SurfaceControl::Wheel);
        event.input.delta = detents;
        shell->surface(event, ++now);
    };

    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
    c.open(PageId::Stage);
    const int8_t octave = tr.octave;
    const uint8_t bank = tr.bank;
    auto keys_kept = [&] { return tr.octave == octave && tr.bank == bank && !ui.preset_due; };

    if (broken) {
        wait([&] { return rack->status(entry) == PluginStatus::Failed; });
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus, SurfaceButton::Minus}) {
            tap(key);
            CHECK(keys_kept());
        }
        c.prints.freeze[0].state = FreezeState::Frozen; // nothing would load: no question either
        tap(SurfaceButton::Plus);
        CHECK(keys_kept() && ui.ask == UiState::Ask::None);
        c.prints.freeze[0] = {};
        button(SurfaceButton::Sample, true);
        tap(SurfaceButton::Plus);
        button(SurfaceButton::Sample, false);
        CHECK(tr.octave == octave + 1 && !ui.preset_due);
        shell->set_plugins(nullptr);
        return 0;
    }

    auto opening = [&] { return rack->status(entry) == PluginStatus::Loading; };
    auto looking = [&] { return rack->status(entry) == PluginStatus::Ready && rack->programs(entry).finding; };
    auto whole = [&] { return rack->status(entry) == PluginStatus::Ready && !rack->programs(entry).finding; };
    if (presets) {
        wait(opening); // nothing to walk yet, nor the octave's
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus}) {
            tap(key);
            CHECK(keys_kept());
        }
        c.prints.freeze[0].state = FreezeState::Frozen; // nothing would load: no question either
        tap(SurfaceButton::Plus);
        CHECK(ui.ask == UiState::Ask::None && keys_kept());
        c.prints.freeze[0] = {};
        fs::remove(hold_open);
        wait(looking); // open, nothing listed yet: said, and nothing moves
        CHECK(rack->programs(entry).list.empty());
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus}) {
            ui.notice[0] = 0;
            tap(key);
            CHECK(std::string(ui.notice) == "Looking for presets" && keys_kept());
        }
        fs::remove(hold_presets);
    }
    wait(whole);

    if (face) {
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.status == PluginStatus::Ready; });
        CHECK(!ui.plugin_panel.programs);
        tap(SurfaceButton::Plus);
        CHECK(tr.octave == octave + 1 && !ui.preset_due);
        shell->set_plugins(nullptr);
        return 0;
    }

    wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
    const auto list = rack->programs(entry).list;
    CHECK(list.size() == 3);
    auto state = [&] {
        auto bytes = rack->state(entry);
        CHECK(bytes.size() >= 3 * sizeof(double));
        std::array<double, 3> values; // the fixture's Level, Wave and Gain, at the state's end
        std::memcpy(values.data(), bytes.data() + bytes.size() - sizeof(values), sizeof(values));
        return values;
    };
    auto level = [&](unsigned n) { return list[n].name == "Low" ? 0.25 : list[n].name == "Mid" ? 0.5 : 0.75; };
    auto playing = [&] { return rack->programs(entry).current; };
    auto rest = [&] { c.stage_tick(now += stage_preset_ms); }; // the turning stopped: the lights' tick loads it
    auto lands = [&](unsigned n) {
        CHECK(ui.preset_due && ui.presets_cursor == n);
        rest();
        wait([&] { return playing() == list[n].index && state()[0] == level(n); });
    };
    auto edit = [&] { // the Level turned up: the preset it plays, edited
        PluginPanel panel;
        rack->panel(entry, 0, panel);
        bool set = false;
        for (unsigned k = 0; k < panel.window.size() && !set; ++k)
            if (panel.window[k].used && std::strcmp(panel.window[k].name, "Level") == 0) {
                rack->set_param(entry, panel.window_first + k, 1000);
                set = true;
            }
        CHECK(set);
        wait([&] { return state()[0] == 1; });
    };
    auto kept = [&](unsigned n) { // nothing is sent to the plug-in: the preset it plays, as it was edited
        c.dirty = false;
        rest();
        CHECK(!c.dirty && !ui.preset_due);
        for (int k = 0; k < 20; ++k) tick();
        CHECK(state()[0] == 1 && playing() == list[n].index);
    };

    CHECK(playing() < 0);
    tap(SurfaceButton::Minus); // none playing yet: the first, whichever key
    CHECK(ui.preset_due && ui.presets_cursor == 0 && tr.octave == octave);
    ui.preset_due = 0; // let go before it landed: none still
    tap(SurfaceButton::Plus);
    lands(0);
    tap(SurfaceButton::Plus);
    lands(1);
    button(SurfaceButton::Sample, true); // Sample held: the keys are Keys' octave, as its cards are Keys'
    tap(SurfaceButton::Plus);
    CHECK(tr.octave == octave + 1 && !ui.preset_due);
    tap(SurfaceButton::Minus);
    CHECK(tr.octave == octave && !ui.preset_due);
    button(SurfaceButton::Sample, false);
    // Sample held on the plug-in's PANEL and EDIT and on B2's ARP: Keys' octave, not the plug-in's screen or its pages;
    // and a short hold's release leaves the page as it is (it took Stage home to PRESETS).
    for (auto kind : {StageKind::PluginScreen, StageKind::PluginEdit, StageKind::Arp}) {
        CHECK(stage_go(ui, EngineKind::Plugin, kind));
        CHECK(stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind == kind);
        const auto row = ui.plugin_row[entry];
        button(SurfaceButton::Sample, true);
        tap(SurfaceButton::Plus);
        button(SurfaceButton::Sample, false);
        CHECK(tr.octave == octave + 1 && !ui.preset_due && ui.plugin_row[entry] == row);
        CHECK(stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind == kind);
        button(SurfaceButton::Sample, true);
        tap(SurfaceButton::Minus);
        button(SurfaceButton::Sample, false);
        CHECK(tr.octave == octave && stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind == kind);
    }
    tap(SurfaceButton::Minus); // on ARP without Sample: the octave, as before
    CHECK(tr.octave == octave - 1 && !ui.preset_due);
    tap(SurfaceButton::Plus);
    CHECK(tr.octave == octave);
    CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
    edit();
    tap(SurfaceButton::Plus); // a press taken back before it lands
    tap(SurfaceButton::Minus);
    CHECK(ui.preset_due && ui.presets_cursor == 1);
    kept(1);
    wheel(1); // the wheel and the keys walk the same list
    lands(2);
    wheel(-1);
    lands(1);
    for (int n = 0; n < 3; ++n) tap(SurfaceButton::Plus); // three presses from the second: the last, no further
    lands(2);
    edit();
    tap(SurfaceButton::Plus); // against the end: nothing is armed
    CHECK(!ui.preset_due);
    wheel(1);
    CHECK(!ui.preset_due);
    c.prints.freeze[0].state = FreezeState::Frozen; // nor asked on a frozen track: nothing would load
    tap(SurfaceButton::Plus);
    CHECK(!ui.preset_due && ui.ask == UiState::Ask::None);
    c.prints.freeze[0] = {};
    kept(2);
    tap(SurfaceButton::Minus);
    lands(1);
    tap(SurfaceButton::Minus);
    lands(0);
    tap(SurfaceButton::Minus); // and against the start
    CHECK(!ui.preset_due);
    tr.layout = PadLayout::Drums; // a drum machine's pads: their bank stays too
    tap(SurfaceButton::Plus);
    lands(1);
    CHECK(tr.octave == octave && tr.bank == bank);
    tr.layout = PadLayout::Scale;

    ui.plugin_panel.status = PluginStatus::Crashed; // the page shows it STOPPED (a bridge that died): nothing moves
    CHECK(ui.plugin_panel.entry == entry);
    for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus}) {
        tap(key);
        CHECK(keys_kept());
    }
    wheel(1);
    CHECK(keys_kept());
    wait([&] { return ui.plugin_panel.status == PluginStatus::Ready; }); // read from the rack again

    c.prints.freeze[0].state = FreezeState::Frozen; // a frozen track asks before a preset would load
    tap(SurfaceButton::Plus);
    CHECK(ui.ask == UiState::Ask::Unfreeze && keys_kept());
    tap(SurfaceButton::TopLeft);
    CHECK(ui.ask == UiState::Ask::None);
    c.prints.freeze[0] = {};

    // The plug-in opened again (picked again in the picker) with a press still to land: nothing moves while it opens,
    // and the press lands on nothing while nothing is listed. It plays on the preset it played.
    tap(SurfaceButton::Plus);
    CHECK(ui.preset_due && ui.presets_cursor == 2);
    hold(hold_open);
    hold(hold_presets);
    rack->reload(entry);
    wait(opening);
    tap(SurfaceButton::Minus);
    CHECK(ui.preset_due && ui.presets_cursor == 2 && tr.octave == octave);
    fs::remove(hold_open);
    wait(looking);
    rest(); // nothing listed: it lands on nothing
    fs::remove(hold_presets);
    wait(whole);
    for (int k = 0; k < 20; ++k) tick();
    CHECK(playing() == list[1].index && tr.octave == octave && tr.bank == bank);

    shell->set_plugins(nullptr);
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: test-stage-keys CARD PLUGIN MODE");
        run(argv[1], argv[2], argv[3]);
        std::printf("stage keys ok\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test-stage-keys %s: %s\n", argc > 3 ? argv[3] : "", e.what());
        return 1;
    }
}
