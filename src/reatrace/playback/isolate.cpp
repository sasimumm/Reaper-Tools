// Filtered listening: installs the isolate JSFX, adds or removes it on the audio track, and
// feeds it the coming pitch changes of the followed voices.
#include "playback/isolate.h"

#include <array>
#include <fstream>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include "analysis.h"
#include "index.h"
#include "notes/commands.h"
#include "shared/guard.h"
#include "state.h"

static const char *const FX_NAME = "JS:ReaTrace/isolate.jsfx"; // as TrackFX_AddByName finds it
static const double HORIZON = 10; // seconds of notes ahead that the changes are taken from

// Writes the JSFX to REAPER's Effects folder when missing or out of date; false if that failed.
static bool installJSFX() {
    const std::string dir = std::string(GetResourcePath()) + "/Effects/ReaTrace";
    const std::string path = dir + "/isolate.jsfx";
    const std::string want = isolateJSFX();

    std::ifstream in(path);
    std::stringstream have;
    have << in.rdbuf();
    if (have.str() == want) {
        return true;
    }
    RecursiveCreateDirectory(dir.c_str(), 0);
    std::ofstream out(path, std::ios::trunc);
    out << want;
    return static_cast<bool>(out);
}

// The track of the audio item being transcribed, or NULL.
static MediaTrack *audioTrack() {
    return ValidatePtr2(NULL, g_session.audio, "MediaItem*") ? GetMediaItem_Track(g_session.audio)
                                                             : NULL;
}

// The FX's GUID as a string, "" if there is no such FX.
static std::string fxGUID(MediaTrack *tr, int fx) {
    char guid[64] = "";
    if (const GUID *g = TrackFX_GetFXGUID(tr, fx)) {
        guidToString(g, guid);
    }
    return guid;
}

// Where the filter is: kept, so it is removed from there even after the audio item was moved to
// another track or deleted.
static struct {
    ReaProject *proj = NULL;
    MediaTrack *track = NULL;
    std::string guid;
} g_fx;

// The filter's track and index there; {NULL, -1} if it is gone. One found on the audio track by its
// description (left by an earlier session) is taken as the filter.
static std::pair<MediaTrack *, int> findFX() {
    if (g_fx.track && ValidatePtr2(NULL, g_fx.proj, "ReaProject*") &&
        ValidatePtr2(g_fx.proj, g_fx.track, "MediaTrack*")) {
        for (int i = 0; i < TrackFX_GetCount(g_fx.track); i++) {
            if (fxGUID(g_fx.track, i) == g_fx.guid) {
                return {g_fx.track, i};
            }
        }
    }
    g_fx = {};
    MediaTrack *tr = audioTrack();
    for (int i = 0; tr && i < TrackFX_GetCount(tr); i++) {
        char name[256] = "";
        TrackFX_GetFXName(tr, i, name, sizeof(name));
        if (std::string_view(name).find(ISOLATE_DESC) != std::string_view::npos) {
            g_fx = {EnumProjects(-1, NULL, 0), tr, fxGUID(tr, i)};
            return {tr, i};
        }
    }
    return {NULL, -1};
}

static std::vector<double> g_sent; // parameter values last sent, to only send changes...
static std::string g_sentTo;       // ...to this FX instance (its GUID): a re-added one has defaults

bool isolateOn() { return findFX().second >= 0; }

// Adds (installing the JSFX first) the filter on the audio track, or removes it from where it is.
void setIsolate(bool on) {
    const auto [fxTrack, fx] = findFX();
    g_sent.clear();
    if (!on) {
        if (fx >= 0) {
            TrackFX_Delete(fxTrack, fx);
        }
        g_fx = {};
        return;
    }
    MediaTrack *tr = audioTrack();
    if (!tr || fx >= 0) {
        return;
    }
    const int added = installJSFX() ? TrackFX_AddByName(tr, FX_NAME, false, -1) : -1;
    if (added < 0) {
        ReportFailure("isolate", "couldn't add the ReaTrace isolate JSFX to the audio track");
        return;
    }
    g_fx = {EnumProjects(-1, NULL, 0), tr, fxGUID(tr, added)};
    updateIsolate();
}

void toggleIsolate(bool duck) {
    if (isolateOn() && g_settings.isolateDuck == duck) {
        setIsolate(false);
        return;
    }
    g_settings.isolateDuck = duck;
    g_sent.clear(); // already on in the other mode: send everything again
    setIsolate(true);
}

// Sends the filter its parameters, only those that changed since the last call.
void updateIsolate() {
    const auto [tr, fx] = findFX();
    if (fx < 0) {
        return;
    }
    if (tr != audioTrack()) { // the audio item moved to another track: the filter goes with it
        setIsolate(false);
        setIsolate(true);
        return;
    }
    const std::string guid = fxGUID(tr, fx);
    const bool playing = GetPlayState() & 1;
    const double now = playing ? GetPlayPosition2() : GetCursorPosition();

    // stopped and nothing changed: what was sent still holds
    const auto inputs = std::make_tuple(
        now, g_session.notesVersion, g_session.ed.active() ? g_session.ed.moves : -1,
        g_analysis.valVersion, g_session.voice, g_settings.isolateAll, g_ui.hiddenVoices,
        g_settings.isolateWidth, g_settings.isolateBackground, g_settings.isolateDuck,
        g_settings.suggestFrom);
    static std::remove_const_t<decltype(inputs)> s_inputs;
    if (!playing && inputs == s_inputs && g_sentTo == guid) {
        return;
    }
    s_inputs = inputs;
    if (g_sentTo != guid) {
        g_sent.clear();
        g_sentTo = guid;
    }

    static_assert(ISOLATE_VOICES == WINDOW_VOICES);
    // what is followed: each voice's notes, and its suggestions where nothing covers them, over
    // the next seconds (ISOLATE_CHANGES changes are sent; refreshed every tick). Ducking follows
    // only what is notated, in every shown voice.
    const bool duck = g_settings.isolateDuck;
    const double horizon = now + HORIZON;
    auto followed = [duck](int voice) {
        const bool shown = !(g_ui.hiddenVoices >> voice & 1);
        return voice == g_session.voice || ((duck || g_settings.isolateAll) && shown);
    };
    std::array<std::vector<Note>, WINDOW_VOICES> spans;
    for (int voice = 0; voice < WINDOW_VOICES; voice++) {
        if (!followed(voice)) {
            continue;
        }
        for (const Note &n :
             voice == g_session.voice ? g_session.ed.notes : g_session.others[idx(voice)].notes) {
            if (n.end > now - 1 && n.start < horizon) {
                spans[idx(voice)].push_back(n);
            }
        }
    }

    const std::vector<Segment> &sug = suggestions();
    for (size_t i = 0; i < sug.size(); i++) {
        const double a = frameTime(sug[i].f0), b = frameTime(sug[i].f1 + 1);
        const int voice = suggestionVoice(i);
        if (!duck && voice >= 0 && b > now - 1 && a < horizon && followed(voice) && !covered(i)) {
            spans[idx(voice)].push_back({a, b, static_cast<int>(lround(sug[i].pitch)), false, -1});
        }
    }

    // the parameters (see isolate.h): width, level, mode, then per voice its changes
    std::vector<double> v(idx(isolateParam(WINDOW_VOICES)));
    v[0] = g_settings.isolateWidth;
    v[1] = g_settings.isolateBackground;
    v[2] = duck ? 1 : 0;
    for (int voice = 0; voice < WINDOW_VOICES; voice++) {
        const auto sched = pitchSchedule(spans[idx(voice)], now - 1, ISOLATE_CHANGES);
        size_t at = idx(isolateParam(voice));
        v[at++] = static_cast<double>(sched.size());
        for (const auto &[t, p] : sched) {
            v[at++] = t;
            v[at++] = p;
        }
    }

    for (size_t i = 0; i < v.size(); i++) {
        if (i >= g_sent.size() || g_sent[i] != v[i]) {
            TrackFX_SetParam(tr, fx, static_cast<int>(i), v[i]);
        }
    }
    g_sent = std::move(v);
}
