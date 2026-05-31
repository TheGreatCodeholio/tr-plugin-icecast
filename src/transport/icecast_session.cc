#include "transport/icecast_session.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <memory>

namespace icecast_bridge {

namespace {
constexpr int kBackoffMaxSeconds = 30;
constexpr int kSilencePrimeFrames = 16;   // prime lame's bit reservoir

// RFC 4648 base64 — small enough to inline rather than pulling boost::beast
// detail. Used only at handshake to build the Authorization header.
std::string base64_encode(const std::string& in) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) | c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tab[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tab[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// Percent-encode a query-string value (RFC 3986 unreserved set stays literal).
std::string url_encode(const std::string& in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Self-contained, fire-and-forget HTTP GET to Icecast's /admin/metadata. Owns
// its own socket so it never interferes with the persistent streaming socket,
// and keeps itself alive via shared_from_this across the async chain. Best-
// effort: any error is logged at debug and dropped (the update already reached
// the server by the time we read the response, so a read failure is harmless).
class MetadataPush : public std::enable_shared_from_this<MetadataPush> {
public:
    MetadataPush(asio::io_context& ioc, std::string host, uint16_t port,
                 std::string target, std::string auth, std::string mount)
        : resolver_(ioc), socket_(ioc), host_(std::move(host)), port_(port),
          target_(std::move(target)), auth_(std::move(auth)),
          mount_(std::move(mount)) {}

    void run() {
        auto self = shared_from_this();
        resolver_.async_resolve(host_, std::to_string(port_),
            [self](beast::error_code ec, tcp::resolver::results_type r) {
                if (ec) { self->done("meta_resolve", ec); return; }
                asio::async_connect(self->socket_, r,
                    [self](beast::error_code ec2, const tcp::endpoint&) {
                        if (ec2) { self->done("meta_connect", ec2); return; }
                        self->send();
                    });
            });
    }

private:
    void send() {
        namespace http = beast::http;
        req_.version(11);
        req_.method(http::verb::get);
        req_.target(target_);
        req_.set(http::field::host, host_ + ":" + std::to_string(port_));
        req_.set(http::field::user_agent, "tr-plugin-icecast/0.1");
        req_.set(http::field::authorization, auth_);
        req_.set(http::field::connection, "close");
        auto self = shared_from_this();
        http::async_write(socket_, req_,
            [self](beast::error_code ec, std::size_t) {
                if (ec) { self->done("meta_write", ec); return; }
                http::async_read(self->socket_, self->buf_, self->resp_,
                    [self](beast::error_code ec2, std::size_t) {
                        self->on_response(ec2);
                    });
            });
    }

    void on_response(beast::error_code ec) {
        // end_of_stream/eof is expected with Connection: close once the body is
        // read; treat it as success if we parsed a status line.
        const unsigned status = resp_.result_int();
        if (status == 0) { done("meta_read", ec); return; }

        // Icecast returns HTTP 200 even when it refuses the update; the real
        // outcome is the <message> in the iceresponse body, e.g.
        // "Metadata update successful" vs "Mountpoint will not accept URL
        // updates" (the latter means this mount needs ADMIN credentials).
        const std::string msg = extract_message(resp_.body());
        std::string lower = msg;
        for (char& c : lower) c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        const bool succeeded =
            status < 400 && lower.find("success") != std::string::npos;

        if (succeeded) {
            BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] " << mount_
                << " metadata updated: " << msg;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Icecast Bridge] " << mount_
                << " metadata update rejected (HTTP " << status << "): "
                << (msg.empty() ? "no message" : msg)
                << " - check admin_user/admin_password";
        }
        beast::error_code ignore;
        socket_.shutdown(tcp::socket::shutdown_both, ignore);
    }

    // Pull the text between <message>...</message> from an iceresponse body.
    static std::string extract_message(const std::string& body) {
        const std::string open = "<message>", close = "</message>";
        auto a = body.find(open);
        if (a == std::string::npos) return {};
        a += open.size();
        auto b = body.find(close, a);
        if (b == std::string::npos) return {};
        return body.substr(a, b - a);
    }

    void done(const char* where, beast::error_code ec) {
        BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] " << mount_
            << " metadata " << where << ": " << ec.message();
        beast::error_code ignore;
        socket_.shutdown(tcp::socket::shutdown_both, ignore);
    }

    tcp::resolver resolver_;
    tcp::socket socket_;
    std::string host_;
    uint16_t port_;
    std::string target_;
    std::string auth_;
    std::string mount_;
    beast::http::request<beast::http::empty_body> req_;
    beast::http::response<beast::http::string_body> resp_;
    beast::flat_buffer buf_;
};
}  // namespace

IcecastSession::IcecastSession(asio::io_context& ioc, Config cfg)
    : ioc_(ioc),
      cfg_(std::move(cfg)),
      resolver_(ioc),
      pacer_(ioc),
      backoff_(ioc) {
    // 1152 PCM samples per MPEG-1 L3 frame; convert to wallclock per-frame.
    // Kept at nanosecond resolution: at 22050 Hz the true period is 52.2449 ms,
    // and rounding to whole milliseconds paces the source ~0.5% fast — enough
    // to drift a listener's buffer by ~17 s over an hour.
    if (cfg_.output_sample_rate > 0) {
        frame_period_ = std::chrono::nanoseconds(
            1152LL * 1'000'000'000LL / cfg_.output_sample_rate);
    }

    // Spin up the silence encoder and prime its bit reservoir so steady-state
    // tick output is roughly constant rather than zero-then-burst.
    try {
        silence_encoder_ = std::make_unique<Mp3FrameEncoder>(
            cfg_.output_sample_rate, cfg_.bitrate_kbps, cfg_.channels);
        std::vector<int16_t> zeros(
            Mp3FrameEncoder::kFrameSamples * cfg_.channels, 0);
        for (int i = 0; i < kSilencePrimeFrames; ++i) {
            (void)silence_encoder_->encode(zeros.data(),
                                           Mp3FrameEncoder::kFrameSamples);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Icecast Bridge] " << cfg_.mountpoint
            << " silence encoder init failed: " << e.what();
    }
}

IcecastSession::~IcecastSession() = default;

void IcecastSession::start() {
    if (state_ != State::kIdle && state_ != State::kClosed) return;
    do_resolve();
}

void IcecastSession::set_frame_producer(FrameProducer p) {
    producer_ = std::move(p);
}

void IcecastSession::update_metadata(std::string title) {
    if (!cfg_.metadata_enabled) return;
    // mountpoint is config-controlled (validated to start with '/' and be
    // simple), so embed it raw; only the free-text title needs encoding.
    std::string target = "/admin/metadata?mount=" + cfg_.mountpoint +
                         "&mode=updinfo&charset=UTF-8&song=" + url_encode(title);
    std::string auth = "Basic " + base64_encode(
        cfg_.metadata_user + ":" + cfg_.metadata_password);
    std::make_shared<MetadataPush>(ioc_, cfg_.host, cfg_.port, std::move(target),
                                   std::move(auth), cfg_.mountpoint)->run();
}

void IcecastSession::close() {
    state_ = State::kClosing;
    boost::system::error_code ignore;
    pacer_.cancel(ignore);
    backoff_.cancel(ignore);
    if (socket_ && socket_->is_open()) socket_->close(ignore);
    state_ = State::kClosed;
}

// ---- connection lifecycle ---------------------------------------------------

void IcecastSession::do_resolve() {
    state_ = State::kResolving;
    auto self = shared_from_this();
    resolver_.async_resolve(cfg_.host, std::to_string(cfg_.port),
        [self](beast::error_code ec, tcp::resolver::results_type r) {
            self->on_resolve(ec, std::move(r));
        });
}

void IcecastSession::on_resolve(beast::error_code ec,
                                tcp::resolver::results_type results) {
    if (ec) { fail("resolve", ec); return; }
    state_ = State::kConnecting;
    socket_ = std::make_unique<tcp::socket>(ioc_);
    auto self = shared_from_this();
    asio::async_connect(*socket_, results,
        [self](beast::error_code ec2, tcp::resolver::results_type::endpoint_type ep) {
            self->on_connect(ec2, ep);
        });
}

void IcecastSession::on_connect(beast::error_code ec,
                                tcp::resolver::results_type::endpoint_type) {
    if (ec) { fail("connect", ec); return; }
    state_ = State::kHandshaking;
    send_source_request();
}

void IcecastSession::send_source_request() {
    namespace http = beast::http;
    req_ = std::make_unique<http::request<http::empty_body>>(
        http::verb::put, cfg_.mountpoint, 11);
    req_->set(http::field::host, cfg_.host + ":" + std::to_string(cfg_.port));
    req_->set(http::field::user_agent, "tr-plugin-icecast/0.1");
    req_->set(http::field::authorization,
              "Basic " + base64_encode(cfg_.username + ":" + cfg_.password));
    req_->set(http::field::content_type, "audio/mpeg");
    req_->set(http::field::connection, "keep-alive");
    // Icecast extension headers — listeners see these in directory listings
    // and in their player UI.
    req_->set("Ice-Public", cfg_.is_public ? "1" : "0");
    if (!cfg_.display_name.empty()) req_->set("Ice-Name", cfg_.display_name);
    if (!cfg_.description.empty()) req_->set("Ice-Description", cfg_.description);
    if (!cfg_.genre.empty())       req_->set("Ice-Genre", cfg_.genre);
    req_->set("Ice-Bitrate", std::to_string(cfg_.bitrate_kbps));
    req_->set("Ice-Audio-Info",
              "ice-samplerate=" + std::to_string(cfg_.output_sample_rate) +
              ";ice-bitrate=" + std::to_string(cfg_.bitrate_kbps) +
              ";ice-channels=" + std::to_string(cfg_.channels));

    auto self = shared_from_this();
    beast::http::async_write(*socket_, *req_,
        [self](beast::error_code ec, std::size_t n) {
            self->on_handshake_write(ec, n);
        });
}

void IcecastSession::on_handshake_write(beast::error_code ec, std::size_t) {
    if (ec) { fail("handshake_write", ec); return; }
    read_handshake_response();
}

void IcecastSession::read_handshake_response() {
    namespace http = beast::http;
    resp_parser_ = std::make_unique<http::response_parser<http::empty_body>>();
    // Icecast doesn't send Content-Length on the success response (it just
    // keeps the socket open for our streaming body). Tell the parser to stop
    // at end-of-headers rather than waiting for a body.
    resp_parser_->skip(true);

    auto self = shared_from_this();
    http::async_read_header(*socket_, read_buf_, *resp_parser_,
        [self](beast::error_code ec, std::size_t n) {
            self->on_handshake_response(ec, n);
        });
}

void IcecastSession::on_handshake_response(beast::error_code ec, std::size_t) {
    if (ec) { fail("handshake_read", ec); return; }
    const unsigned status = resp_parser_->get().result_int();
    if (status == 100) {
        // 100 Continue from servers that honor Expect: 100-continue. Read again
        // for the final 200.
        read_handshake_response();
        return;
    }
    if (status / 100 != 2) {
        BOOST_LOG_TRIVIAL(error) << "[Icecast Bridge] " << cfg_.mountpoint
            << " server rejected source connection: HTTP " << status;
        beast::error_code synthetic;
        fail("handshake_status", synthetic);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "[Icecast Bridge] " << cfg_.mountpoint
        << " connected, streaming";
    backoff_attempt_ = 0;          // success — reset reconnect backoff
    state_ = State::kStreaming;
    req_.reset();
    resp_parser_.reset();
    start_pacer();
}

// ---- pacer + write loop -----------------------------------------------------

void IcecastSession::start_pacer() {
    next_tick_ = std::chrono::steady_clock::now();
    on_pacer_tick({});
}

void IcecastSession::on_pacer_tick(const boost::system::error_code& ec) {
    if (ec) return;
    if (state_ != State::kStreaming) return;

    std::vector<uint8_t> frame;
    if (producer_) frame = producer_();
    if (frame.empty() && silence_encoder_) {
        // Encode one silent frame to keep listeners' players from underrunning.
        try {
            std::vector<int16_t> zeros(
                Mp3FrameEncoder::kFrameSamples * cfg_.channels, 0);
            frame = silence_encoder_->encode(zeros.data(),
                                             Mp3FrameEncoder::kFrameSamples);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "[Icecast Bridge] " << cfg_.mountpoint
                << " silence encode failed: " << e.what();
        }
    }
    if (!frame.empty()) write_frame(std::move(frame));

    next_tick_ += frame_period_;
    pacer_.expires_at(next_tick_);
    auto self = shared_from_this();
    pacer_.async_wait([self](const boost::system::error_code& ec2) {
        self->on_pacer_tick(ec2);
    });
}

void IcecastSession::write_frame(std::vector<uint8_t> bytes) {
    write_queue_.push_back(std::move(bytes));
    if (write_in_flight_) return;
    write_in_flight_ = true;
    auto self = shared_from_this();
    asio::async_write(*socket_, asio::buffer(write_queue_.front()),
        [self](beast::error_code ec, std::size_t n) { self->on_write(ec, n); });
}

void IcecastSession::on_write(beast::error_code ec, std::size_t) {
    if (ec) { fail("write", ec); return; }
    write_queue_.pop_front();
    write_in_flight_ = false;
    if (write_queue_.empty()) return;
    write_in_flight_ = true;
    auto self = shared_from_this();
    asio::async_write(*socket_, asio::buffer(write_queue_.front()),
        [self](beast::error_code ec2, std::size_t n) { self->on_write(ec2, n); });
}

// ---- failure + reconnect ----------------------------------------------------

void IcecastSession::fail(const std::string& where, beast::error_code ec) {
    BOOST_LOG_TRIVIAL(warning) << "[Icecast Bridge] " << cfg_.mountpoint
        << " " << where << ": " << ec.message();
    boost::system::error_code ignore;
    pacer_.cancel(ignore);
    if (socket_ && socket_->is_open()) socket_->close(ignore);
    write_queue_.clear();
    write_in_flight_ = false;
    if (state_ == State::kClosing || state_ == State::kClosed) return;
    schedule_reconnect();
}

void IcecastSession::schedule_reconnect() {
    state_ = State::kBackoff;
    int delay = 1 << std::min(backoff_attempt_, 5);   // 1,2,4,8,16,32 -> capped
    if (delay > kBackoffMaxSeconds) delay = kBackoffMaxSeconds;
    backoff_attempt_++;
    backoff_.expires_after(std::chrono::seconds(delay));
    auto self = shared_from_this();
    backoff_.async_wait([self](const boost::system::error_code& ec) {
        self->on_backoff_elapsed(ec);
    });
}

void IcecastSession::on_backoff_elapsed(const boost::system::error_code& ec) {
    if (ec) return;
    if (state_ != State::kBackoff) return;
    state_ = State::kIdle;
    do_resolve();
}

}  // namespace icecast_bridge