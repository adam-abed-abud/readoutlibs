/**
 * @file SourceEmulatorModel.hpp Emulates a source with given raw type
 *
 * This is part of the DUNE DAQ , copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */
#ifndef READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_SOURCEEMULATORMODEL_HPP_
#define READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_SOURCEEMULATORMODEL_HPP_

#include "appfwk/DAQSink.hpp"
#include "appfwk/DAQSource.hpp"

#include "logging/Logging.hpp"

#include "opmonlib/InfoCollector.hpp"

#include "readoutlibs/sourceemulatorconfig/Nljs.hpp"
#include "readoutlibs/sourceemulatorinfo/InfoNljs.hpp"

#include "readoutlibs/ReadoutIssues.hpp"
#include "readoutlibs/concepts/SourceEmulatorConcept.hpp"
#include "readoutlibs/utils/ErrorBitGenerator.hpp"
#include "readoutlibs/utils/FileSourceBuffer.hpp"
#include "readoutlibs/utils/RateLimiter.hpp"
#include "readoutlibs/utils/ReusableThread.hpp"

#include "unistd.h"
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using dunedaq::readoutlibs::logging::TLVL_TAKE_NOTE;
using dunedaq::readoutlibs::logging::TLVL_WORK_STEPS;

namespace dunedaq {
namespace readoutlibs {

template<class ReadoutType>
class SourceEmulatorModel : public SourceEmulatorConcept
{
public:
  using sink_t = appfwk::DAQSink<ReadoutType>;

  explicit SourceEmulatorModel(std::string name,
                               std::atomic<bool>& run_marker,
                               uint64_t time_tick_diff, // NOLINT(build/unsigned)
                               double dropout_rate,
                               double frame_error_rate,
                               double rate_khz)
    : m_run_marker(run_marker)
    , m_time_tick_diff(time_tick_diff)
    , m_dropout_rate(dropout_rate)
    , m_frame_error_rate(frame_error_rate)
    , m_packet_count{ 0 }
    , m_sink_queue_timeout_ms(0)
    , m_raw_data_sink(nullptr)
    , m_producer_thread(0)
    , m_name(name)
    , m_rate_khz(rate_khz)
  {}

  void init(const nlohmann::json& /*args*/) {}

  void set_sink(const std::string& sink_name)
  {
    if (!m_sink_is_set) {
      m_raw_data_sink = std::make_unique<sink_t>(sink_name);
      m_sink_is_set = true;
    } else {
      // ers::error();
    }
  }

  void conf(const nlohmann::json& args, const nlohmann::json& link_conf)
  {
    if (m_is_configured) {
      TLOG_DEBUG(TLVL_WORK_STEPS) << "This emulator is already configured!";
    } else {
      m_conf = args.get<module_conf_t>();
      m_link_conf = link_conf.get<link_conf_t>();
      m_sink_queue_timeout_ms = std::chrono::milliseconds(m_conf.queue_timeout_ms);

      std::mt19937 mt(rand()); // NOLINT(runtime/threadsafe_fn)
      std::uniform_real_distribution<double> dis(0.0, 1.0);

      m_geoid.element_id = m_link_conf.geoid.element;
      m_geoid.region_id = m_link_conf.geoid.region;
      m_geoid.system_type = ReadoutType::system_type;

      m_file_source = std::make_unique<FileSourceBuffer>(m_link_conf.input_limit, sizeof(ReadoutType));
      try {
        m_file_source->read(m_link_conf.data_filename);
      } catch (const ers::Issue& ex) {
        ers::fatal(ex);
        throw ConfigurationError(ERS_HERE, m_geoid, "", ex);
      }
      m_dropouts_length = m_link_conf.random_population_size;
      if (m_dropout_rate == 0.0) {
        m_dropouts = std::vector<bool>(1);
      } else {
        m_dropouts = std::vector<bool>(m_dropouts_length);
      }
      for (size_t i = 0; i < m_dropouts.size(); ++i) {
        m_dropouts[i] = dis(mt) >= m_dropout_rate;
      }

      m_frame_errors_length = m_link_conf.random_population_size;
      m_frame_error_rate = m_link_conf.emu_frame_error_rate;
      m_error_bit_generator = ErrorBitGenerator(m_frame_error_rate);
      m_error_bit_generator.generate();

      m_is_configured = true;
    }
    // Configure thread:
    m_producer_thread.set_name("fakeprod", m_link_conf.geoid.element);
  }

  void scrap(const nlohmann::json& /*args*/)
  {
    m_file_source.reset();
    m_is_configured = false;
  }

  bool is_configured() override { return m_is_configured; }

  void start(const nlohmann::json& /*args*/)
  {
    m_packet_count_tot = 0;
    TLOG_DEBUG(TLVL_WORK_STEPS) << "Starting threads...";
    m_rate_limiter = std::make_unique<RateLimiter>(m_rate_khz / m_link_conf.slowdown);
    // m_stats_thread.set_work(&SourceEmulatorModel<ReadoutType>::run_stats, this);
    m_producer_thread.set_work(&SourceEmulatorModel<ReadoutType>::run_produce, this);
  }

  void stop(const nlohmann::json& /*args*/)
  {
    while (!m_producer_thread.get_readiness()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void get_info(opmonlib::InfoCollector& ci, int /*level*/)
  {
    sourceemulatorinfo::Info info;
    info.packets = m_packet_count_tot.load();
    info.new_packets = m_packet_count.exchange(0);

    ci.add(info);
  }

protected:
  void run_produce()
  {
    TLOG_DEBUG(TLVL_WORK_STEPS) << "Data generation thread " << m_this_link_number << " started";

    // pthread_setname_np(pthread_self(), get_name().c_str());

    uint offset = 0; // NOLINT(build/unsigned)
    auto& source = m_file_source->get();

    int num_elem = m_file_source->num_elements();
    if (num_elem == 0) {
      TLOG_DEBUG(TLVL_WORK_STEPS) << "No elements to read from buffer! Sleeping...";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      num_elem = m_file_source->num_elements();
    }

    auto rptr = reinterpret_cast<ReadoutType*>(source.data()); // NOLINT

    // set the initial timestamp to a configured value, otherwise just use the timestamp from the header
    uint64_t ts_0 = (m_conf.set_t0_to >= 0) ? m_conf.set_t0_to : rptr->get_first_timestamp(); // NOLINT(build/unsigned)
    TLOG_DEBUG(TLVL_BOOKKEEPING) << "First timestamp in the source file: " << ts_0;
    uint64_t timestamp = ts_0; // NOLINT(build/unsigned)
    int dropout_index = 0;

    while (m_run_marker.load()) {
      // Which element to push to the buffer
      if (offset == num_elem * sizeof(ReadoutType) || (offset + 1) * sizeof(ReadoutType) > source.size()) {
        offset = 0;
      }

      bool create_frame = m_dropouts[dropout_index]; // NOLINT(runtime/threadsafe_fn)
      dropout_index = (dropout_index + 1) % m_dropouts.size();
      if (create_frame) {
        ReadoutType payload;
        // Memcpy from file buffer to flat char array
        ::memcpy(static_cast<void*>(&payload),
                 static_cast<void*>(source.data() + offset * sizeof(ReadoutType)),
                 sizeof(ReadoutType));

        // Fake timestamp
        payload.fake_timestamps(timestamp, m_time_tick_diff);

        // Introducing frame errors
        std::vector<uint16_t> frame_errs; // NOLINT(build/unsigned)
        for (size_t i = 0; i < rptr->get_num_frames(); ++i) {
          frame_errs.push_back(m_error_bit_generator.next());
        }
        payload.fake_frame_errors(&frame_errs);

        // queue in to actual DAQSink
        try {
          m_raw_data_sink->push(std::move(payload), m_sink_queue_timeout_ms);
        } catch (ers::Issue& excpt) {
          ers::warning(CannotWriteToQueue(ERS_HERE, m_geoid, "raw data input queue", excpt));
          // std::runtime_error("Queue timed out...");
        }

        // Count packet and limit rate if needed.
        ++offset;
        ++m_packet_count;
        ++m_packet_count_tot;
      }

      timestamp += m_time_tick_diff * 12;

      m_rate_limiter->limit();
    }
    TLOG_DEBUG(TLVL_WORK_STEPS) << "Data generation thread " << m_this_link_number << " finished";
  }

private:
  // Constuctor params
  std::atomic<bool>& m_run_marker;

  // CONFIGURATION
  uint32_t m_this_apa_number;  // NOLINT(build/unsigned)
  uint32_t m_this_link_number; // NOLINT(build/unsigned)

  uint64_t m_time_tick_diff; // NOLINT(build/unsigned)
  double m_dropout_rate;
  double m_frame_error_rate;

  // STATS
  std::atomic<int> m_packet_count{ 0 };
  std::atomic<int> m_packet_count_tot{ 0 };

  sourceemulatorconfig::Conf m_cfg;

  // RAW SINK
  std::chrono::milliseconds m_sink_queue_timeout_ms;
  using raw_sink_qt = appfwk::DAQSink<ReadoutType>;
  std::unique_ptr<raw_sink_qt> m_raw_data_sink;

  bool m_sink_is_set = false;
  using module_conf_t = dunedaq::readoutlibs::sourceemulatorconfig::Conf;
  module_conf_t m_conf;
  using link_conf_t = dunedaq::readoutlibs::sourceemulatorconfig::LinkConfiguration;
  link_conf_t m_link_conf;

  std::unique_ptr<RateLimiter> m_rate_limiter;
  std::unique_ptr<FileSourceBuffer> m_file_source;
  ErrorBitGenerator m_error_bit_generator;

  ReusableThread m_producer_thread;

  std::string m_name;
  bool m_is_configured = false;
  double m_rate_khz;

  std::vector<bool> m_dropouts; // Random population
  std::vector<bool> m_frame_errors;

  uint m_dropouts_length = 10000; // NOLINT(build/unsigned) Random population size
  uint m_frame_errors_length = 10000;
  daqdataformats::GeoID m_geoid;
};

} // namespace readoutlibs
} // namespace dunedaq

#endif // READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_SOURCEEMULATORMODEL_HPP_
