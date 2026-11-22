#include "session.hpp"

#include "pzynet/core/error.hpp"
#include "pzynet/core/sequence.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pzynet
{
namespace raknet
{
namespace detail
{
namespace
{

using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr Duration ack_delay = milliseconds(10);          // acknowledgements are batched this long
constexpr Duration unreliable_timeout = seconds(1);       // unreliable data older than this is not worth sending
constexpr Duration assembly_timeout = seconds(60);        // a split message with no progress is abandoned
constexpr Duration initial_rto = seconds(1);
constexpr double maximum_rto_us = 2'000'000.0;
constexpr std::size_t maximum_history = 4096;             // sent datagrams remembered for ACK/NAK lookup
constexpr std::uint32_t maximum_datagram_jump = 50'000;   // a larger gap is not credible
constexpr std::uint32_t maximum_naks_per_gap = 1000;
constexpr std::uint32_t maximum_reliable_hole = 1'000'000;
constexpr std::size_t minimum_chunk_charge = 256;         // memory accounting floor per buffered chunk
constexpr std::size_t waiting_overhead = 64;              // memory accounting per buffered ordered message
constexpr std::uint32_t ordered_key = std::numeric_limits<std::uint32_t>::max();

static_assert(static_cast<int>(transport::Reliability::unreliable) == static_cast<int>(wire::Reliability::unreliable) &&
                  static_cast<int>(transport::Reliability::reliable_sequenced) ==
                      static_cast<int>(wire::Reliability::reliable_sequenced),
              "transport and wire reliabilities share their numbering");

wire::Reliability to_wire(transport::Reliability r) noexcept
{
    return static_cast<wire::Reliability>(r);
}
transport::Reliability to_transport(wire::Reliability r) noexcept
{
    return static_cast<transport::Reliability>(r);
}

double to_us(Duration d) noexcept
{
    return std::chrono::duration<double, std::micro>(d).count();
}

} // namespace

// --- RangeSet ----------------------------------------------------------------------------------------

void RangeSet::insert(std::uint32_t value)
{
    // First range that ends at or after `value`.
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), value,
                               [](const wire::Range& range, std::uint32_t v) { return range.last < v; });
    if (it != ranges_.end() && it->first <= value)
        return; // already present

    const bool joins_next = it != ranges_.end() && it->first == value + 1;
    const bool joins_previous = it != ranges_.begin() && (it - 1)->last + 1 == value;
    if (joins_previous && joins_next)
    {
        (it - 1)->last = it->last;
        ranges_.erase(it);
    }
    else if (joins_previous)
        (it - 1)->last = value;
    else if (joins_next)
        it->first = value;
    else
        ranges_.insert(it, wire::Range{value, value});
}

void RangeSet::erase(std::uint32_t value)
{
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), value,
                               [](const wire::Range& range, std::uint32_t v) { return range.last < v; });
    if (it == ranges_.end() || it->first > value)
        return;
    if (it->first == it->last)
        ranges_.erase(it);
    else if (it->first == value)
        ++it->first;
    else if (it->last == value)
        --it->last;
    else
    {
        const wire::Range upper{value + 1, it->last};
        it->last = value - 1;
        ranges_.insert(it + 1, upper);
    }
}

void RangeSet::pop_front(std::size_t count)
{
    ranges_.erase(ranges_.begin(), ranges_.begin() + static_cast<std::ptrdiff_t>(std::min(count, ranges_.size())));
}

// --- Session: construction and queries ---------------------------------------------------------------

Session::Session(const SessionConfig& config, TimePoint now)
    : config_(config), payload_budget_(config.mtu - wire::udp_header_size), last_reliable_send_(now),
      cwnd_(static_cast<double>(config.mtu - wire::udp_header_size)), last_backoff_(now), oldest_unsent_ack_(now),
      last_received_(now)
{
}

std::size_t Session::max_unsplit_payload() const noexcept
{
    return payload_budget_ - wire::data_datagram_header_size - wire::maximum_message_header_size;
}

std::optional<Duration> Session::rtt() const
{
    if (!has_rtt_)
        return std::nullopt;
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::micro>(rtt_us_));
}

Duration Session::rto() const
{
    if (!has_rtt_)
        return initial_rto;
    const double us = std::min(2.0 * rtt_us_ + 4.0 * rtt_deviation_us_ + 30'000.0, maximum_rto_us);
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::micro>(us));
}

std::size_t Session::queued_messages() const noexcept
{
    std::size_t count = 0;
    for (const auto& entry : queue_)
    {
        const wire::MessageHeader& header = entry.second.header;
        if (!header.split || header.split_index + 1 == header.split_count)
            ++count;
    }
    return count;
}

std::size_t Session::queued_bytes() const noexcept
{
    std::size_t bytes = 0;
    for (const auto& entry : queue_)
        bytes += entry.second.length;
    return bytes;
}

bool Session::poll_delivered(Delivered& out)
{
    if (delivered_.empty())
        return false;
    out = std::move(delivered_.front());
    delivered_.pop_front();
    return true;
}

bool Session::poll_receipt(Receipt& out)
{
    if (receipts_.empty())
        return false;
    out = receipts_.front();
    receipts_.pop_front();
    return true;
}

// --- Session: sending --------------------------------------------------------------------------------

// RakNet's weighting: each priority level gets roughly twice the share of the next lower one, and a
// level that was idle does not get to jump the whole queue when it becomes active again.
std::uint64_t Session::next_weight(transport::Priority priority)
{
    const auto base = [](std::uint64_t p) { return (std::uint64_t{1} << p) * p + p; };
    const auto p = static_cast<std::uint64_t>(priority);

    if (queue_.empty())
        for (std::uint64_t level = 0; level < next_weights_.size(); ++level)
            next_weights_[level] = base(level);

    std::uint64_t next = next_weights_[p];
    if (!queue_.empty())
    {
        const std::uint64_t head_priority = queue_.begin()->second.priority;
        const std::uint64_t minimum =
            queue_.begin()->first - (std::uint64_t{1} << head_priority) * head_priority + head_priority;
        if (next < minimum)
            next = minimum + base(p);
    }
    next_weights_[p] = next + (std::uint64_t{1} << p) * (p + 1) + p;
    return next;
}

void Session::enqueue(OutMessage&& message, transport::Priority priority)
{
    const std::uint64_t weight = next_weight(priority);
    message.priority = static_cast<std::uint8_t>(priority);
    queue_.emplace(weight, std::move(message));
}

std::error_code Session::send(ByteView payload, const transport::SendOptions& options, TimePoint now)
{
    if (timed_out_ || violated_)
        return make_error_code(Errc::not_connected);
    if (payload.empty() || options.channel >= wire::ordering_channel_count ||
        static_cast<unsigned>(options.reliability) > static_cast<unsigned>(transport::Reliability::reliable_sequenced) ||
        static_cast<unsigned>(options.priority) > static_cast<unsigned>(transport::Priority::low))
        return make_error_code(Errc::invalid_argument);
    if (payload.size() > config_.max_message_size)
        return make_error_code(Errc::message_too_large);

    const std::size_t chunk_size = max_unsplit_payload();
    const bool split = payload.size() > chunk_size;

    wire::MessageHeader header;
    header.reliability = to_wire(options.reliability);
    if (split) // a message in pieces is useless unless every piece arrives
    {
        if (header.reliability == wire::Reliability::unreliable)
            header.reliability = wire::Reliability::reliable;
        else if (header.reliability == wire::Reliability::unreliable_sequenced)
            header.reliability = wire::Reliability::reliable_sequenced;
    }

    Channel& channel = channels_[options.channel];
    if (wire::is_sequenced(header.reliability))
    {
        header.ordering_index = channel.ordered_write;
        header.sequencing_index = channel.sequenced_write;
        channel.sequenced_write = seq24::wrap(channel.sequenced_write + 1);
        header.ordering_channel = options.channel;
    }
    else if (header.reliability == wire::Reliability::reliable_ordered)
    {
        header.ordering_index = channel.ordered_write;
        channel.ordered_write = seq24::wrap(channel.ordered_write + 1);
        channel.sequenced_write = 0;
        header.ordering_channel = options.channel;
    }

    OutMessage message;
    message.buffer = std::make_shared<const Bytes>(payload.begin(), payload.end());
    message.enqueued = now;

    if (!split)
    {
        message.header = header;
        message.length = payload.size();
        message.header.bit_length = static_cast<std::uint16_t>(payload.size() * 8);
        message.receipt_token = options.receipt_token;
        enqueue(std::move(message), options.priority);
        return {};
    }

    const std::size_t count = (payload.size() - 1) / chunk_size + 1;
    header.split = true;
    header.split_count = static_cast<std::uint32_t>(count);
    header.split_id = next_split_id_++;
    for (std::size_t i = 0; i < count; ++i)
    {
        OutMessage chunk;
        chunk.buffer = message.buffer;
        chunk.enqueued = now;
        chunk.offset = i * chunk_size;
        chunk.length = std::min(chunk_size, payload.size() - chunk.offset);
        chunk.header = header;
        chunk.header.split_index = static_cast<std::uint32_t>(i);
        chunk.header.bit_length = static_cast<std::uint16_t>(chunk.length * 8);
        // The receipt reports the whole message, so it rides on the final chunk.
        chunk.receipt_token = i + 1 == count ? options.receipt_token : 0;
        enqueue(std::move(chunk), options.priority);
    }
    return {};
}

void Session::append_message(OutMessage& message, TimePoint now, std::vector<Bytes>& out)
{
    if (building_open_ && building_.size() + message.wire_size() > payload_budget_)
        close_datagram(out);

    if (!building_open_)
    {
        building_open_ = true;
        building_.clear();
        building_record_ = SentDatagram();
        building_record_.sent = now;
        building_record_.continuous = continuous_ || datagrams_this_advance_ > 0;

        wire::DatagramHeader header;
        header.kind = wire::DatagramKind::data;
        header.number = seq24::wrap(next_datagram_);
        header.continuous_send = building_record_.continuous;
        header.needs_b_and_as = in_slow_start();
        ByteWriter writer(building_);
        wire::write_datagram_header(writer, header);
    }

    ByteWriter writer(building_);
    wire::write_message_header(writer, message.header);
    writer.bytes(ByteView(message.buffer->data() + message.offset, message.length));
    if (wire::is_reliable(message.header.reliability))
        building_record_.reliable_numbers.push_back(message.header.reliable_number);
}

void Session::close_datagram(std::vector<Bytes>& out)
{
    if (!building_open_)
        return;
    building_open_ = false;
    if (building_record_.resends)
        ++counters_.datagrams_resent;
    history_.push_back(std::move(building_record_));
    ++next_datagram_;
    ++datagrams_this_advance_;
    out.push_back(std::move(building_));
    building_ = Bytes();
    trim_history();
}

void Session::trim_history()
{
    while (!history_.empty() && (history_.front().acked || history_.size() > maximum_history))
    {
        for (std::uint32_t token : history_.front().receipt_tokens)
            receipts_.push_back(Receipt{token, false});
        history_.pop_front();
        ++history_base_;
    }
}

void Session::presume_lost(SentDatagram& record)
{
    if (record.acked || record.lost)
        return;
    record.lost = true;
    ++counters_.datagrams_lost;
}

void Session::flush_acks(std::vector<Bytes>& out)
{
    while (!acks_.empty())
    {
        Bytes datagram;
        ByteWriter writer(datagram);
        wire::DatagramHeader header;
        header.kind = wire::DatagramKind::ack;
        wire::write_datagram_header(writer, header);
        const std::size_t written = wire::write_ranges(writer, acks_.ranges(), payload_budget_ - datagram.size());
        if (written == 0)
            break;
        acks_.pop_front(written);
        out.push_back(std::move(datagram));
    }
}

void Session::flush_naks(std::vector<Bytes>& out)
{
    Bytes datagram;
    ByteWriter writer(datagram);
    wire::DatagramHeader header;
    header.kind = wire::DatagramKind::nak;
    wire::write_datagram_header(writer, header);
    // Whatever does not fit is dropped: the sender's retransmission timer covers it.
    wire::write_ranges(writer, naks_.ranges(), payload_budget_ - datagram.size());
    naks_.clear();
    out.push_back(std::move(datagram));
}

void Session::expire_receipts(TimePoint now)
{
    while (!pending_receipts_.empty() && pending_receipts_.front().deadline <= now)
    {
        const std::uint64_t number = pending_receipts_.front().datagram;
        pending_receipts_.pop_front();
        if (number < history_base_)
            continue; // already resolved when the record left the history
        SentDatagram& record = history_[static_cast<std::size_t>(number - history_base_)];
        if (record.acked)
            continue;
        for (std::uint32_t token : record.receipt_tokens)
            receipts_.push_back(Receipt{token, false});
        record.receipt_tokens.clear();
    }
}

void Session::advance(TimePoint now, std::vector<Bytes>& out)
{
    if (timed_out_ || violated_)
        return;
    if (!unacked_.empty() && now - last_received_ > config_.timeout)
    {
        timed_out_ = true;
        return;
    }

    const std::size_t first_output = out.size();
    expire_receipts(now);
    for (auto it = assemblies_.begin(); it != assemblies_.end();)
    {
        if (now - it->second.updated > assembly_timeout)
        {
            assembly_charged_ -= it->second.charged;
            it = assemblies_.erase(it);
        }
        else
            ++it;
    }

    // Until a round-trip estimate exists, acknowledge at once: the peer is waiting for its first sample.
    if (!acks_.empty() && (!has_rtt_ || now - oldest_unsent_ack_ >= ack_delay))
        flush_acks(out);
    if (!naks_.empty())
        flush_naks(out);

    datagrams_this_advance_ = 0;
    const double mtu = static_cast<double>(payload_budget_);

    // Retransmissions first, and never throttled: they are already counted against the window.
    bool timed_out_resend = false;
    while (!resend_schedule_.empty() && resend_schedule_.begin()->first <= now)
    {
        const std::uint32_t number = resend_schedule_.begin()->second;
        resend_schedule_.erase(resend_schedule_.begin());
        OutMessage& message = unacked_.at(number);
        if (!message.resend_requested)
        {
            timed_out_resend = true;
            if (message.last_datagram >= history_base_ && message.last_datagram < history_base_ + history_.size())
                presume_lost(history_[static_cast<std::size_t>(message.last_datagram - history_base_)]);
        }
        message.resend_requested = false;
        append_message(message, now, out);
        building_record_.resends = true;
        message.last_datagram = next_datagram_;
        message.schedule = resend_schedule_.emplace(now + rto(), number);
    }
    if (timed_out_resend && now - last_backoff_ >= rto())
    {
        // Loss detected by time-out: fall back to slow start, at most once per retransmission period.
        ss_thresh_ = std::max(cwnd_ / 2.0, mtu);
        cwnd_ = mtu;
        next_congestion_block_ = next_datagram_ + 1;
        last_backoff_ = now;
    }

    while (!queue_.empty() && static_cast<double>(unacked_bytes_) < cwnd_)
    {
        OutMessage message = std::move(queue_.begin()->second);
        queue_.erase(queue_.begin());

        const bool reliable = wire::is_reliable(message.header.reliability);
        if (!reliable && now - message.enqueued > unreliable_timeout)
        {
            if (message.receipt_token != 0)
                receipts_.push_back(Receipt{message.receipt_token, false});
            continue;
        }
        if (reliable) // numbered only now, so numbers go out in transmission order
        {
            message.header.reliable_number = next_reliable_number_;
            next_reliable_number_ = seq24::wrap(next_reliable_number_ + 1);
        }

        append_message(message, now, out);

        if (reliable)
        {
            const std::uint32_t number = message.header.reliable_number;
            message.last_datagram = next_datagram_;
            message.schedule = resend_schedule_.emplace(now + rto(), number);
            unacked_bytes_ += message.wire_size();
            last_reliable_send_ = now;
            unacked_.emplace(number, std::move(message));
        }
        else if (message.receipt_token != 0)
        {
            building_record_.receipt_tokens.push_back(message.receipt_token);
            if (pending_receipts_.empty() || pending_receipts_.back().datagram != next_datagram_)
                pending_receipts_.push_back(PendingReceipt{now + rto(), next_datagram_});
        }
    }
    close_datagram(out);
    continuous_ = !queue_.empty();

    for (std::size_t i = first_output; i < out.size(); ++i)
        counters_.bytes_sent += out[i].size();
}

std::optional<TimePoint> Session::next_deadline() const
{
    if (timed_out_ || violated_)
        return std::nullopt;

    std::optional<TimePoint> deadline;
    const auto consider = [&deadline](TimePoint t) {
        if (!deadline || t < *deadline)
            deadline = t;
    };
    const TimePoint asap = TimePoint::min();

    if (!acks_.empty())
        consider(has_rtt_ ? oldest_unsent_ack_ + ack_delay : asap);
    if (!naks_.empty())
        consider(asap);
    if (!resend_schedule_.empty())
        consider(resend_schedule_.begin()->first);
    if (!queue_.empty() && static_cast<double>(unacked_bytes_) < cwnd_)
        consider(asap);
    if (!unacked_.empty())
        consider(last_received_ + config_.timeout);
    if (!pending_receipts_.empty())
        consider(pending_receipts_.front().deadline);
    return deadline;
}

// --- Session: acknowledgements -----------------------------------------------------------------------

// Visits the remembered datagrams whose (24-bit) number lies in `range`.
template <class F>
void Session::for_each_sent(const wire::Range& range, F&& visit)
{
    if (history_.empty())
        return;
    const std::uint32_t base = seq24::wrap(history_base_);
    const std::uint32_t offset = seq24::distance(base, range.first);

    std::size_t start;
    if (offset < history_.size())
        start = offset;
    else if (seq24::distance(range.first, base) <= seq24::distance(range.first, range.last))
        start = 0; // the range begins before the history and reaches into it
    else
        return;

    const std::uint32_t remaining = seq24::distance(seq24::wrap(history_base_ + start), range.last);
    const std::size_t end = std::min<std::size_t>(history_.size() - 1, start + remaining);
    for (std::size_t i = start; i <= end; ++i)
        visit(history_base_ + i, history_[i]);
}

void Session::on_acked(std::uint64_t number, SentDatagram& record, TimePoint now)
{
    if (record.acked)
        return;
    record.acked = true;

    // Every datagram number is sent exactly once, so each acknowledgement is an unambiguous sample.
    const double sample = std::max(to_us(now - record.sent), 0.0);
    if (!has_rtt_)
    {
        has_rtt_ = true;
        rtt_us_ = sample;
        rtt_deviation_us_ = sample;
    }
    else
    {
        const double difference = sample - rtt_us_;
        rtt_us_ += 0.05 * difference;
        rtt_deviation_us_ += 0.05 * (std::fabs(difference) - rtt_deviation_us_);
    }

    for (std::uint32_t reliable_number : record.reliable_numbers)
    {
        const auto it = unacked_.find(reliable_number);
        if (it == unacked_.end())
            continue; // acknowledged before, through another transmission
        OutMessage& message = it->second;
        resend_schedule_.erase(message.schedule);
        unacked_bytes_ -= message.wire_size();
        if (message.receipt_token != 0)
            receipts_.push_back(Receipt{message.receipt_token, true});
        unacked_.erase(it);
    }
    for (std::uint32_t token : record.receipt_tokens)
        receipts_.push_back(Receipt{token, true});
    record.reliable_numbers.clear();
    record.receipt_tokens.clear();

    // The window only grows while it is actually the limiting factor.
    if (record.continuous)
    {
        const double mtu = static_cast<double>(payload_budget_);
        if (in_slow_start())
        {
            cwnd_ += mtu;
            if (ss_thresh_ != 0.0 && cwnd_ > ss_thresh_)
                cwnd_ = ss_thresh_ + mtu * mtu / cwnd_;
        }
        else if (number >= next_congestion_block_)
        {
            cwnd_ += mtu * mtu / cwnd_;
            next_congestion_block_ = next_datagram_;
        }
    }
}

void Session::on_nak(std::uint64_t number, SentDatagram& record, TimePoint now)
{
    if (record.acked)
        return;
    presume_lost(record);
    for (std::uint32_t reliable_number : record.reliable_numbers)
    {
        const auto it = unacked_.find(reliable_number);
        if (it == unacked_.end() || it->second.last_datagram != number)
            continue; // delivered meanwhile, or a newer transmission is already under way
        OutMessage& message = it->second;
        resend_schedule_.erase(message.schedule);
        message.schedule = resend_schedule_.emplace(now, reliable_number);
        message.resend_requested = true;
    }
    if (record.continuous)
        ss_thresh_ = std::max(cwnd_ / 2.0, static_cast<double>(payload_budget_));
}

// --- Session: receiving ------------------------------------------------------------------------------

void Session::handle_datagram(ByteView datagram, TimePoint now)
{
    if (timed_out_ || violated_)
        return;

    ByteReader in(datagram);
    wire::DatagramHeader header;
    if (!wire::read_datagram_header(in, header))
        return;
    last_received_ = now;
    counters_.bytes_received += datagram.size();

    if (header.kind == wire::DatagramKind::data)
    {
        handle_data(header, in, now);
        return;
    }

    std::vector<wire::Range> ranges;
    if (!wire::read_ranges(in, ranges))
        return;
    for (const wire::Range& range : ranges)
    {
        if (header.kind == wire::DatagramKind::ack)
            for_each_sent(range, [&](std::uint64_t number, SentDatagram& record) { on_acked(number, record, now); });
        else
            for_each_sent(range, [&](std::uint64_t number, SentDatagram& record) { on_nak(number, record, now); });
    }
    trim_history();
}

void Session::handle_data(const wire::DatagramHeader& header, ByteReader& in, TimePoint now)
{
    const std::uint32_t number = header.number;
    const std::uint32_t ahead = seq24::distance(expected_datagram_, number);
    if (ahead < seq24::half)
    {
        if (ahead > maximum_datagram_jump)
            return;
        // Everything skipped is reported missing (the most recent ones, if the gap is huge).
        for (std::uint32_t back = std::min(ahead, maximum_naks_per_gap); back > 0; --back)
            naks_.insert(seq24::wrap(std::uint64_t{number} + seq24::mask + 1 - back));
        expected_datagram_ = seq24::wrap(std::uint64_t{number} + 1);
    }
    else
        naks_.erase(number); // arrived late after all

    // Every data datagram is acknowledged, even if it only carried unreliable messages: the
    // acknowledgements drive the sender's round-trip estimate and congestion window.
    if (acks_.empty())
        oldest_unsent_ack_ = now;
    acks_.insert(number);

    // A message is at least 3 header bytes + 1 payload byte; anything shorter is padding.
    while (in.remaining() >= 4)
    {
        wire::MessageHeader message;
        if (!wire::read_message_header(in, message))
            break;
        const ByteView payload = in.bytes(message.payload_size());
        if (!in.ok())
            break;
        receive_message(message, payload, now);
        if (violated_)
            return;
    }
}

void Session::receive_message(const wire::MessageHeader& header, ByteView payload, TimePoint now)
{
    if (wire::is_reliable(header.reliability) && !accept_reliable(header.reliable_number))
        return;

    Bytes data;
    if (header.split)
    {
        if (!assemble(header, payload, now, data))
            return;
    }
    else
        data.assign(payload.begin(), payload.end());

    if (wire::has_ordering(header.reliability))
        order(header, std::move(data));
    else
        deliver(header.reliability, 0, std::move(data));
}

// Duplicate suppression for reliable messages: a sliding window of "still missing" flags.
bool Session::accept_reliable(std::uint32_t number)
{
    const std::uint32_t hole = seq24::distance(received_base_, number);
    if (hole == 0)
    {
        if (!holes_.empty())
            holes_.pop_front();
        received_base_ = seq24::wrap(received_base_ + 1);
    }
    else if (hole >= seq24::half)
        return false; // older than the window: a duplicate
    else if (hole < holes_.size())
    {
        if (!holes_[hole])
            return false; // duplicate
        holes_[hole] = false;
    }
    else
    {
        if (hole > maximum_reliable_hole)
            return false; // not credible; refusing also bounds the window's memory
        holes_.resize(hole, true);
        holes_.push_back(false);
    }

    while (!holes_.empty() && !holes_.front())
    {
        holes_.pop_front();
        received_base_ = seq24::wrap(received_base_ + 1);
    }
    return true;
}

bool Session::assemble(const wire::MessageHeader& header, ByteView payload, TimePoint now, Bytes& out)
{
    // Every chunk carries at least one byte, so the count alone can exceed the size limit.
    if (header.split_count > config_.max_message_size)
    {
        violated_ = true;
        return false;
    }

    Assembly& assembly = assemblies_[header.split_id];
    if (assembly.count == 0)
        assembly.count = header.split_count;
    else if (assembly.count != header.split_count)
        return false; // contradicts the chunks seen so far
    assembly.updated = now;

    if (!assembly.parts.emplace(header.split_index, Bytes(payload.begin(), payload.end())).second)
        return false; // duplicate chunk

    // Tiny chunks are charged a minimum so that their bookkeeping overhead is bounded too.
    const std::size_t charge = std::max(payload.size(), minimum_chunk_charge);
    assembly.bytes += payload.size();
    assembly.charged += charge;
    assembly_charged_ += charge;
    if (assembly.bytes > config_.max_message_size || assembly_charged_ > 4 * config_.max_message_size)
    {
        violated_ = true;
        return false;
    }
    if (assembly.parts.size() < assembly.count)
        return false;

    out.reserve(assembly.bytes);
    for (const auto& part : assembly.parts) // std::map: already in index order
        out.insert(out.end(), part.second.begin(), part.second.end());
    assembly_charged_ -= assembly.charged;
    assemblies_.erase(header.split_id);
    return true;
}

bool Session::deliver_sequenced(Channel& channel, std::uint32_t sequencing, Delivered&& message)
{
    if (seq24::less(sequencing, channel.highest_sequenced))
        return false; // a newer one was already delivered
    channel.highest_sequenced = seq24::wrap(sequencing + 1);
    delivered_.push_back(std::move(message));
    return true;
}

// Ordered and sequenced messages share one ordering index per channel: a sequenced message belongs
// "after ordered message N-1 and before ordered message N", where N is its ordering index.
void Session::order(const wire::MessageHeader& header, Bytes&& data)
{
    Channel& channel = channels_[header.ordering_channel];
    const bool sequenced = wire::is_sequenced(header.reliability);
    Delivered message{to_transport(header.reliability), header.ordering_channel, std::move(data)};

    const std::uint32_t ahead = seq24::distance(seq24::wrap(channel.ordered_read), header.ordering_index);
    if (ahead >= seq24::half)
        return; // belongs before something already delivered

    if (ahead > 0)
    {
        const OrderKey key{channel.ordered_read + ahead, sequenced ? header.sequencing_index : ordered_key};
        const std::size_t size = message.data.size() + waiting_overhead;
        if (channel.waiting.emplace(key, std::move(message)).second)
        {
            waiting_bytes_ += size;
            if (waiting_bytes_ > 4 * config_.max_message_size)
                violated_ = true;
        }
        return;
    }

    if (sequenced)
    {
        deliver_sequenced(channel, header.sequencing_index, std::move(message));
        return;
    }

    delivered_.push_back(std::move(message));
    ++channel.ordered_read;
    channel.highest_sequenced = 0;
    while (!channel.waiting.empty() && channel.waiting.begin()->first.ordering == channel.ordered_read)
    {
        const auto node = channel.waiting.begin();
        const std::uint32_t sequencing = node->first.sequencing;
        Delivered next = std::move(node->second);
        waiting_bytes_ -= next.data.size() + waiting_overhead;
        channel.waiting.erase(node);

        if (sequencing != ordered_key)
            deliver_sequenced(channel, sequencing, std::move(next));
        else
        {
            delivered_.push_back(std::move(next));
            ++channel.ordered_read;
            channel.highest_sequenced = 0;
        }
    }
}

void Session::deliver(wire::Reliability reliability, std::uint8_t channel, Bytes&& data)
{
    delivered_.push_back(Delivered{to_transport(reliability), channel, std::move(data)});
}

} // namespace detail
} // namespace raknet
} // namespace pzynet
