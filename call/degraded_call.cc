/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "call/degraded_call.h"

#include <utility>

#include "absl/memory/memory.h"
#include "rtc_base/location.h"

namespace webrtc {

DegradedCall::FakeNetworkPipeOnTaskQueue::FakeNetworkPipeOnTaskQueue(
    TaskQueueFactory* task_queue_factory,
    Clock* clock,
    std::unique_ptr<NetworkBehaviorInterface> network_behavior,
    Transport* transport)
    : clock_(clock),
      task_queue_(task_queue_factory->CreateTaskQueue(
          "DegradedSendQueue",
          TaskQueueFactory::Priority::NORMAL)),
      pipe_(clock, std::move(network_behavior), transport) {}

void DegradedCall::FakeNetworkPipeOnTaskQueue::SendRtp(
    const uint8_t* packet,
    size_t length,
    const PacketOptions& options) {
  pipe_.SendRtp(packet, length, options);
  Process();
}

void DegradedCall::FakeNetworkPipeOnTaskQueue::SendRtcp(const uint8_t* packet,
                                                        size_t length) {
  pipe_.SendRtcp(packet, length);
  Process();
}

bool DegradedCall::FakeNetworkPipeOnTaskQueue::Process() {
  pipe_.Process();
  auto time_to_next = pipe_.TimeUntilNextProcess();
  if (!time_to_next) {
    // Packet was probably sent immediately.
    return false;
  }

  task_queue_.PostTask([this, time_to_next]() {
    RTC_DCHECK_RUN_ON(&task_queue_);
    int64_t next_process_time = *time_to_next + clock_->TimeInMilliseconds();
    if (!next_process_ms_ || next_process_time < *next_process_ms_) {
      next_process_ms_ = next_process_time;
      task_queue_.PostDelayedTask(
          [this]() {
            RTC_DCHECK_RUN_ON(&task_queue_);
            if (!Process()) {
              next_process_ms_.reset();
            }
          },
          *time_to_next);
    }
  });

  return true;
}

DegradedCall::DegradedCall(
    std::unique_ptr<Call> call,
    absl::optional<BuiltInNetworkBehaviorConfig> send_config,
    absl::optional<BuiltInNetworkBehaviorConfig> receive_config,
    TaskQueueFactory* task_queue_factory)
    : clock_(Clock::GetRealTimeClock()),
      call_(std::move(call)),
      task_queue_factory_(task_queue_factory),
      send_config_(send_config),
      send_simulated_network_(nullptr),
      receive_config_(receive_config) {
  if (receive_config_) {
    auto network = absl::make_unique<SimulatedNetwork>(*receive_config_);
    receive_simulated_network_ = network.get();
    receive_pipe_ =
        absl::make_unique<webrtc::FakeNetworkPipe>(clock_, std::move(network));
    receive_pipe_->SetReceiver(call_->Receiver());
  }
}

DegradedCall::~DegradedCall() = default;

AudioSendStream* DegradedCall::CreateAudioSendStream(
    const AudioSendStream::Config& config) {
  return call_->CreateAudioSendStream(config);
}

void DegradedCall::DestroyAudioSendStream(AudioSendStream* send_stream) {
  call_->DestroyAudioSendStream(send_stream);
}

AudioReceiveStream* DegradedCall::CreateAudioReceiveStream(
    const AudioReceiveStream::Config& config) {
  return call_->CreateAudioReceiveStream(config);
}

void DegradedCall::DestroyAudioReceiveStream(
    AudioReceiveStream* receive_stream) {
  call_->DestroyAudioReceiveStream(receive_stream);
}

VideoSendStream* DegradedCall::CreateVideoSendStream(
    VideoSendStream::Config config,
    VideoEncoderConfig encoder_config) {
  if (send_config_ && !send_pipe_) {
    auto network = absl::make_unique<SimulatedNetwork>(*send_config_);
    send_simulated_network_ = network.get();
    send_pipe_ = absl::make_unique<FakeNetworkPipeOnTaskQueue>(
        task_queue_factory_, clock_, std::move(network), config.send_transport);
    config.send_transport = this;
  }
  return call_->CreateVideoSendStream(std::move(config),
                                      std::move(encoder_config));
}

VideoSendStream* DegradedCall::CreateVideoSendStream(
    VideoSendStream::Config config,
    VideoEncoderConfig encoder_config,
    std::unique_ptr<FecController> fec_controller) {
  if (send_config_ && !send_pipe_) {
    auto network = absl::make_unique<SimulatedNetwork>(*send_config_);
    send_simulated_network_ = network.get();
    send_pipe_ = absl::make_unique<FakeNetworkPipeOnTaskQueue>(
        task_queue_factory_, clock_, std::move(network), config.send_transport);
    config.send_transport = this;
  }
  return call_->CreateVideoSendStream(
      std::move(config), std::move(encoder_config), std::move(fec_controller));
}

void DegradedCall::DestroyVideoSendStream(VideoSendStream* send_stream) {
  call_->DestroyVideoSendStream(send_stream);
}

VideoReceiveStream* DegradedCall::CreateVideoReceiveStream(
    VideoReceiveStream::Config configuration) {
  return call_->CreateVideoReceiveStream(std::move(configuration));
}

void DegradedCall::DestroyVideoReceiveStream(
    VideoReceiveStream* receive_stream) {
  call_->DestroyVideoReceiveStream(receive_stream);
}

FlexfecReceiveStream* DegradedCall::CreateFlexfecReceiveStream(
    const FlexfecReceiveStream::Config& config) {
  return call_->CreateFlexfecReceiveStream(config);
}

void DegradedCall::DestroyFlexfecReceiveStream(
    FlexfecReceiveStream* receive_stream) {
  call_->DestroyFlexfecReceiveStream(receive_stream);
}

PacketReceiver* DegradedCall::Receiver() {
  if (receive_config_) {
    return this;
  }
  return call_->Receiver();
}

RtpTransportControllerSendInterface*
DegradedCall::GetTransportControllerSend() {
  return call_->GetTransportControllerSend();
}

Call::Stats DegradedCall::GetStats() const {
  return call_->GetStats();
}

void DegradedCall::SignalChannelNetworkState(MediaType media,
                                             NetworkState state) {
  call_->SignalChannelNetworkState(media, state);
}

void DegradedCall::OnAudioTransportOverheadChanged(
    int transport_overhead_per_packet) {
  call_->OnAudioTransportOverheadChanged(transport_overhead_per_packet);
}

void DegradedCall::OnSentPacket(const rtc::SentPacket& sent_packet) {
  if (send_config_) {
    // If we have a degraded send-transport, we have already notified call
    // about the supposed network send time. Discard the actual network send
    // time in order to properly fool the BWE.
    return;
  }
  call_->OnSentPacket(sent_packet);
}

bool DegradedCall::SendRtp(const uint8_t* packet,
                           size_t length,
                           const PacketOptions& options) {
  // A call here comes from the RTP stack (probably pacer). We intercept it and
  // put it in the fake network pipe instead, but report to Call that is has
  // been sent, so that the bandwidth estimator sees the delay we add.
  send_pipe_->SendRtp(packet, length, options);
  if (options.packet_id != -1) {
    rtc::SentPacket sent_packet;
    sent_packet.packet_id = options.packet_id;
    sent_packet.send_time_ms = clock_->TimeInMilliseconds();
    sent_packet.info.included_in_feedback = options.included_in_feedback;
    sent_packet.info.included_in_allocation = options.included_in_allocation;
    sent_packet.info.packet_size_bytes = length;
    sent_packet.info.packet_type = rtc::PacketType::kData;
    call_->OnSentPacket(sent_packet);
  }
  return true;
}

bool DegradedCall::SendRtcp(const uint8_t* packet, size_t length) {
  send_pipe_->SendRtcp(packet, length);
  return true;
}

PacketReceiver::DeliveryStatus DegradedCall::DeliverPacket(
    MediaType media_type,
    rtc::CopyOnWriteBuffer packet,
    int64_t packet_time_us) {
  PacketReceiver::DeliveryStatus status = receive_pipe_->DeliverPacket(
      media_type, std::move(packet), packet_time_us);
  // This is not optimal, but there are many places where there are thread
  // checks that fail if we're not using the worker thread call into this
  // method. If we want to fix this we probably need a task queue to do handover
  // of all overriden methods, which feels like overikill for the current use
  // case.
  // By just having this thread call out via the Process() method we work around
  // that, with the tradeoff that a non-zero delay may become a little larger
  // than anticipated at very low packet rates.
  receive_pipe_->Process();
  return status;
}

}  // namespace webrtc
