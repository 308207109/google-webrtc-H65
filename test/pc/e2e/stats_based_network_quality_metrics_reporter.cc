/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/pc/e2e/stats_based_network_quality_metrics_reporter.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats.h"
#include "api/stats/rtcstats_objects.h"
#include "api/test/metrics/metric.h"
#include "api/test/network_emulation/network_emulation_interfaces.h"
#include "api/test/network_emulation_manager.h"
#include "api/units/data_rate.h"
#include "api/units/timestamp.h"
#include "rtc_base/checks.h"
#include "rtc_base/event.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/synchronization/mutex.h"
#include "system_wrappers/include/field_trial.h"
#include "test/pc/e2e/metric_metadata_keys.h"

namespace webrtc {
namespace webrtc_pc_e2e {
namespace {

using ::webrtc::test::ImprovementDirection;
using ::webrtc::test::Unit;

constexpr TimeDelta kStatsWaitTimeout = TimeDelta::Seconds(1);

// Field trial which controls whether to report standard-compliant bytes
// sent/received per stream.  If enabled, padding and headers are not included
// in bytes sent or received.
constexpr char kUseStandardBytesStats[] = "WebRTC-UseStandardBytesStats";

std::unique_ptr<EmulatedNetworkStats> PopulateStats(
    std::vector<EmulatedEndpoint*> endpoints,
    NetworkEmulationManager* network_emulation) {
  rtc::Event stats_loaded;
  std::unique_ptr<EmulatedNetworkStats> stats;
  network_emulation->GetStats(endpoints,
                              [&](std::unique_ptr<EmulatedNetworkStats> s) {
                                stats = std::move(s);
                                stats_loaded.Set();
                              });
  bool stats_received = stats_loaded.Wait(kStatsWaitTimeout);
  RTC_CHECK(stats_received);
  return stats;
}

std::map<rtc::IPAddress, std::string> PopulateIpToPeer(
    const std::map<std::string, std::vector<EmulatedEndpoint*>>&
        peer_endpoints) {
  std::map<rtc::IPAddress, std::string> out;
  for (const auto& entry : peer_endpoints) {
    for (const EmulatedEndpoint* const endpoint : entry.second) {
      RTC_CHECK(out.find(endpoint->GetPeerLocalAddress()) == out.end())
          << "Two peers can't share the same endpoint";
      out.emplace(endpoint->GetPeerLocalAddress(), entry.first);
    }
  }
  return out;
}

}  // namespace

StatsBasedNetworkQualityMetricsReporter::
    StatsBasedNetworkQualityMetricsReporter(
        std::map<std::string, std::vector<EmulatedEndpoint*>> peer_endpoints,
        NetworkEmulationManager* network_emulation,
        test::MetricsLogger* metrics_logger)
    : collector_(std::move(peer_endpoints), network_emulation),
      clock_(network_emulation->time_controller()->GetClock()),
      metrics_logger_(metrics_logger) {
  RTC_CHECK(metrics_logger_);
}

StatsBasedNetworkQualityMetricsReporter::NetworkLayerStatsCollector::
    NetworkLayerStatsCollector(
        std::map<std::string, std::vector<EmulatedEndpoint*>> peer_endpoints,
        NetworkEmulationManager* network_emulation)
    : peer_endpoints_(std::move(peer_endpoints)),
      ip_to_peer_(PopulateIpToPeer(peer_endpoints_)),
      network_emulation_(network_emulation) {}

void StatsBasedNetworkQualityMetricsReporter::NetworkLayerStatsCollector::
    Start() {
  MutexLock lock(&mutex_);
  // Check that network stats are clean before test execution.
  for (const auto& entry : peer_endpoints_) {
    std::unique_ptr<EmulatedNetworkStats> stats =
        PopulateStats(entry.second, network_emulation_);
    RTC_CHECK_EQ(stats->PacketsSent(), 0);
    RTC_CHECK_EQ(stats->PacketsReceived(), 0);
  }
}

void StatsBasedNetworkQualityMetricsReporter::NetworkLayerStatsCollector::
    AddPeer(absl::string_view peer_name,
            std::vector<EmulatedEndpoint*> endpoints) {
  MutexLock lock(&mutex_);
  // When new peer is added not in the constructor, don't check if it has empty
  // stats, because their endpoint could be used for traffic before.
  peer_endpoints_.emplace(peer_name, std::move(endpoints));
  for (const EmulatedEndpoint* const endpoint : endpoints) {
    RTC_CHECK(ip_to_peer_.find(endpoint->GetPeerLocalAddress()) ==
              ip_to_peer_.end())
        << "Two peers can't share the same endpoint";
    ip_to_peer_.emplace(endpoint->GetPeerLocalAddress(), peer_name);
  }
}

std::map<std::string,
         StatsBasedNetworkQualityMetricsReporter::NetworkLayerStats>
StatsBasedNetworkQualityMetricsReporter::NetworkLayerStatsCollector::
    GetStats() {
  MutexLock lock(&mutex_);
  std::map<std::string, NetworkLayerStats> peer_to_stats;
  std::map<std::string, std::vector<std::string>> sender_to_receivers;
  for (const auto& entry : peer_endpoints_) {
    NetworkLayerStats stats;
    stats.stats = PopulateStats(entry.second, network_emulation_);
    const std::string& peer_name = entry.first;
    for (const auto& income_stats_entry :
         stats.stats->IncomingStatsPerSource()) {
      const rtc::IPAddress& source_ip = income_stats_entry.first;
      auto it = ip_to_peer_.find(source_ip);
      if (it == ip_to_peer_.end()) {
        // Source IP is unknown for this collector, so will be skipped.
        continue;
      }
      sender_to_receivers[it->second].push_back(peer_name);
    }
    peer_to_stats.emplace(peer_name, std::move(stats));
  }
  for (auto& entry : peer_to_stats) {
    const std::vector<std::string>& receivers =
        sender_to_receivers[entry.first];
    entry.second.receivers =
        std::set<std::string>(receivers.begin(), receivers.end());
  }
  return peer_to_stats;
}

void StatsBasedNetworkQualityMetricsReporter::AddPeer(
    absl::string_view peer_name,
    std::vector<EmulatedEndpoint*> endpoints) {
  collector_.AddPeer(peer_name, std::move(endpoints));
}

void StatsBasedNetworkQualityMetricsReporter::Start(
    absl::string_view test_case_name,
    const TrackIdStreamInfoMap* reporter_helper) {
  test_case_name_ = std::string(test_case_name);
  collector_.Start();
  start_time_ = clock_->CurrentTime();
}

void StatsBasedNetworkQualityMetricsReporter::OnStatsReports(
    absl::string_view pc_label,
    const rtc::scoped_refptr<const RTCStatsReport>& report) {
  PCStats cur_stats;

  auto inbound_stats = report->GetStatsOfType<RTCInboundRTPStreamStats>();
  for (const auto& stat : inbound_stats) {
    cur_stats.payload_received +=
        DataSize::Bytes(stat->bytes_received.ValueOrDefault(0ul) +
                        stat->header_bytes_received.ValueOrDefault(0ul));
  }

  auto outbound_stats = report->GetStatsOfType<RTCOutboundRTPStreamStats>();
  for (const auto& stat : outbound_stats) {
    cur_stats.payload_sent +=
        DataSize::Bytes(stat->bytes_sent.ValueOrDefault(0ul) +
                        stat->header_bytes_sent.ValueOrDefault(0ul));
  }

  auto candidate_pairs_stats = report->GetStatsOfType<RTCTransportStats>();
  for (const auto& stat : candidate_pairs_stats) {
    cur_stats.total_received +=
        DataSize::Bytes(stat->bytes_received.ValueOrDefault(0ul));
    cur_stats.total_sent +=
        DataSize::Bytes(stat->bytes_sent.ValueOrDefault(0ul));
    cur_stats.packets_received += stat->packets_received.ValueOrDefault(0ul);
    cur_stats.packets_sent += stat->packets_sent.ValueOrDefault(0ul);
  }

  MutexLock lock(&mutex_);
  pc_stats_[std::string(pc_label)] = cur_stats;
}

void StatsBasedNetworkQualityMetricsReporter::StopAndReportResults() {
  Timestamp end_time = clock_->CurrentTime();

  if (!webrtc::field_trial::IsEnabled(kUseStandardBytesStats)) {
    RTC_LOG(LS_ERROR)
        << "Non-standard GetStats; \"payload\" counts include RTP headers";
  }

  std::map<std::string, NetworkLayerStats> stats = collector_.GetStats();
  for (const auto& entry : stats) {
    LogNetworkLayerStats(entry.first, entry.second);
  }
  MutexLock lock(&mutex_);
  for (const auto& pair : pc_stats_) {
    auto it = stats.find(pair.first);
    RTC_CHECK(it != stats.end())
        << "Peer name used for PeerConnection stats collection and peer name "
           "used for endpoints naming doesn't match. No endpoints found for "
           "peer "
        << pair.first;
    const NetworkLayerStats& network_layer_stats = it->second;
    int64_t total_packets_received = 0;
    bool found = false;
    for (const auto& dest_peer : network_layer_stats.receivers) {
      auto pc_stats_it = pc_stats_.find(dest_peer);
      if (pc_stats_it == pc_stats_.end()) {
        continue;
      }
      found = true;
      total_packets_received += pc_stats_it->second.packets_received;
    }
    int64_t packet_loss = -1;
    if (found) {
      packet_loss = pair.second.packets_sent - total_packets_received;
    }
    ReportStats(pair.first, pair.second, network_layer_stats, packet_loss,
                end_time);
  }
}

void StatsBasedNetworkQualityMetricsReporter::ReportStats(
    const std::string& pc_label,
    const PCStats& pc_stats,
    const NetworkLayerStats& network_layer_stats,
    int64_t packet_loss,
    const Timestamp& end_time) {
  std::map<std::string, std::string> metric_metadata{
      {MetricMetadataKey::kPeerMetadataKey, pc_label}};
  metrics_logger_->LogSingleValueMetric(
      "bytes_discarded_no_receiver", GetTestCaseName(pc_label),
      network_layer_stats.stats->BytesDropped().bytes(), Unit::kBytes,
      ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "packets_discarded_no_receiver", GetTestCaseName(pc_label),
      network_layer_stats.stats->PacketsDropped(), Unit::kUnitless,
      ImprovementDirection::kNeitherIsBetter, metric_metadata);

  metrics_logger_->LogSingleValueMetric(
      "payload_bytes_received", GetTestCaseName(pc_label),
      pc_stats.payload_received.bytes(), Unit::kBytes,
      ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "payload_bytes_sent", GetTestCaseName(pc_label),
      pc_stats.payload_sent.bytes(), Unit::kBytes,
      ImprovementDirection::kNeitherIsBetter, metric_metadata);

  metrics_logger_->LogSingleValueMetric(
      "bytes_sent", GetTestCaseName(pc_label), pc_stats.total_sent.bytes(),
      Unit::kBytes, ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "packets_sent", GetTestCaseName(pc_label), pc_stats.packets_sent,
      Unit::kUnitless, ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "average_send_rate", GetTestCaseName(pc_label),
      (pc_stats.total_sent / (end_time - start_time_)).kbps<double>(),
      Unit::kKilobitsPerSecond, ImprovementDirection::kNeitherIsBetter,
      metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "bytes_received", GetTestCaseName(pc_label),
      pc_stats.total_received.bytes(), Unit::kBytes,
      ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "packets_received", GetTestCaseName(pc_label), pc_stats.packets_received,
      Unit::kUnitless, ImprovementDirection::kNeitherIsBetter, metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "average_receive_rate", GetTestCaseName(pc_label),
      (pc_stats.total_received / (end_time - start_time_)).kbps<double>(),
      Unit::kKilobitsPerSecond, ImprovementDirection::kNeitherIsBetter,
      metric_metadata);
  metrics_logger_->LogSingleValueMetric(
      "sent_packets_loss", GetTestCaseName(pc_label), packet_loss,
      Unit::kUnitless, ImprovementDirection::kNeitherIsBetter, metric_metadata);
}

std::string StatsBasedNetworkQualityMetricsReporter::GetTestCaseName(
    absl::string_view network_label) const {
  rtc::StringBuilder builder;
  builder << test_case_name_ << "/" << network_label.data();
  return builder.str();
}

void StatsBasedNetworkQualityMetricsReporter::LogNetworkLayerStats(
    const std::string& peer_name,
    const NetworkLayerStats& stats) const {
  DataRate average_send_rate = stats.stats->PacketsSent() >= 2
                                   ? stats.stats->AverageSendRate()
                                   : DataRate::Zero();
  DataRate average_receive_rate = stats.stats->PacketsReceived() >= 2
                                      ? stats.stats->AverageReceiveRate()
                                      : DataRate::Zero();
  std::map<std::string, std::string> metric_metadata{
      {MetricMetadataKey::kPeerMetadataKey, peer_name}};
  rtc::StringBuilder log;
  log << "Raw network layer statistic for [" << peer_name << "]:\n"
      << "Local IPs:\n";
  std::vector<rtc::IPAddress> local_ips = stats.stats->LocalAddresses();
  for (size_t i = 0; i < local_ips.size(); ++i) {
    log << "  " << local_ips[i].ToString() << "\n";
  }
  if (!stats.stats->SentPacketsSizeCounter().IsEmpty()) {
    metrics_logger_->LogMetric(
        "sent_packets_size", GetTestCaseName(peer_name),
        stats.stats->SentPacketsSizeCounter(), Unit::kBytes,
        ImprovementDirection::kNeitherIsBetter, metric_metadata);
  }
  if (!stats.stats->ReceivedPacketsSizeCounter().IsEmpty()) {
    metrics_logger_->LogMetric(
        "received_packets_size", GetTestCaseName(peer_name),
        stats.stats->ReceivedPacketsSizeCounter(), Unit::kBytes,
        ImprovementDirection::kNeitherIsBetter, metric_metadata);
  }
  if (!stats.stats->DroppedPacketsSizeCounter().IsEmpty()) {
    metrics_logger_->LogMetric(
        "dropped_packets_size", GetTestCaseName(peer_name),
        stats.stats->DroppedPacketsSizeCounter(), Unit::kBytes,
        ImprovementDirection::kNeitherIsBetter, metric_metadata);
  }
  if (!stats.stats->SentPacketsQueueWaitTimeUs().IsEmpty()) {
    metrics_logger_->LogMetric(
        "sent_packets_queue_wait_time_us", GetTestCaseName(peer_name),
        stats.stats->SentPacketsQueueWaitTimeUs(), Unit::kUnitless,
        ImprovementDirection::kNeitherIsBetter, metric_metadata);
  }

  log << "Send statistic:\n"
      << "  packets: " << stats.stats->PacketsSent()
      << " bytes: " << stats.stats->BytesSent().bytes()
      << " avg_rate (bytes/sec): " << average_send_rate.bytes_per_sec()
      << " avg_rate (bps): " << average_send_rate.bps() << "\n"
      << "Send statistic per destination:\n";

  for (const auto& entry : stats.stats->OutgoingStatsPerDestination()) {
    DataRate source_average_send_rate = entry.second->PacketsSent() >= 2
                                            ? entry.second->AverageSendRate()
                                            : DataRate::Zero();
    log << "(" << entry.first.ToString() << "):\n"
        << "  packets: " << entry.second->PacketsSent()
        << " bytes: " << entry.second->BytesSent().bytes()
        << " avg_rate (bytes/sec): " << source_average_send_rate.bytes_per_sec()
        << " avg_rate (bps): " << source_average_send_rate.bps() << "\n";
    if (!entry.second->SentPacketsSizeCounter().IsEmpty()) {
      metrics_logger_->LogMetric(
          "sent_packets_size",
          GetTestCaseName(peer_name + "/" + entry.first.ToString()),
          stats.stats->SentPacketsSizeCounter(), Unit::kBytes,
          ImprovementDirection::kNeitherIsBetter, metric_metadata);
    }
  }

  log << "Receive statistic:\n"
      << "  packets: " << stats.stats->PacketsReceived()
      << " bytes: " << stats.stats->BytesReceived().bytes()
      << " avg_rate (bytes/sec): " << average_receive_rate.bytes_per_sec()
      << " avg_rate (bps): " << average_receive_rate.bps() << "\n"
      << "Receive statistic per source:\n";

  for (const auto& entry : stats.stats->IncomingStatsPerSource()) {
    DataRate source_average_receive_rate =
        entry.second->PacketsReceived() >= 2
            ? entry.second->AverageReceiveRate()
            : DataRate::Zero();
    log << "(" << entry.first.ToString() << "):\n"
        << "  packets: " << entry.second->PacketsReceived()
        << " bytes: " << entry.second->BytesReceived().bytes()
        << " avg_rate (bytes/sec): "
        << source_average_receive_rate.bytes_per_sec()
        << " avg_rate (bps): " << source_average_receive_rate.bps() << "\n";
    if (!entry.second->ReceivedPacketsSizeCounter().IsEmpty()) {
      metrics_logger_->LogMetric(
          "received_packets_size",
          GetTestCaseName(peer_name + "/" + entry.first.ToString()),
          stats.stats->ReceivedPacketsSizeCounter(), Unit::kBytes,
          ImprovementDirection::kNeitherIsBetter, metric_metadata);
    }
    if (!entry.second->DroppedPacketsSizeCounter().IsEmpty()) {
      metrics_logger_->LogMetric(
          "dropped_packets_size",
          GetTestCaseName(peer_name + "/" + entry.first.ToString()),
          stats.stats->DroppedPacketsSizeCounter(), Unit::kBytes,
          ImprovementDirection::kNeitherIsBetter, metric_metadata);
    }
  }

  RTC_LOG(LS_INFO) << log.str();
}

}  // namespace webrtc_pc_e2e
}  // namespace webrtc
