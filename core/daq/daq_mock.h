#ifndef DAQ_MOCK_H
#define DAQ_MOCK_H

#include "daq_interface.h"
#include "RT_Network.h"
#include "RT_ModelCell.h"
#include "RT_InjectionElectrode.h"
#include "RT_HHCurrent.h"
#include <cmath>
#include <chrono>
#include <memory>

/// Mock DAQ backend that uses a TModelCell to simulate a biological cell.
/// AI reads return the model cell's Vm (as Volts). AO writes inject current.
/// Timing simulates real sample accumulation based on wall-clock elapsed time.
class DAQMock : public DAQInterface {
public:
    DAQMock() = default;
    ~DAQMock() override { stop(); }

    std::vector<DAQDeviceInfo> enumerateDevices() override {
        DAQDeviceInfo dev;
        dev.name = "MockDev1";
        dev.maxAIRate = 100000;
        dev.maxAORate = 100000;
        for (int i = 0; i < 8; ++i) {
            dev.aiChannels.push_back("MockDev1/ai" + std::to_string(i));
            dev.aoChannels.push_back("MockDev1/ao" + std::to_string(i));
        }
        return {dev};
    }

    double configureAI(const std::string &channels, double sampleRate) override {
        m_sampleRate = sampleRate;
        m_numAI = 1;
        for (char c : channels) if (c == ',') m_numAI++;
        return sampleRate;
    }

    void configureAO(const std::string &channels) override {
        m_numAO = 1;
        for (char c : channels) if (c == ',') m_numAO++;
    }

    void start() override {
        m_running = true;
        m_lastRead = std::chrono::steady_clock::now();

        if (!m_cell) {
            // Build a spiking HH model cell with an injection electrode for current input
            m_cell = std::make_unique<TModelCell>(L"MockBiological");
            auto *hh = new THHCurrent(m_cell.get(), L"HH_mock");
            m_cell->AddCurrent(hh);
            m_electrode = new TInjectionElectrode(m_cell.get(), L"DAC_inject");
            m_electrode->SetAmplitude(0);
            m_electrode->SetDelay(0);
            m_electrode->SetDuration(1e9);
            m_electrode->SetInitDelay(0);
            m_electrode->SetNumRepeats(-1);
            m_cell->AddElectrode(m_electrode);
            m_cell->Initialize(true);
        }
    }

    void stop() override { m_running = false; }

    int32_t readAI(double *buffer, int32_t bufferSize) override {
        if (!m_running) return 0;

        auto now = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(now - m_lastRead).count();

        // Compute samples whose wall-clock "arrival time" has elapsed.
        // Unlike the old code (std::max(1, ...)), we return 0 when not
        // enough real time has passed. This matches the blocking
        // behavior of the real NI-DAQmx readAI() more closely and keeps
        // the producer thread from tight-spinning against the mock.
        int32_t samplesAvail = static_cast<int32_t>(elapsed_s * m_sampleRate);
        if (samplesAvail <= 0) return 0;

        m_lastRead = now;

        int32_t maxScans = bufferSize / m_numAI;
        int32_t scans = std::min(samplesAvail, maxScans);

        double step_ms = 1000.0 / m_sampleRate;
        for (int32_t i = 0; i < scans; ++i) {
            // Update currents then integrate
            m_cell->Update(step_ms);
            double Vm = m_cell->CalcVm(step_ms);

            // First AI channel = Vm in Volts (Vm_mV / 1000)
            buffer[i * m_numAI] = Vm * 0.001;
            for (int ch = 1; ch < m_numAI; ++ch)
                buffer[i * m_numAI + ch] = 0.0;
        }
        return scans;
    }

    void writeAO(const double *data, int numScans, int numChannels) override {
        if (!m_running || !m_electrode || numChannels < 1) return;
        // Mock pretends to be a perfect HW-timed device. Use the latest
        // scan in the chunk as the new injected current — the previous
        // scans would have been latched on previous clock edges, but
        // since we only simulate one cell with one electrode the
        // current observable is just "what was injected last."
        const double *last = (numScans > 0) ? &data[(numScans - 1) * numChannels] : data;
        m_electrode->SetAmplitude(last[0]);
    }

    int numAIChannels() const override { return m_numAI; }

    /// Access the internal model cell for configuration (e.g. add more currents)
    TModelCell *modelCell() { return m_cell.get(); }

private:
    double m_sampleRate = 10000;
    int m_numAI = 1;
    int m_numAO = 1;
    bool m_running = false;
    std::chrono::steady_clock::time_point m_lastRead;
    std::unique_ptr<TModelCell> m_cell;
    TInjectionElectrode *m_electrode = nullptr; // owned by m_cell
};

#endif
