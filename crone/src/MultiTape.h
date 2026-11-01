//
// MultiTape: a RAM-resident 4-track tape (OP-1 style).
//
// tracks are stored as 16-bit interleaved chunks, allocated on demand by a
// worker thread. the audio thread never allocates or frees memory.
// see notes/multitape/design.md
//

#ifndef CRONE_MULTITAPE_H
#define CRONE_MULTITAPE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crone {

class MultiTape {
  public:
    static constexpr int NumTracks = 4;
    static constexpr size_t ChunkShift = 15;
    static constexpr size_t ChunkFrames = size_t(1) << ChunkShift;
    static constexpr size_t PeakShift = 11;
    static constexpr size_t PeakFrames = size_t(1) << PeakShift;
    static constexpr size_t PeaksPerChunk = ChunkFrames / PeakFrames;
    static constexpr float MaxSeconds = 360.f;
    static constexpr size_t DefaultMemReserve = size_t(64) << 20;

    enum Source {
        SourceInput = 0,
        SourceEngine = 1,
        SourceSoftcut = 2,
        SourceAll = 3,
        NumSources
    };

    struct Status {
        bool playing;
        bool recording;
        int armed; // -1 = none
        int source;
        int channels;
        size_t posFrame;
        size_t maxFrames;
        float sampleRate;
        size_t endFrame[NumTracks];
        size_t allocatedBytes;
        bool memFull;
        int jobsPending;
        int droppedBlocks; // record blocks lost to missing chunks
    };

    // returns available system memory in bytes; replaceable for tests
    typedef std::function<size_t()> MemProbe;

    MultiTape();
    ~MultiTape();
    MultiTape(const MultiTape &) = delete;
    MultiTape &operator=(const MultiTape &) = delete;

    //--- setup (non-realtime; before the audio thread runs)
    void init(float sampleRate, int channels, bool startWorker = true);
    // channel count from MemTotal (< 1.5 GiB -> mono), env override
    static int defaultChannels();
    void setMemProbe(MemProbe p);
    void setMemReserve(size_t bytes) {
        memReserve = bytes;
    }

    //--- audio thread
    // src: record source (stereo). out: stereo, playback is added to it.
    void process(const float *const src[2], float *const out[2], size_t numFrames);
    void setPlaying(bool play);
    void seek(size_t frame);
    void setArmed(int track);
    void setRecord(bool rec);
    void setTrackLevel(int track, float amp);
    void setTrackPan(int track, float pan);
    void setTrackMute(int track, bool mute);
    void setSource(int source);

    //--- any thread
    void setLatency(size_t captureFrames, size_t playbackFrames);
    Status getStatus() const;
    int getSource() const {
        return source.load(std::memory_order_relaxed);
    }
    // max peak (0-255) per bin, for bins of binFrames starting at startFrame
    void overview(int track, size_t startFrame, size_t binFrames, int numBins, uint8_t *out) const;

    //--- control thread: queued for the worker
    void requestLoad(int track, const std::string &path, size_t startFrame);
    void requestSave(const std::string &dir);
    void requestClear(int track);

    // run one worker pass synchronously (tests, or no worker thread)
    void service();
    // block until the job queue is empty (tests)
    void flush();
    // read a frame of a track as float; for tests and tools
    float peekSample(int track, size_t frame, int channel) const;

  private:
    typedef int16_t Sample;

    struct Track {
        std::unique_ptr<std::atomic<Sample *>[]> chunks;
        std::unique_ptr<std::atomic<uint8_t>[]> peaks;
        std::atomic<size_t> endFrame{0};
        // audio thread mix state
        float level = 1.f;
        float pan = 0.f;
        bool mute = false;
        float gainL = 0.f; // gains applied at end of previous block
        float gainR = 0.f;
    };

    struct Job {
        enum Type { Load,
                    Save,
                    Clear } type;
        int track;
        std::string path;
        size_t startFrame;
    };

    struct Retired {
        Sample *ptr;
        uint64_t epoch;
    };

    // geometry
    float sr = 48000.f;
    int chans = 1;
    size_t numChunks = 0;
    size_t numPeaks = 0;
    size_t maxFrames = 0;
    size_t chunkSamples = 0; // ChunkFrames * chans
    size_t fadeFrames = 240;

    Track tracks[NumTracks];

    //--- audio thread state (published via atomics where the worker needs it)
    std::atomic<bool> playing{false};
    std::atomic<size_t> pos{0};
    std::atomic<int> armed{-1};
    std::atomic<bool> recEnabled{false};
    std::atomic<int> recTrack{-1};
    std::atomic<int> source{SourceInput};
    std::atomic<uint64_t> blockCount{0};
    std::atomic<int> droppedBlocks{0};
    bool stopPending = false;
    bool seekPending = false;
    size_t seekTarget = 0;
    float transportGain = 0.f;
    float recGain = 0.f;
    uint32_t peakAcc = 0;
    size_t peakBlock = SIZE_MAX;

    std::atomic<size_t> latCapture{0};
    std::atomic<size_t> latPlayback{0};

    //--- worker state
    std::atomic<size_t> allocatedBytes{0};
    std::atomic<bool> memFull{false};
    std::atomic<int> jobsPending{0};
    size_t memReserve = DefaultMemReserve;
    MemProbe memProbe;
    std::vector<Retired> retired;
    int lastArmed = -1;

    std::deque<Job> jobs;
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::condition_variable idleCv;
    std::unique_ptr<std::thread> worker;
    std::atomic<bool> quit{false};
    std::mutex serviceMutex;

    void workLoop();
    void ensureRecordChunks();
    void reclaimEmptyChunks(int track);
    void reclaimRetired(bool force);
    Sample *allocChunk();
    void retireChunk(Sample *p);
    void freeChunk(Sample *p);
    void runJob(const Job &job);
    void doLoad(int track, const std::string &path, size_t startFrame);
    void doSave(const std::string &dir);
    void doClear(int track);
    void recomputePeaks(int track, size_t chunk, const Sample *data);
    size_t recOffset() const;
    void flushRecord(int track, size_t end);

    static inline Sample toSample(float x) {
        if (x > 1.f) {
            x = 1.f;
        } else if (x < -1.f) {
            x = -1.f;
        }
        return static_cast<Sample>(x * 32767.f + (x >= 0.f ? 0.5f : -0.5f));
    }
    static inline float toFloat(Sample s) {
        return static_cast<float>(s) * (1.f / 32768.f);
    }
};

} // namespace crone

#endif // CRONE_MULTITAPE_H
