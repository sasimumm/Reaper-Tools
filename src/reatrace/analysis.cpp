// The background analysis of the open item and its caches: one display spectrogram (with its
// pitch contour, segments and onsets) per channel, and the last note estimate.
#include "analysis.h"

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <thread>

#include "dsp/estimate.h"
#include "index.h"

// --- spectrogram ---------------------------------------------------------------

// The raw channels a spectrogram is computed from (CH_MIX..CH_SIDE); the centre is derived from
// the mix and side ones.
static const int RAW_CHANNELS = CH_SIDE + 1;

void updateValues() {
    renderValues(*g_analysis.spec, g_settings.rp, g_analysis.val);
    makeColormap(g_settings.rp.bias, g_analysis.lut, g_settings.rp.colormap);
    g_analysis.valVersion++;
    redraw();
}

void updateColormap() {
    makeColormap(g_settings.rp.bias, g_analysis.lut, g_settings.rp.colormap);
    g_analysis.valVersion++;
    redraw();
}

double frameTime(double f) { return g_session.audioPos + f / g_analysis.spec->frameRate(); }

// The samples analysed: the recording and the part of it the item plays. The item's position isn't
// in it (samples are read from the item start; frames map to time through the live position).
struct AudioKey {
    std::string audio; // audioIdentity(): another take or source is another recording
    double len = 0, offs = 0, rate = 1;
    bool operator==(const AudioKey &) const = default;
};

// Whether two transforms give the same spectrogram.
static bool sameNSGT(const NSGTParams &a, const NSGTParams &b) {
    return a.sampleRate == b.sampleRate && a.fmin == b.fmin && a.fmax == b.fmax &&
           a.binsPerOctave == b.binsPerOctave && a.hop == b.hop && a.gamma == b.gamma;
}

// What a channel's display spectrogram (and its contour, onsets) depends on.
struct SpecKey {
    AudioKey audio;
    NSGTParams p;
    bool operator==(const SpecKey &o) const { return audio == o.audio && sameNSGT(p, o.p); }
};

// What the note estimate depends on: not the display settings (it has its own spectrogram).
struct EstimateKey {
    AudioKey audio;
    int channel = CH_MIX;
    bool vote = true;
    bool operator==(const EstimateKey &) const = default;
};

// The pitch analysis of a display spectrogram.
struct Vocal {
    Contour contour;
    std::vector<Segment> segments;
    std::vector<int> onsets;
};

// The note estimator's output (see AnalysisResult for the fields).
struct Estimate {
    std::vector<Segment> estimated;
    std::vector<int> estimatedVoice;
    std::string note;
};

// A channel's sample from the left and right ones (CH_MIX and CH_CENTRE: the mid).
static float channelSample(int ch, float a, float b) {
    if (ch == CH_LEFT) {
        return a;
    }
    if (ch == CH_RIGHT) {
        return b;
    }
    if (ch == CH_SIDE) {
        return 0.5f * (a - b);
    }
    return 0.5f * (a + b);
}

// basic-pitch's compiled model, installed next to the plugin
static std::string basicPitchModel() {
    return std::string(GetResourcePath()) + "/UserPlugins/reatrace/nmp.mlmodelc";
}

static Vocal analyzeVocal(const Spectrogram &s) {
    Vocal v;
    v.contour = trackPitch(s);
    v.onsets = detectOnsets(s);
    v.segments = segmentContour(v.contour, s.frameRate(), &v.onsets);
    return v;
}

// A channel's display spectrogram and, once computed, its vocal analysis.
struct ChannelCache {
    bool valid = false;
    SpecKey key;
    std::shared_ptr<Spectrogram> spec; // shared with the job and the installed analysis
    bool vocalValid = false;
    Vocal vocal;
};
// One entry per channel (CH_*). Memory ceiling: ~30 MB per channel per 3 minutes of audio.
static std::array<ChannelCache, CH_CENTRE + 1> g_cache;

// The last note estimate (one: it is small, but slow to recompute).
static struct {
    bool valid = false;
    EstimateKey key;
    Estimate est;
} g_estimate;

void clearCache() {
    for (ChannelCache &c : g_cache) {
        c = ChannelCache();
    }
    g_estimate = {};
}

// Whether channel ch's spectrogram for `key` is cached.
static bool cached(int ch, const SpecKey &key) {
    return g_cache[idx(ch)].valid && g_cache[idx(ch)].key == key;
}

// The audio, read on the main thread (accessors must be), resampled to the analysis rate: len
// seconds from the item start at sr Hz. A mono source gives the same samples in left and right.
static void readAudio(MediaItem_Take *take, double len, double sr, std::vector<float> &left,
                      std::vector<float> &right) {
    AudioAccessor *acc = CreateTakeAudioAccessor(take);
    const int nch =
        std::max(1, std::min(2, GetMediaSourceNumChannels(GetMediaItemTake_Source(take))));
    const long long n = static_cast<long long>(len * sr);
    left.assign(idx(std::max(0LL, n)), 0);
    right.assign(left.size(), 0);

    std::vector<double> buf; // interleaved, nch per frame
    const int chunk = 65536; // frames per read
    for (long long pos = 0; pos < n; pos += chunk) {
        const int m = static_cast<int>(std::min<long long>(chunk, n - pos));
        // the accessor leaves the buffer untouched where there's no audio
        buf.assign(idx(m) * idx(nch), 0);
        // time relative to item start
        GetAudioAccessorSamples(acc, static_cast<int>(sr), nch, static_cast<double>(pos) / sr, m,
                                buf.data());
        for (int i = 0; i < m; i++) {
            left[idx(pos + i)] = static_cast<float>(buf[idx(i) * idx(nch)]);
            right[idx(pos + i)] = static_cast<float>(buf[idx(i) * idx(nch) + (idx(nch - 1))]);
        }
    }
    DestroyAudioAccessor(acc);
}

// A background analysis of one channel: its display spectrogram and contour, the note estimate, or
// both, whichever isn't cached. The worker only touches the job; the main thread polls `done` from
// the timer and installs the results.
struct Job {
    std::thread thread;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int> permille{0};
    SpecKey key;
    EstimateKey estKey;
    int channel = CH_MIX;
    std::vector<float> left, right;
    bool needSpec = false;                    // the display spectrogram and contour
    std::array<bool, RAW_CHANNELS> need = {}; // raw channels to compute for it
    // computed, or the cache's when only needed for the centre
    std::array<std::shared_ptr<Spectrogram>, RAW_CHANNELS> raw;
    std::shared_ptr<Spectrogram> spec; // the requested channel
    Vocal vocal;
    bool needEstimate = false;
    Estimate est;
    std::string modelPath;

    // The note estimator, on the channel's own signal (the centre uses the mid), with its own
    // spectrogram unless the display's has the same settings.
    void estimate(const std::function<bool(double)> &progress) {
        std::vector<float> mono(left.size());
        for (size_t i = 0; i < mono.size(); i++) {
            const float a = left[i], b = i < right.size() ? right[i] : a;
            mono[i] = channelSample(channel, a, b);
        }
        // free the input: ~80 MB each at 15 min
        std::vector<float>().swap(left);
        std::vector<float>().swap(right);

        const NSGTParams ep = estimateNSGTParams();
        const bool same = needSpec && sameNSGT(spec->p, ep);
        // progress over [a, b) of the whole: the spectrogram is the first fifth
        auto part = [&](double a, double b) {
            return [&, a, b](double f) { return progress(a + (b - a) * f); };
        };
        Spectrogram own;
        if (!same) {
            own = computeNSGT(
                ep, static_cast<long long>(mono.size()),
                [&](long long start, std::span<float> out) { readPadded(mono, start, out); },
                part(0, 0.2));
        }
        const Spectrogram &es = same ? *spec : own;
        if (cancel) {
            return;
        }

        // basic-pitch alongside the main NMF (which doesn't need it): ~0.7 s saved per 3 minutes
        BasicPitch bp;
        std::string err;
        bool haveBp = false;
        std::jthread bpThread([&] {
            try {
                haveBp = runBasicPitch(modelPath, mono, es.frames, bp, &err,
                                       [&](double) { return !cancel; });
            } catch (const std::exception &e) { // must not escape the thread
                err = e.what();
            } catch (...) {
                err = "unknown error";
            }
        }); // joined by basicPitch(), or here when the estimator stops before it
        auto basicPitch = [&]() -> const BasicPitch * {
            bpThread.join();
            est.note = haveBp ? "" : "basic-pitch: " + err;
            return haveBp ? &bp : nullptr;
        };

        EstimateParams opts;
        opts.vote = estKey.vote;
        const std::vector<NoteEstimate> notes =
            estimateNotes(es, mono, basicPitch, opts, part(0.2, 1));
        for (const NoteEstimate &e : notes) { // f1 is exclusive there, inclusive in a Segment
            est.estimated.push_back({e.f0, e.f1 - 1, static_cast<double>(e.pitch)});
        }
        est.estimatedVoice = assignVoices(notes, es.frameRate(), WINDOW_VOICES);
    }

    std::string error; // set instead of the results when the analysis failed

    // Exceptions must not escape the thread (that terminates REAPER); the main thread shows them.
    void run() {
        try {
            work();
        } catch (const std::bad_alloc &) {
            error = "out of memory";
        } catch (const std::exception &e) {
            error = e.what();
        } catch (...) {
            error = "unknown error";
        }
        if (!error.empty()) {
            done = true;
        }
    }

    // The analysis itself, with progress in permille.
    void work() {
        // progress weights: one per spectrogram, two for the note estimation
        int stages = needEstimate ? 2 : 0;
        for (bool b : need) {
            stages += b;
        }
        stages = std::max(1, stages);
        int stage = 0;

        if (needSpec) {
            computeSpec(stage, stages);
        }
        if (cancel) {
            return;
        }
        if (needEstimate) {
            estimate([&](double f) {
                permille = static_cast<int>(1000 * (stage + f * 2) / stages);
                return !cancel;
            });
        }
        if (cancel) {
            return;
        }
        permille = 1000;
        done = true;
    }

    // The raw channels in `need`, then the requested channel's spectrogram (the centre from the
    // mix and side) and its vocal analysis. stage counts the spectrograms done, of `stages`.
    void computeSpec(int &stage, int stages) {
        // sample j of x, 0 outside it
        const auto sampleAt = [](const std::vector<float> &x, long long j) {
            return j >= 0 && j < static_cast<long long>(x.size()) ? x[idx(j)] : 0;
        };
        for (int ch = 0; ch < RAW_CHANNELS; ch++) {
            if (!need[idx(ch)]) {
                continue;
            }
            raw[idx(ch)] = std::make_shared<Spectrogram>(computeNSGT(
                key.p, static_cast<long long>(left.size()),
                [&](long long start, std::span<float> out) {
                    for (size_t i = 0; i < out.size(); i++) {
                        const long long j = start + static_cast<long long>(i);
                        out[i] = channelSample(ch, sampleAt(left, j), sampleAt(right, j));
                    }
                },
                [&](double f) {
                    permille = static_cast<int>(1000 * (stage + f) / stages);
                    return !cancel;
                }));
            stage++;
            if (cancel) {
                return;
            }
        }

        if (channel == CH_CENTRE && raw[CH_SIDE]) {
            // centre-panned content (usually the lead vocal): mid magnitudes minus side magnitudes
            // (not its noise: the main thread may be rewriting that one's; install() recomputes it)
            const Spectrogram &mid = *raw[CH_MIX];
            spec = std::make_shared<Spectrogram>();
            spec->p = mid.p;
            spec->bins = mid.bins;
            spec->frames = mid.frames;
            spec->mag = mid.mag;
            if (raw[CH_SIDE]->mag.size() == spec->mag.size()) {
                for (size_t i = 0; i < spec->mag.size(); i++) {
                    spec->mag[i] = std::max(0.0f, spec->mag[i] - raw[CH_SIDE]->mag[i]);
                }
            }
        } else { // a plain channel, or a cached centre missing only its contour
            spec = raw[idx(channel == CH_CENTRE ? CH_MIX : channel)];
        }
        vocal = analyzeVocal(*spec);
    }
};
static std::unique_ptr<Job> g_job;
static bool g_autoOnInstall; // auto-calibrate the display once the analysis arrives

void cancelJob() {
    if (!g_job) {
        return;
    }
    g_job->cancel = true;
    if (g_job->thread.joinable()) {
        g_job->thread.join();
    }
    g_job.reset();
}

// Makes c's spectrogram and vocal analysis and the estimate e the shown analysis.
static void install(const ChannelCache &c, const Estimate &e) {
    g_analysis.spec = c.spec ? c.spec : std::make_shared<Spectrogram>();
    g_analysis.contour = c.vocal.contour;
    g_analysis.segments = c.vocal.segments;
    g_analysis.onsets = c.vocal.onsets;
    g_analysis.estimated = e.estimated;
    g_analysis.estimatedVoice = e.estimatedVoice;
    g_analysis.estimateNote = e.note;

    g_analysis.snapTimes.clear();
    for (int f : g_analysis.onsets) {
        g_analysis.snapTimes.push_back(frameTime(f));
    }
    for (const Segment &sg : g_analysis.segments) {
        g_analysis.snapTimes.push_back(frameTime(sg.f0));
        g_analysis.snapTimes.push_back(frameTime(sg.f1 + 1)); // f1 is the last frame
    }
    std::sort(g_analysis.snapTimes.begin(), g_analysis.snapTimes.end());

    estimateNoise(*g_analysis.spec, g_settings.rp.noiseFraction);
    if (g_autoOnInstall) {
        autoCalibrate(*g_analysis.spec, g_settings.rp);
        g_autoOnInstall = false;
    }
    g_ui.analysisDirty = false;
    updateValues();
}

// Memory ceiling: 15 minutes peaks at ~1.3 GB in the note estimator (measured), plus ~150 MB per
// cached channel spectrogram; longer items are refused rather than risking running out.
static const double MAX_ANALYSIS_SECONDS = 15 * 60;

void requestAnalysis(bool autoCalibrateAfter) {
    MediaItem_Take *take = GetActiveTake(g_session.audio);
    if (!take) {
        return;
    }
    g_autoOnInstall |= autoCalibrateAfter;
    g_settings.np.binsPerOctave = 12 * static_cast<int>(lround(g_settings.binsPerSemitone));
    cancelJob();
    g_analysis.error.clear();
    if (g_session.audioLen > MAX_ANALYSIS_SECONDS) {
        g_analysis.error =
            "not analysed: items over 15 minutes need too much memory; split the item";
        redraw();
        return;
    }

    const AudioKey audio = {
        g_session.audioIdentity, g_session.audioLen, g_session.audioOffs, g_session.audioRate
    };
    const int ch = g_settings.channel;
    const SpecKey key = {audio, g_settings.np};
    const EstimateKey estKey = {audio, ch, g_settings.estimateVote};
    const bool haveSpec = cached(ch, key) && g_cache[idx(ch)].vocalValid;
    const bool haveEstimate = g_estimate.valid && g_estimate.key == estKey;
    if (haveSpec && haveEstimate) {
        install(g_cache[idx(ch)], g_estimate.est);
        return;
    }

    auto job = std::make_unique<Job>();
    job->key = key;
    job->estKey = estKey;
    job->channel = ch;
    job->modelPath = basicPitchModel();
    job->needSpec = !haveSpec;
    job->needEstimate = !haveEstimate;
    if (job->needSpec) {
        // the raw channels the requested one comes from; -1: none
        const int rawCh[2] = {ch == CH_CENTRE ? CH_MIX : ch, ch == CH_CENTRE ? CH_SIDE : -1};
        for (int r : rawCh) {
            if (r < 0) {
                continue;
            }
            if (cached(r, key)) {
                job->raw[idx(r)] = g_cache[idx(r)].spec;
            } else {
                job->need[idx(r)] = true;
            }
        }
        if (ch == CH_CENTRE && cached(CH_CENTRE, key)) { // only the vocal analysis is missing
            job->need[CH_MIX] = job->need[CH_SIDE] = false;
            job->raw[CH_MIX] = g_cache[CH_CENTRE].spec;
            job->raw[CH_SIDE] = nullptr;
        }
    }

    const bool needRaw = std::ranges::any_of(job->need, [](bool b) { return b; });
    if (job->needEstimate || needRaw) {
        readAudio(take, g_session.audioLen, g_settings.np.sampleRate, job->left, job->right);
    }

    Job *j = job.get();
    job->thread = std::thread([j] { j->run(); });
    g_job = std::move(job);
    g_ui.analysisDirty = false;
    redraw();
}

// Installs a finished analysis into the cache and the view.
void pollJob() {
    if (!g_job) {
        return;
    }
    if (!g_job->done) {
        redraw(); // keep the progress display moving
        return;
    }

    g_job->thread.join();
    Job &j = *g_job;
    if (!j.error.empty()) {
        g_analysis.error = "analysis failed: " + j.error;
        g_job.reset();
        redraw();
        return;
    }

    if (j.needSpec) {
        // raw channels computed on the way to another one are cached without a vocal analysis
        for (int r = 0; r < RAW_CHANNELS; r++) {
            if (j.need[idx(r)] && r != j.channel) {
                ChannelCache &c = g_cache[idx(r)];
                c = ChannelCache();
                c.valid = true;
                c.key = j.key;
                c.spec = std::move(j.raw[idx(r)]);
            }
        }
        ChannelCache &c = g_cache[idx(j.channel)];
        c.valid = true;
        c.key = j.key;
        c.spec = std::move(j.spec);
        c.vocal = std::move(j.vocal);
        c.vocalValid = true;
    }
    if (j.needEstimate) {
        g_estimate.valid = true;
        g_estimate.key = j.estKey;
        g_estimate.est = std::move(j.est);
    }
    const int ch = j.channel;
    g_job.reset();
    install(g_cache[idx(ch)], g_estimate.est);
}

int analysisProgress() { return g_job ? g_job->permille.load() : -1; }

// The version keeps counting: caches keyed on it (the spectrogram layer, the overview) must not
// mistake the next recording's values for the last one's.
void clearAnalysis() {
    const int version = g_analysis.valVersion;
    g_analysis = AnalysisResult();
    g_analysis.valVersion = version + 1;
}

// The take and source pointers stand for the take and source; the file name for what the source
// plays, which can be replaced in place.
std::string audioIdentity(MediaItem_Take *take) {
    if (!take) {
        return "";
    }
    PCM_source *src = GetMediaItemTake_Source(take);
    char file[1024] = "";
    if (src) {
        GetMediaSourceFileName(src, file, sizeof(file));
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%p %p ", static_cast<void *>(take), static_cast<void *>(src));
    return buf + std::string(file);
}
