/**
 * @file RecorderImpl.hpp Templated recorder implementation
 *
 * This is part of the DUNE DAQ , copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */
#ifndef READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_RECORDERMODEL_HPP_
#define READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_RECORDERMODEL_HPP_

#include "appfwk/DAQModuleHelper.hpp"
#include "appfwk/DAQSource.hpp"
#include "utilities/WorkerThread.hpp"
#include "readoutlibs/ReadoutTypes.hpp"
#include "readoutlibs/concepts/RecorderConcept.hpp"
#include "readoutlibs/recorderconfig/Nljs.hpp"
#include "readoutlibs/recorderconfig/Structs.hpp"
#include "readoutlibs/recorderinfo/InfoStructs.hpp"
#include "readoutlibs/utils/BufferedFileWriter.hpp"
#include "readoutlibs/utils/ReusableThread.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace dunedaq {
namespace readoutlibs {
template<class ReadoutType>
class RecorderImpl : public RecorderConcept
{
public:
  explicit RecorderImpl(std::string name)
    : m_work_thread(0)
    , m_name(name)
  {}

  void init(const nlohmann::json& args) override
  {
    try {
      auto qi = appfwk::queue_index(args, { "raw_recording" });
      m_input_queue.reset(new source_t(qi["raw_recording"].inst));
    } catch (const ers::Issue& excpt) {
      throw ResourceQueueError(ERS_HERE, "raw_recording", "RecorderModel");
    }
  }

  void get_info(opmonlib::InfoCollector& ci, int /* level */) override
  {
    recorderinfo::Info info;
    info.packets_processed = m_packets_processed_total;
    double time_diff = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() -
                                                                                 m_time_point_last_info)
                         .count();
    info.throughput_processed_packets = m_packets_processed_since_last_info / time_diff;

    ci.add(info);

    m_packets_processed_since_last_info = 0;
    m_time_point_last_info = std::chrono::steady_clock::now();
  }

  void do_conf(const nlohmann::json& args) override
  {
    m_conf = args.get<recorderconfig::Conf>();
    std::string output_file = m_conf.output_file;
    if (remove(output_file.c_str()) == 0) {
      TLOG(TLVL_WORK_STEPS) << "Removed existing output file from previous run" << std::endl;
    }

    m_buffered_writer.open(
      m_conf.output_file, m_conf.stream_buffer_size, m_conf.compression_algorithm, m_conf.use_o_direct);
    m_work_thread.set_name(m_name, 0);
  }

  void do_scrap(const nlohmann::json& /*args*/) override { m_buffered_writer.close(); }

  void do_start(const nlohmann::json& /* args */) override
  {
    m_packets_processed_total = 0;
    m_run_marker.store(true);
    m_work_thread.set_work(&RecorderImpl::do_work, this);
  }

  void do_stop(const nlohmann::json& /* args */) override
  {
    m_run_marker.store(false);
    while (!m_work_thread.get_readiness()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

private:
  void do_work()
  {
    m_time_point_last_info = std::chrono::steady_clock::now();

    ReadoutType element;
    while (m_run_marker) {
      try {
        m_input_queue->pop(element, std::chrono::milliseconds(100));
        m_packets_processed_total++;
        m_packets_processed_since_last_info++;
        if (!m_buffered_writer.write(reinterpret_cast<char*>(&element), sizeof(element))) { // NOLINT
          ers::warning(CannotWriteToFile(ERS_HERE, m_conf.output_file));
          break;
        }
      } catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt) {
        continue;
      }
    }
    m_buffered_writer.flush();
  }

  // Queue
  using source_t = dunedaq::appfwk::DAQSource<ReadoutType>;
  std::unique_ptr<source_t> m_input_queue;

  // Internal
  recorderconfig::Conf m_conf;
  BufferedFileWriter<> m_buffered_writer;

  // Threading
  ReusableThread m_work_thread;
  std::atomic<bool> m_run_marker;

  // Stats
  std::atomic<int> m_packets_processed_total{ 0 };
  std::atomic<int> m_packets_processed_since_last_info{ 0 };
  std::chrono::steady_clock::time_point m_time_point_last_info;

  std::string m_name;
};
} // namespace readoutlibs
} // namespace dunedaq

#endif // READOUTLIBS_INCLUDE_READOUTLIBS_MODELS_RECORDERMODEL_HPP_
