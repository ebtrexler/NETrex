#ifndef DAQ_NIDAQMX_H
#define DAQ_NIDAQMX_H

#ifdef HAVE_NIDAQMX

#include "daq_interface.h"
#include "nidaqmx.h"
#include <sstream>
#include <cstring>

/// Helper: check NI-DAQmx return code, throw on error
inline void daqmxCheck(int32 err) {
    if (err < 0) {
        char buf[2048] = {};
        DAQmxGetExtendedErrorInfo(buf, sizeof(buf));
        throw DAQException(std::string("NI-DAQmx error: ") + buf);
    }
}

class DAQNIDAQmx : public DAQInterface {
public:
    ~DAQNIDAQmx() override { stop(); }

    std::vector<DAQDeviceInfo> enumerateDevices() override {
        std::vector<DAQDeviceInfo> devices;
        char devNames[4096] = {};
        daqmxCheck(DAQmxGetSysDevNames(devNames, sizeof(devNames)));

        // Parse comma-separated device names
        std::istringstream ss(devNames);
        std::string name;
        while (std::getline(ss, name, ',')) {
            // Trim whitespace
            size_t s = name.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            name = name.substr(s, name.find_last_not_of(" \t") - s + 1);

            DAQDeviceInfo info;
            info.name = name;

            char buf[4096] = {};
            daqmxCheck(DAQmxGetDevAIPhysicalChans(name.c_str(), buf, sizeof(buf)));
            parseChannelList(buf, info.aiChannels);

            memset(buf, 0, sizeof(buf));
            daqmxCheck(DAQmxGetDevAOPhysicalChans(name.c_str(), buf, sizeof(buf)));
            parseChannelList(buf, info.aoChannels);

            daqmxCheck(DAQmxGetDevAIMaxMultiChanRate(name.c_str(), &info.maxAIRate));

            bool32 aoClkSupported = 0;
            daqmxCheck(DAQmxGetDevAOSampClkSupported(name.c_str(), &aoClkSupported));
            if (aoClkSupported)
                daqmxCheck(DAQmxGetDevAOMaxRate(name.c_str(), &info.maxAORate));

            devices.push_back(std::move(info));
        }
        return devices;
    }

    double configureAI(const std::string &channels, double sampleRate) override {
        daqmxCheck(DAQmxCreateTask("AI", &m_aiTask));
        daqmxCheck(DAQmxCreateAIVoltageChan(m_aiTask, channels.c_str(), "",
                   DAQmx_Val_RSE, -10.0, 10.0, DAQmx_Val_Volts, NULL));
        daqmxCheck(DAQmxCfgSampClkTiming(m_aiTask, "", sampleRate,
                   DAQmx_Val_Rising, DAQmx_Val_ContSamps,
                   static_cast<uInt64>(sampleRate * 10)));
        daqmxCheck(DAQmxSetReadReadAllAvailSamp(m_aiTask, true));

        // Get coerced rate
        float64 coerced = 0;
        daqmxCheck(DAQmxGetSampClkRate(m_aiTask, &coerced));
        m_sampleRate = coerced;

        uInt32 nCh = 0;
        daqmxCheck(DAQmxGetReadNumChans(m_aiTask, &nCh));
        m_numAI = static_cast<int>(nCh);

        return coerced;
    }

    void configureAO(const std::string &channels) override {
        m_aoChannels = channels;
        m_aoMode = AOMode::HwTimedSinglePoint;
        m_aoOnDemand = false;
        m_aoBufferDepthSamples = 0;
        m_aoModeDescription = to_string(m_aoMode);

        // Tier 1: hardware-timed single-point. PCIe / PXI X-series.
        if (tryConfigureHWTimedSingle(channels)) {
            m_aoMode = AOMode::HwTimedSinglePoint;
            m_aoModeDescription = to_string(m_aoMode);
            return;
        }
        // Tier 2: continuous-buffer. Most USB devices.
        if (tryConfigureContSamps(channels)) {
            m_aoMode = AOMode::ContSampsBuffer;
            m_aoModeDescription = std::string(to_string(m_aoMode)) +
                " (" + std::to_string(m_aoBufferDepthSamples) + "-scan buffer)";
            return;
        }
        // Tier 3: software-timed on-demand. Last resort.
        configureOnDemand(channels);
        m_aoMode = AOMode::OnDemand;
        m_aoOnDemand = true;
        m_aoModeDescription = to_string(m_aoMode);
    }

    void start() override {
        if (m_aoTask) daqmxCheck(DAQmxStartTask(m_aoTask));
        if (m_aiTask) daqmxCheck(DAQmxStartTask(m_aiTask));
        m_running = true;
    }

    void stop() override {
        if (!m_running) return;
        m_running = false;
        if (m_aiTask) { DAQmxStopTask(m_aiTask); DAQmxClearTask(m_aiTask); m_aiTask = 0; }
        if (m_aoTask) { DAQmxStopTask(m_aoTask); DAQmxClearTask(m_aoTask); m_aoTask = 0; }
    }

    int32_t readAI(double *buffer, int32_t bufferSize) override {
        int32 read = 0;
        daqmxCheck(DAQmxReadAnalogF64(m_aiTask, -1, 10.0,
                   DAQmx_Val_GroupByScanNumber, buffer, bufferSize, &read, NULL));
        return read;
    }

    void writeAO(const double *data, int numScans, int numChannels) override {
        int32 written = 0;
        // On-demand has no sample clock and no buffer — multiple samples
        // would just overwrite each other instantly. Take the freshest
        // (last) scan and write it.
        const double *src = data;
        int32 sampsToWrite = numScans;
        if (m_aoMode == AOMode::OnDemand && numScans > 1) {
            src = data + (numScans - 1) * numChannels;
            sampsToWrite = 1;
        }
        // For HW-timed and ContSamps modes, all numScans samples go
        // through. The hardware sample clock delivers them at exact
        // rate regardless of when we write.
        bool32 autoStart = (m_aoMode == AOMode::OnDemand) ? 1 : 0;
        daqmxCheck(DAQmxWriteAnalogF64(m_aoTask, sampsToWrite, autoStart, 10.0,
                   DAQmx_Val_GroupByScanNumber, const_cast<float64*>(src),
                   &written, NULL));
    }

    int numAIChannels() const override { return m_numAI; }

    AOMode aoMode() const override { return m_aoMode; }

    /// True if the AO task is software-timed (each writeAO goes
    /// straight to the DAC). Convenience: equivalent to
    /// `aoMode() == AOMode::OnDemand`.
    bool aoIsOnDemand() const { return m_aoMode == AOMode::OnDemand; }
    const std::string &aoModeDescription() const { return m_aoModeDescription; }
    /// Buffer depth in scans (only meaningful for ContSampsBuffer mode).
    uInt32 aoBufferDepthSamples() const { return m_aoBufferDepthSamples; }

    /// Set the AO buffer depth (in seconds) used by the ContSamps tier.
    /// Must be called BEFORE configureAO. Default is 50 ms.
    void setAOBufferDepthSeconds(double seconds) {
        m_aoBufferDepthSeconds = seconds;
    }

private:
    /// Tier 1: hardware-timed single-point AO. Each scan latched on
    /// the next sample-clock edge; sub-microsecond determinism.
    /// Returns true if the task was successfully configured.
    bool tryConfigureHWTimedSingle(const std::string &channels) {
        if (DAQmxCreateTask("AO", &m_aoTask) < 0) return false;
        if (DAQmxCreateAOVoltageChan(m_aoTask, channels.c_str(), "",
                -10.0, 10.0, DAQmx_Val_Volts, "") < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        float64 maxRate = 0;
        if (DAQmxGetSampClkMaxRate(m_aoTask, &maxRate) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        if (DAQmxCfgSampClkTiming(m_aoTask, "", maxRate,
                DAQmx_Val_Rising, DAQmx_Val_HWTimedSinglePoint, 5000) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        return true;
    }

    /// Tier 2: continuous-sample AO with a hardware-clocked FIFO. Most
    /// USB devices support this. We size the buffer for ~50 ms of
    /// samples (configurable via setAOBufferDepthSeconds()) and
    /// pre-load it with zeros so the DAC has data to play during the
    /// priming window before the first writeAO from the DAQ thread.
    bool tryConfigureContSamps(const std::string &channels) {
        if (DAQmxCreateTask("AO", &m_aoTask) < 0) return false;
        if (DAQmxCreateAOVoltageChan(m_aoTask, channels.c_str(), "",
                -10.0, 10.0, DAQmx_Val_Volts, "") < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        float64 maxRate = 0;
        if (DAQmxGetSampClkMaxRate(m_aoTask, &maxRate) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        // Use a sane default rate — match AI rate when possible. Falls
        // back to the device's max if AI hasn't been configured yet.
        const double rate = (m_sampleRate > 0) ? m_sampleRate : maxRate;
        const uInt64 bufScans = static_cast<uInt64>(
            rate * m_aoBufferDepthSeconds);

        if (DAQmxCfgSampClkTiming(m_aoTask, "", rate,
                DAQmx_Val_Rising, DAQmx_Val_ContSamps, bufScans) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        if (DAQmxCfgOutputBuffer(m_aoTask, bufScans) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        // Pre-load with zeros so the DAC starts playing 0 V immediately
        // and the buffer doesn't underflow before the first writeAO
        // from DaqThread arrives.
        uInt32 nCh = 0;
        DAQmxGetTaskNumChans(m_aoTask, &nCh);
        std::vector<double> zeros(static_cast<std::size_t>(bufScans) * nCh, 0.0);
        int32 written = 0;
        if (DAQmxWriteAnalogF64(m_aoTask, static_cast<int32>(bufScans),
                /*autoStart=*/0, 10.0, DAQmx_Val_GroupByScanNumber,
                zeros.data(), &written, NULL) < 0) {
            DAQmxClearTask(m_aoTask); m_aoTask = 0; return false;
        }
        m_aoBufferDepthSamples = static_cast<uInt32>(bufScans);
        return true;
    }

    /// Tier 3: software-timed on-demand AO. Each writeAO bypasses any
    /// sample clock and reaches the DAC over USB directly. Last-resort
    /// fallback. Throws on failure (we've already exhausted alternatives).
    void configureOnDemand(const std::string &channels) {
        daqmxCheck(DAQmxCreateTask("AO", &m_aoTask));
        daqmxCheck(DAQmxCreateAOVoltageChan(m_aoTask, channels.c_str(), "",
                   -10.0, 10.0, DAQmx_Val_Volts, ""));
        // No CfgSampClkTiming call — task will be on-demand.
    }

    static void parseChannelList(const char *text, std::vector<std::string> &out) {
        std::istringstream ss(text);
        std::string ch;
        while (std::getline(ss, ch, ',')) {
            size_t s = ch.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            out.push_back(ch.substr(s, ch.find_last_not_of(" \t") - s + 1));
        }
    }

    TaskHandle m_aiTask = 0;
    TaskHandle m_aoTask = 0;
    double m_sampleRate = 0;
    int m_numAI = 0;
    bool m_running = false;

    // AO mode tracking (set by configureAO's three-tier resolution).
    AOMode m_aoMode = AOMode::HwTimedSinglePoint;
    bool m_aoOnDemand = false;
    std::string m_aoModeDescription = "hw-timed single-point";
    std::string m_aoChannels;
    // Buffer depth for ContSamps mode. 50 ms is a balance between
    // robustness to OS hiccups and dynamic-clamp latency. Tune via
    // setAOBufferDepthSeconds() before configureAO if needed.
    double m_aoBufferDepthSeconds = 0.050;
    uInt32 m_aoBufferDepthSamples = 0;
};

#endif // HAVE_NIDAQMX
#endif
