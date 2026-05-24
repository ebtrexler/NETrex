// core/daq/daq_thread.cpp

#include "daq_thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "RT_Network.h"

// ---------------------------------------------------------------------------
DaqThread::DaqThread(Config cfg) : m_cfg(cfg) {}

DaqThread::~DaqThread() {
    stop();
}

bool DaqThread::start() {
    if (m_started.load()) return false;

    if (!m_cfg.daq || !m_cfg.net) {
        set_error("DaqThread::start: daq and net must be non-null");
        return false;
    }
    if (m_cfg.sample_rate_hz <= 0) {
        set_error("DaqThread::start: sample_rate_hz must be > 0");
        return false;
    }
    if (m_cfg.num_ai_channels <= 0 || m_cfg.num_cells <= 0) {
        set_error("DaqThread::start: num_ai_channels and num_cells must be > 0");
        return false;
    }

    m_sample_rate = m_cfg.sample_rate_hz;
    m_step_ms     = 1000.0 / m_sample_rate;
    m_scan_size   = m_cfg.num_ai_channels +
                    m_cfg.num_cells +
                    std::max(0, m_cfg.num_vdep_cells);

    // Pre-allocate all hot-loop buffers.
    m_ai_buf.assign(m_cfg.max_scans_per_read * m_cfg.num_ai_channels, 0.0);
    m_vm_out.assign(m_cfg.num_cells, 0.0);
    m_i_na.assign(std::max(1, m_cfg.num_vdep_cells), 0.0);
    m_zero_ao.assign(std::max(1, m_cfg.num_vdep_cells), 0.0);
    m_ao_chunk.assign(
        m_cfg.max_scans_per_read * std::max(1, m_cfg.num_vdep_cells), 0.0);

    // Start the DAQ tasks. If this throws, report and bail.
    try {
        m_cfg.daq->start();
    } catch (const std::exception& e) {
        set_error(std::string("daq->start: ") + e.what());
        return false;
    }

    // Prime the DAC with zeros so the amplifier is held at 0 nA on entry.
    try {
        m_cfg.daq->writeAO(m_zero_ao.data(), 1,
                           std::max(1, m_cfg.num_vdep_cells));
    } catch (const std::exception&) {
        // Not fatal — some mock backends don't implement priming. The
        // first real writeAO in run() will still happen.
    }

    m_stopping.store(false);
    m_ok.store(true);
    m_done.store(false);
    m_scans_acquired.store(0);
    m_chunks_processed.store(0);
    m_disk_rejects.store(0);

    m_thread = std::thread(&DaqThread::run, this);
    m_started.store(true);
    return true;
}

void DaqThread::stop() {
    if (!m_started.load()) return;

    m_stopping.store(true);
    if (m_thread.joinable()) m_thread.join();

    // Drive AO to zero on shutdown so the amplifier doesn't keep
    // injecting current after the run ends.
    try {
        m_cfg.daq->writeAO(m_zero_ao.data(), 1,
                           std::max(1, m_cfg.num_vdep_cells));
    } catch (...) {}

    try {
        m_cfg.daq->stop();
    } catch (...) {}

    m_started.store(false);
}

std::string DaqThread::last_error() const {
    std::lock_guard<std::mutex> g(m_err_mu);
    return m_last_error;
}

// ---------------------------------------------------------------------------
// Hot loop. Runs on its own thread at elevated priority (best-effort).
// ---------------------------------------------------------------------------
void DaqThread::run() {
    // Elevate priority inside the thread — never on the caller's thread.
    {
        auto report = rt_priority::elevate_current_thread();
        std::lock_guard<std::mutex> g(m_err_mu);
        m_rt_report = report;
    }

    // Pre-compute chunk ingest for the scan-assembly buffer so we don't
    // allocate in the hot loop.
    std::vector<double> scan_chunk;
    scan_chunk.reserve(m_cfg.max_scans_per_read * m_scan_size);

    const int32_t ai_buf_size = static_cast<int32_t>(m_ai_buf.size());
    const int32_t num_ai = m_cfg.num_ai_channels;
    const int32_t num_vdep = std::max(1, m_cfg.num_vdep_cells);

    while (!m_stopping.load(std::memory_order_acquire)) {
        int32_t scans = 0;
        try {
            scans = m_cfg.daq->readAI(m_ai_buf.data(), ai_buf_size);
        } catch (const std::exception& e) {
            set_error(std::string("readAI: ") + e.what());
            m_ok.store(false);
            break;
        }

        if (scans <= 0) {
            // No samples available yet — brief yield and retry. This
            // matches DAQMock's behavior where readAI returns 0 when
            // wall-clock hasn't advanced enough. On real hardware,
            // readAI blocks, so this branch rarely runs.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // Pointer to the last acquired scan — TNetwork::Update only
        // looks at the current Vm snapshot, consistent with legacy
        // RunDialog::daqStep behavior.
        double* last_sample = &m_ai_buf[(scans - 1) * num_ai];

        // Integrate the network over the chunk. The sub-stepping inside
        // RK4 is driven by TNetwork::GetMaxRK4Timestep().
        try {
            m_cfg.net->Update(scans * m_step_ms,
                              last_sample,
                              m_vm_out.data(),
                              m_i_na.data());
        } catch (const std::exception& e) {
            set_error(std::string("net->Update: ") + e.what());
            m_ok.store(false);
            break;
        }

        // Phase logic: before/after zero the DAC command; during hands
        // through the network's I_nA (after clipping).
        const uint64_t prev_scans = m_scans_acquired.load(std::memory_order_relaxed);
        const double sec_end = static_cast<double>(prev_scans + scans) / m_sample_rate;
        const bool in_during = (sec_end > m_cfg.sec_before) &&
                               (sec_end <= m_cfg.sec_before + m_cfg.sec_during);

        const double* ao_out = m_zero_ao.data();
        if (in_during) {
            for (int i = 0; i < m_cfg.num_vdep_cells; ++i) {
                double v = m_i_na[i];
                if (v < m_cfg.ao_clip_min) v = m_cfg.ao_clip_min;
                if (v > m_cfg.ao_clip_max) v = m_cfg.ao_clip_max;
                m_i_na[i] = v;
            }
            ao_out = m_i_na.data();
        }

        // Replicate the per-chunk AO command across all `scans` samples
        // and write the whole chunk. The backend interprets this:
        //   • HW-timed single-point: queues N samples for N clock edges
        //   • ContSamps buffer: appends N samples to the FIFO; hardware
        //     drains them at the AO sample rate (cont-buff USB path)
        //   • On-demand: writes only the last sample, immediately
        for (int32_t s = 0; s < scans; ++s) {
            std::memcpy(&m_ao_chunk[static_cast<std::size_t>(s) * num_vdep],
                        ao_out,
                        num_vdep * sizeof(double));
        }

        try {
            m_cfg.daq->writeAO(m_ao_chunk.data(), scans, num_vdep);
        } catch (const std::exception& e) {
            set_error(std::string("writeAO: ") + e.what());
            m_ok.store(false);
            break;
        }

        // Assemble the scan chunk and submit to disk (guaranteed
        // delivery). If the writer is full, the run aborts.
        if (m_cfg.writer) {
            scan_chunk.clear();
            scan_chunk.resize(static_cast<std::size_t>(scans) * m_scan_size);
            assemble_scan_chunk(m_ai_buf.data(), scans,
                                m_vm_out.data(), m_i_na.data(),
                                scan_chunk);

            ScanWriter::Chunk ch(std::move(scan_chunk),
                                 static_cast<uint32_t>(scans));
            if (!m_cfg.writer->submit(std::move(ch))) {
                m_disk_rejects.fetch_add(1, std::memory_order_relaxed);
                set_error("disk writer full or failed");
                m_ok.store(false);
                break;
            }
            // scan_chunk was moved from; re-acquire a fresh buffer next
            // iteration. `clear() + resize()` does not re-allocate as
            // long as capacity is preserved, but move transferred our
            // capacity too. Reserve again so the next iteration's
            // resize is still allocation-free.
            scan_chunk = std::vector<double>();
            scan_chunk.reserve(static_cast<std::size_t>(m_cfg.max_scans_per_read) *
                               m_scan_size);
        }

        // UI fanout. Invoked with time_ms_start = start of this chunk.
        if (m_cfg.on_chunk) {
            const double time_ms_start =
                static_cast<double>(prev_scans) * m_step_ms;
            m_cfg.on_chunk(m_cfg.on_chunk_ctx,
                           time_ms_start,
                           scans,
                           m_ai_buf.data(),
                           m_vm_out.data(),
                           m_i_na.data());
        }

        m_scans_acquired.fetch_add(static_cast<uint64_t>(scans),
                                   std::memory_order_release);
        m_chunks_processed.fetch_add(1, std::memory_order_release);
    }

    m_done.store(true, std::memory_order_release);
}

void DaqThread::assemble_scan_chunk(
    const double* ai_interleaved,
    int32_t num_scans,
    const double* vm_out,
    const double* i_na,
    std::vector<double>& out) const
{
    const int32_t num_ai   = m_cfg.num_ai_channels;
    const int32_t num_cell = m_cfg.num_cells;
    const int32_t num_vdep = m_cfg.num_vdep_cells;

    // Layout per scan: [ai..., vm_out..., i_na...]
    for (int32_t s = 0; s < num_scans; ++s) {
        double* row = out.data() + static_cast<std::size_t>(s) * m_scan_size;
        // AI: per-scan, time-resolved
        for (int32_t ch = 0; ch < num_ai; ++ch) {
            row[ch] = ai_interleaved[s * num_ai + ch];
        }
        // Vm_out and I_nA: end-of-chunk state, duplicated across scans
        double* vm_slot = row + num_ai;
        for (int32_t i = 0; i < num_cell; ++i) vm_slot[i] = vm_out[i];
        double* i_slot = vm_slot + num_cell;
        for (int32_t i = 0; i < num_vdep; ++i) i_slot[i] = i_na[i];
    }
}

void DaqThread::set_error(std::string msg) {
    std::lock_guard<std::mutex> g(m_err_mu);
    if (m_last_error.empty()) m_last_error = std::move(msg);
}
