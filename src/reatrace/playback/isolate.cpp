// Filtered listening: installs the isolate JSFX, adds or removes it on the audio track, and
// feeds it the coming pitch changes of the followed voices.
#include "playback/isolate.h"

#include <array>
#include <fstream>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>

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

// Index of the isolate JSFX on tr (found by its description), or -1.
static int findFX(MediaTrack *tr) {
    for (int i = 0; tr && i < TrackFX_GetCount(tr); i++) {
        char name[256] = "";
        TrackFX_GetFXName(tr, i, name, sizeof(name));
        if (std::string_view(name).find(ISOLATE_DESC) != std::string_view::npos) {
            return i;
        }
    }
    return -1;
}

static std::vector<double> g_sent; // parameter values last sent, to only send changes...
static std::string g_sentTo;       // ...to this FX instance (its GUID): a re-added one has defaults

bool isolateOn() { return findFX(audioTrack()) >= 0; }

// Adds (installing the JSFX first) or removes the filter on the audio track.
void setIsolate(bool on) {
    MediaTrack *tr = audioTrack();
    const int fx = findFX(tr);
    g_sent.clear();
    if (!tr || on == (fx >= 0)) {
        return;
    }
    if (!on) {
        TrackFX_Delete(tr, fx);
        return;
    }
    if (!installJSFX() || TrackFX_AddByName(tr, FX_NAME, false, -1) < 0) {
        ReportFailure("isolate", "couldn't add the ReaTrace isolate JSFX to the audio track");
        return;
    }
    updateIsolate();
}

void toggleIsolate(bool remove) {
    if (isolateOn() && g_settings.isolateRemove == remove) {
        setIsolate(false);
        return;
    }
    g_settings.isolateRemove = remove;
    g_sent.clear(); // already on in the other mode: send everything again
    setIsolate(true);
}

// Sends the filter its parameters, only those that changed since the last call.
void updateIsolate() {
    MediaTrack *tr = audioTrack();
    const int fx = findFX(tr);
    if (fx < 0) {
        return;
    }
    char guid[64] = "";
    if (const GUID *g = TrackFX_GetFXGUID(tr, fx)) {
        guidToString(g, guid);
    }
    const bool playing = GetPlayState() & 1;
    const double now = playing ? GetPlayPosition2() : GetCursorPosition();

    // stopped and nothing changed: what was sent still holds
    const auto inputs = std::make_tuple(
        now, g_session.notesVersion, g_session.ed.active() ? g_session.ed.moves : -1,
        g_analysis.valVersion, g_session.voice, g_settings.isolateAll, g_ui.hiddenVoices,
        g_settings.isolateWidth, g_settings.isolateBackground, g_settings.isolateRemove,
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
    // the next seconds (ISOLATE_CHANGES changes are sent; refreshed every tick). Removing follows
    // only what is notated, in every shown voice.
    const bool remove = g_settings.isolateRemove;
    const double horizon = now + HORIZON;
    auto followed = [remove](int voice) {
        const bool shown = !(g_ui.hiddenVoices >> voice & 1);
        return voice == g_session.voice || ((remove || g_settings.isolateAll) && shown);
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
        if (!remove && voice >= 0 && b > now - 1 && a < horizon && followed(voice) && !covered(i)) {
            spans[idx(voice)].push_back({a, b, static_cast<int>(lround(sug[i].pitch)), false, -1});
        }
    }

    // the parameters (see isolate.h): width, level, mode, then per voice its changes
    std::vector<double> v(idx(isolateParam(WINDOW_VOICES)));
    v[0] = g_settings.isolateWidth;
    v[1] = g_settings.isolateBackground;
    v[2] = remove ? 1 : 0;
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
