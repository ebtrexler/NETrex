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
        daqmxCheck(DAQmxCreateTask("AO", &m_aoTask));
        daqmxCheck(DAQmxCreateAOVoltageChan(m_aoTask, channels.c_str(), "",
                   -10.0, 10.0, DAQmx_Val_Volts, ""));

        // Try hardware-timed single-point AO. Each DAQmxWriteAnalogF64 is
        // latched on the next sample-clock edge — deterministic per-scan
        // output, ideal for dynamic clamp. PCIe / PXI X-series devices
        // support this; most USB devices (USB-6001, USB-6008, USB-6009,
        // USB-6210 family, etc.) do NOT and the driver returns an error.
        // We catch that and fall back to on-demand (software-timed) AO.
        m_aoOnDemand = false;
        m_aoModeDescription = "hw-timed single-point";

        float64 maxRate = 0;
        int32 err = DAQmxGetSampClkMaxRate(m_aoTask, &maxRate);
        if (err >= 0) {
            err = DAQmxCfgSampClkTiming(m_aoTask, "", maxRate,
                  DAQmx_Val_Rising, DAQmx_Val_HWTimedSinglePoint, 5000);
        }
        if (err < 0) {
            // Hardware doesn't support HW-timed single point. Tear down
            // and recreate the task without sample-clock timing — each
            // writeAO will then go straight to the DAC over USB. Higher
            // latency (~1 ms per write on USB) but functionally correct.
            DAQmxClearTask(m_aoTask);
            m_aoTask = 0;
            daqmxCheck(DAQmxCreateTask("AO", &m_aoTask));
            daqmxCheck(DAQmxCreateAOVoltageChan(m_aoTask, channels.c_str(), "",
                       -10.0, 10.0, DAQmx_Val_Volts, ""));
            m_aoOnDemand = true;
            m_aoModeDescription = "on-demand (USB / software-timed)";
        }
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

    void writeAO(const double *data, int numChannels) override {
        int32 written = 0;
        // On-demand tasks tolerate (and benefit from) autoStart=true:
        // it transitions the task to running on the first write if it
        // wasn't already, with no harmful effect on subsequent writes.
        bool32 autoStart = m_aoOnDemand ? 1 : 0;
        daqmxCheck(DAQmxWriteAnalogF64(m_aoTask, 1, autoStart, 10.0,
                   DAQmx_Val_GroupByScanNumber, const_cast<float64*>(data),
                   &written, NULL));
    }

    int numAIChannels() const override { return m_numAI; }

    /// True if the AO task is software-timed (each writeAO goes
    /// straight to the DAC). Used by RunDialog to surface the mode in
    /// the diagnostics block.
    bool aoIsOnDemand() const { return m_aoOnDemand; }
    const std::string &aoModeDescription() const { return m_aoModeDescription; }

private:
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
    bool m_aoOnDemand = false;
    std::string m_aoModeDescription = "hw-timed single-point";
};

#endif // HAVE_NIDAQMX
#endif
