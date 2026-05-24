#ifndef DAQ_INTERFACE_H
#define DAQ_INTERFACE_H

#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

/// Information about a single DAQ device
struct DAQDeviceInfo {
    std::string name;
    std::vector<std::string> aiChannels;
    std::vector<std::string> aoChannels;
    double maxAIRate = 0;
    double maxAORate = 0;
};

/// Thrown on DAQ errors
class DAQException : public std::runtime_error {
public:
    DAQException(const std::string &msg) : std::runtime_error(msg) {}
};

/// AO timing mode chosen by the backend. Surfaced for diagnostics
/// (RunDialog status block, log lines) and for any caller that wants
/// to know what trade-off it's running with.
enum class AOMode {
    /// Hardware-timed single-point: each scan latched on the next AO
    /// sample-clock edge. Sub-microsecond determinism. PCIe / PXI
    /// X-series only.
    HwTimedSinglePoint,
    /// Continuous-buffer: AO sample clock drains a hardware FIFO at a
    /// fixed rate; software keeps the FIFO topped up. Microsecond
    /// determinism once primed. Latency = buffer depth (~50 ms by
    /// default). Most USB devices support this.
    ContSampsBuffer,
    /// Software-timed: each writeAO bypasses any sample clock and
    /// reaches the DAC at USB transaction speed (~1 ms, jittery).
    /// Last-resort fallback when the hardware supports neither
    /// HW-timed nor ContSamps AO.
    OnDemand
};

inline const char *to_string(AOMode m) {
    switch (m) {
        case AOMode::HwTimedSinglePoint: return "hw-timed single-point";
        case AOMode::ContSampsBuffer:    return "continuous buffer (USB-friendly)";
        case AOMode::OnDemand:           return "on-demand (software-timed)";
    }
    return "?";
}

/// Abstract DAQ interface — implemented by real NI-DAQmx or mock backend
class DAQInterface {
public:
    virtual ~DAQInterface() = default;

    // --- Device enumeration ---
    virtual std::vector<DAQDeviceInfo> enumerateDevices() = 0;

    // --- Task configuration ---
    /// Configure AI: channels (comma-separated), requested sample rate.
    /// Returns coerced sample rate.
    virtual double configureAI(const std::string &channels, double sampleRate) = 0;

    /// Configure AO: channels (comma-separated).
    virtual void configureAO(const std::string &channels) = 0;

    // --- Runtime ---
    virtual void start() = 0;
    virtual void stop() = 0;

    /// Read all available AI samples. Returns number of scans read.
    /// Data is interleaved by channel (GroupByScanNumber).
    /// Buffer must be pre-allocated by caller.
    virtual int32_t readAI(double *buffer, int32_t bufferSize) = 0;

    /// Write a chunk of AO data. Buffer is `numScans × numChannels`
    /// doubles, scan-major. The backend interprets the chunk according
    /// to its current aoMode():
    ///   • HwTimedSinglePoint: queues `numScans` samples to be latched
    ///     on the next `numScans` AO clock edges.
    ///   • ContSampsBuffer:    appends `numScans` samples to the AO
    ///     FIFO; hardware drains them at the AO sample rate.
    ///   • OnDemand:           writes ONLY the last scan immediately
    ///     (earlier scans in the chunk would be overwritten before any
    ///     downstream consumer could observe them).
    virtual void writeAO(const double *data, int numScans, int numChannels) = 0;

    /// Get the number of AI channels in the current task
    virtual int numAIChannels() const = 0;

    /// Which AO timing mode the backend ended up using. Default is
    /// HwTimedSinglePoint so test backends without a configureAO
    /// implementation don't need to override this.
    virtual AOMode aoMode() const { return AOMode::HwTimedSinglePoint; }
};

#endif
