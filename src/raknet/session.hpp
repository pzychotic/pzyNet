#pragma once

// Internal: the reliability layer of one RakNet connection, as a sans-I/O state machine.
// Datagram framing, ACK/NAK, retransmission, ordering/sequencing, splitting and congestion control.
// It knows nothing about handshakes or message ids; RakNetEngine layers those on top.

#include "pzynet/core/clock.hpp"
#include "pzynet/core/span.hpp"
#include "pzynet/raknet/wire.hpp"
#include "pzynet/transport/types.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace pzynet
{
namespace raknet
{
namespace detail
{

using wire::Bytes;

/// A sorted set of 24-bit numbers stored as disjoint, non-adjacent inclusive ranges.
class RangeSet
{
public:
    bool empty() const noexcept { return ranges_.empty(); }
    Span<const wire::Range> ranges() const noexcept { return ranges_; }
    void insert(std::uint32_t value);
    void erase(std::uint32_t value);
    /// Drops the first `count` ranges.
    void pop_front(std::size_t count);
    void clear() noexcept { ranges_.clear(); }

private:
    std::vector<wire::Range> ranges_;
};

struct SessionConfig
{
    std::size_t mtu = wire::minimum_mtu; ///< negotiated MTU including IP and UDP headers
    Duration timeout = std::chrono::seconds(10);
    std::size_t max_message_size = 16u * 1024 * 1024;
};

struct Delivered
{
    transport::Reliability reliability = transport::Reliability::unreliable;
    std::uint8_t channel = 0;
    Bytes data;
};

struct Receipt
{
    std::uint32_t token = 0;
    bool delivered = false;
};

struct SessionCounters
{
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t datagrams_resent = 0;
    std::uint64_t datagrams_lost = 0;
};

class Session
{
public:
    Session(const SessionConfig& config, TimePoint now);

    /// Queues a message; it goes out on a later advance().
    std::error_code send(ByteView payload, const transport::SendOptions& options, TimePoint now);

    /// Processes one datagram received from the peer.
    void handle_datagram(ByteView datagram, TimePoint now);

    /// Runs timers and appends the datagrams to send to `out`.
    void advance(TimePoint now, std::vector<Bytes>& out);

    std::optional<TimePoint> next_deadline() const;

    bool poll_delivered(Delivered& out);
    bool poll_receipt(Receipt& out);

    /// The peer stopped acknowledging: reliable data went unanswered for the whole timeout.
    bool timed_out() const noexcept { return timed_out_; }
    /// The peer sent something no well-behaved implementation sends (resource limits exceeded).
    bool violated() const noexcept { return violated_; }

    /// Nothing queued and nothing awaiting acknowledgement.
    bool flushed() const noexcept { return queue_.empty() && unacked_.empty(); }
    bool has_unacked() const noexcept { return !unacked_.empty(); }
    bool acks_pending() const noexcept { return !acks_.empty(); }
    TimePoint last_reliable_send() const noexcept { return last_reliable_send_; }
    std::size_t mtu() const noexcept { return config_.mtu; }
    Duration timeout() const noexcept { return config_.timeout; }
    void set_timeout(Duration timeout) noexcept { config_.timeout = timeout; }
    /// Smoothed round-trip time, once at least one acknowledgement arrived.
    std::optional<Duration> rtt() const;

    const SessionCounters& counters() const noexcept { return counters_; }
    /// Messages in the send queue (a split message counts once, until its last piece leaves).
    std::size_t queued_messages() const noexcept;
    /// Payload bytes in the send queue.
    std::size_t queued_bytes() const noexcept;

    /// Largest payload that fits in one datagram without splitting.
    std::size_t max_unsplit_payload() const noexcept;

private:
    struct OutMessage
    {
        wire::MessageHeader header;
        std::shared_ptr<const Bytes> buffer; // split chunks share the original message
        std::size_t offset = 0;
        std::size_t length = 0;
        std::uint32_t receipt_token = 0;
        std::uint8_t priority = 0;
        TimePoint enqueued{};
        // while awaiting acknowledgement:
        std::multimap<TimePoint, std::uint32_t>::iterator schedule{};
        std::uint64_t last_datagram = 0; // the datagram that carried the latest transmission
        bool resend_requested = false;   // by a NAK, as opposed to a retransmission time-out

        std::size_t wire_size() const noexcept
        {
            return wire::message_header_size(header.reliability, header.split) + length;
        }
    };

    struct SentDatagram
    {
        TimePoint sent{};
        std::vector<std::uint32_t> reliable_numbers;
        std::vector<std::uint32_t> receipt_tokens; // of unreliable messages that asked for a receipt
        bool continuous = false;
        bool acked = false;
        bool lost = false;    // presumed lost: NAKed, or its messages retransmitted on time-out
        bool resends = false; // carries at least one retransmission
    };

    struct PendingReceipt
    {
        TimePoint deadline{};
        std::uint64_t datagram = 0;
    };

    struct Assembly
    {
        std::uint32_t count = 0;
        std::map<std::uint32_t, Bytes> parts;
        std::size_t bytes = 0;
        std::size_t charged = 0;
        TimePoint updated{};
    };

    struct OrderKey
    {
        std::uint64_t ordering = 0;   // extended (non-wrapping) ordering index
        std::uint32_t sequencing = 0; // sequenced: its index; ordered: UINT32_MAX, so it sorts last
        bool operator<(const OrderKey& other) const noexcept
        {
            return ordering != other.ordering ? ordering < other.ordering : sequencing < other.sequencing;
        }
    };

    struct Channel
    {
        // sending
        std::uint32_t ordered_write = 0;
        std::uint32_t sequenced_write = 0;
        // receiving
        std::uint64_t ordered_read = 0; // extended; the wire carries its low 24 bits
        std::uint32_t highest_sequenced = 0;
        std::map<OrderKey, Delivered> waiting;
    };

    // sending
    std::uint64_t next_weight(transport::Priority priority);
    void enqueue(OutMessage&& message, transport::Priority priority);
    void append_message(OutMessage& message, TimePoint now, std::vector<Bytes>& out);
    void close_datagram(std::vector<Bytes>& out);
    void flush_acks(std::vector<Bytes>& out);
    void flush_naks(std::vector<Bytes>& out);
    void expire_receipts(TimePoint now);
    void trim_history();
    void presume_lost(SentDatagram& record);
    Duration rto() const;
    bool in_slow_start() const noexcept { return ss_thresh_ == 0.0 || cwnd_ <= ss_thresh_; }

    // acknowledgements
    template <class F>
    void for_each_sent(const wire::Range& range, F&& visit);
    void on_acked(std::uint64_t number, SentDatagram& record, TimePoint now);
    void on_nak(std::uint64_t number, SentDatagram& record, TimePoint now);

    // receiving
    void handle_data(const wire::DatagramHeader& header, ByteReader& in, TimePoint now);
    void receive_message(const wire::MessageHeader& header, ByteView payload, TimePoint now);
    bool accept_reliable(std::uint32_t number);
    bool assemble(const wire::MessageHeader& header, ByteView payload, TimePoint now, Bytes& out);
    void order(const wire::MessageHeader& header, Bytes&& data);
    void deliver(wire::Reliability reliability, std::uint8_t channel, Bytes&& data);
    bool deliver_sequenced(Channel& channel, std::uint32_t sequencing, Delivered&& message);

    SessionConfig config_;
    std::size_t payload_budget_; // UDP payload bytes per datagram

    // --- send side ---
    std::multimap<std::uint64_t, OutMessage> queue_; // by priority weight; FIFO among equal weights
    std::array<std::uint64_t, 4> next_weights_{};
    std::unordered_map<std::uint32_t, OutMessage> unacked_; // by reliable message number
    std::multimap<TimePoint, std::uint32_t> resend_schedule_;
    std::size_t unacked_bytes_ = 0;
    std::uint32_t next_reliable_number_ = 0;
    std::uint16_t next_split_id_ = 0;
    TimePoint last_reliable_send_;

    std::deque<SentDatagram> history_; // history_[i] is datagram history_base_ + i
    std::uint64_t history_base_ = 0;
    std::uint64_t next_datagram_ = 0;
    std::deque<PendingReceipt> pending_receipts_;
    std::deque<Receipt> receipts_;

    // the datagram currently being filled during advance()
    Bytes building_;
    SentDatagram building_record_;
    bool building_open_ = false;
    std::size_t datagrams_this_advance_ = 0;
    bool continuous_ = false; // data was left queued after the previous flush

    // congestion control (TCP-Reno-like sliding window, in bytes)
    double cwnd_;
    double ss_thresh_ = 0.0; // 0 = not set yet: slow start
    std::uint64_t next_congestion_block_ = 0;
    TimePoint last_backoff_;
    bool has_rtt_ = false;
    double rtt_us_ = 0.0;
    double rtt_deviation_us_ = 0.0;

    // --- receive side ---
    RangeSet acks_;
    RangeSet naks_;
    TimePoint oldest_unsent_ack_;
    std::uint32_t expected_datagram_ = 0;
    TimePoint last_received_;

    std::uint32_t received_base_ = 0; // lowest reliable message number not yet seen
    std::deque<bool> holes_;          // holes_[i]: number received_base_ + i is still missing

    std::array<Channel, wire::ordering_channel_count> channels_;
    std::size_t waiting_bytes_ = 0;

    std::unordered_map<std::uint16_t, Assembly> assemblies_;
    std::size_t assembly_charged_ = 0;

    std::deque<Delivered> delivered_;
    bool timed_out_ = false;
    bool violated_ = false;

    SessionCounters counters_;
};

} // namespace detail
} // namespace raknet
} // namespace pzynet
