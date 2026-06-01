#include "transport/icecast_session.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <memory>
#include <sstream>

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

// Percent-encode a string for use in a URL query parameter value.
// Encodes everything except unreserved characters (RFC 3986).
std::string url_encode(const std::string& in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}
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

void IcecastSession::set_metadata(const std::string& title) {
    // Out-of-band metadata update via Icecast's admin endpoint.
    // Opens a short-lived TCP connection, sends one HTTP GET, reads the
    // response header, then closes. Errors are logged and swallowed — a
    // metadata failure must never affect the streaming connection.
    //
    // URL: GET /admin/metadata?mount=<mount>&mode=updinfo&song=<title>
    // Authorization: Basic <admin_user:admin_password>
    //
    // Note: Icecast's /admin/metadata endpoint requires *admin* credentials,
    // not source credentials. We reuse the source password here because many
    // self-hosted deployments set them the same; if yours differ, add an
    // admin_password field to Config.
    namespace http = beast::http;

    const std::string target =
        "/admin/metadata?mount=" + url_encode(cfg_.mountpoint) +
        "&mode=updinfo&song=" + url_encode(title);

    // Capture everything needed by value so the lambda outlives this call.
    auto self = shared_from_this();
    auto meta_socket = std::make_shared<tcp::socket>(ioc_);
    auto meta_resolver = std::make_shared<tcp::resolver>(ioc_);
    const std::string host = cfg_.host;
    const std::string port_str = std::to_string(cfg_.port);
    // Build admin auth before entering the lambda chain — cfg_ is not
    // accessible inside deeply nested lambdas that don't capture this.
    const std::string& admin_pw = cfg_.admin_password.empty()
                                      ? cfg_.password
                                      : cfg_.admin_password;
    const std::string auth = base64_encode(cfg_.admin_user + ":" + admin_pw);
    const std::string mountpoint = cfg_.mountpoint;

    meta_resolver->async_resolve(host, port_str,
        [self, meta_socket, meta_resolver, host, port_str, target, auth, mountpoint]
        (beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) {
                BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] " << mountpoint
                    << " metadata resolve failed: " << ec.message();
                return;
            }
            asio::async_connect(*meta_socket, results,
                [self, meta_socket, meta_resolver, host, port_str, target, auth, mountpoint]
                (beast::error_code ec2, tcp::resolver::results_type::endpoint_type) {
                    if (ec2) {
                        BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] " << mountpoint
                            << " metadata connect failed: " << ec2.message();
                        return;
                    }
                    // Build and send the request.
                    auto req = std::make_shared<http::request<http::empty_body>>(
                        http::verb::get, target, 11);
                    req->set(http::field::host, host + ":" + port_str);
                    req->set(http::field::authorization, "Basic " + auth);
                    req->set(http::field::user_agent, "tr-plugin-icecast/0.1");
                    req->set(http::field::connection, "close");

                    http::async_write(*meta_socket, *req,
                        [self, meta_socket, meta_resolver, req, mountpoint]
                        (beast::error_code ec3, std::size_t) {
                            if (ec3) {
                                BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] " << mountpoint
                                    << " metadata write failed: " << ec3.message();
                                return;
                            }
                            // Read and discard the response.
                            auto buf = std::make_shared<beast::flat_buffer>();
                            auto resp = std::make_shared<
                                http::response_parser<http::string_body>>();
                            http::async_read(*meta_socket, *buf, *resp,
                                [self, meta_socket, buf, resp, mountpoint]
                                (beast::error_code ec4, std::size_t) {
                                    if (ec4 && ec4 != beast::http::error::end_of_stream) {
                                        BOOST_LOG_TRIVIAL(debug) << "[Icecast Bridge] "
                                            << mountpoint
                                            << " metadata response error: "
                                            << ec4.message();
                                    }
                                    beast::error_code ignore;
                                    meta_socket->shutdown(
                                        tcp::socket::shutdown_both, ignore);
                                });
                        });
                });
        });
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

    if (cfg_.legacy_source) {
        // Legacy Shoutcast/Icecast 1.x SOURCE method — used by Broadcastify
        // and other older ingest servers that don't accept HTTP PUT.
        std::ostringstream ss;
        ss << "SOURCE " << cfg_.mountpoint << " HTTP/1.0\r\n"
           << "Authorization: Basic "
           << base64_encode(cfg_.username + ":" + cfg_.password) << "\r\n"
           << "User-Agent: tr-plugin-icecast/0.1\r\n"
           << "Content-Type: audio/mpeg\r\n"
           << "ice-name: " << cfg_.display_name << "\r\n"
           << "ice-description: " << cfg_.description << "\r\n"
           << "ice-genre: " << cfg_.genre << "\r\n"
           << "ice-bitrate: " << cfg_.bitrate_kbps << "\r\n"
           << "ice-public: " << (cfg_.is_public ? "1" : "0") << "\r\n"
           << "ice-audio-info: ice-samplerate=" << cfg_.output_sample_rate
           << ";ice-bitrate=" << cfg_.bitrate_kbps
           << ";ice-channels=" << cfg_.channels << "\r\n"
           << "\r\n";
        legacy_handshake_buf_ = ss.str();
        auto self = shared_from_this();
        asio::async_write(*socket_,
            asio::buffer(legacy_handshake_buf_),
            [self](beast::error_code ec, std::size_t n) {
                self->on_handshake_write(ec, n);
            });
        return;
    }

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

    if (cfg_.legacy_source) {
        // Legacy servers respond with "OK" (not valid HTTP). Read up to the
        // first \n and check for OK.
        auto self = shared_from_this();
        asio::async_read_until(*socket_, read_buf_, '\n',
            [self](beast::error_code ec, std::size_t) {
                if (ec) { self->fail("handshake_read", ec); return; }
                std::string line{
                    asio::buffers_begin(self->read_buf_.data()),
                    asio::buffers_end(self->read_buf_.data())};
                self->read_buf_.consume(self->read_buf_.size());
                if (line.find("OK") == std::string::npos) {
                    BOOST_LOG_TRIVIAL(error) << "[Icecast Bridge] "
                        << self->cfg_.mountpoint
                        << " legacy server rejected connection: " << line;
                    beast::error_code synthetic;
                    self->fail("handshake_status", synthetic);
                    return;
                }
                BOOST_LOG_TRIVIAL(info) << "[Icecast Bridge] "
                    << self->cfg_.mountpoint << " connected (legacy), streaming";
                self->backoff_attempt_ = 0;
                self->state_ = State::kStreaming;
                self->start_pacer();
            });
        return;
    }

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
