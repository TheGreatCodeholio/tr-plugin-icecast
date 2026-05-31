#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codec/mp3_encoder.h"

namespace icecast_bridge {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

// Per-mountpoint persistent source-client connection to icecast2.
//
// Wire protocol: HTTP/1.1 PUT (preferred) or SOURCE (legacy) to /<mount>, with
//   Authorization: Basic <user:pass>
//   Content-Type: audio/mpeg
//   Ice-Public, Ice-Name, Ice-Description, Ice-Genre, Ice-Bitrate, Ice-Audio-Info
//   Connection: keep-alive
//   Transfer-Encoding: chunked   (or no body length; icecast tolerates streaming)
// Once the 200 response lands, the connection is left open and we stream MP3
// frames into it indefinitely.
//
// Stay-connected strategy:
//   - Always-on pacer timer ticks every (1152 / output_sample_rate) seconds.
//   - When no call is active, the pacer encodes a silent MP3 frame via
//     silence_encoder_ and writes it to keep the bytestream alive.
//   - When a call IS active, pacer pulls one encoded frame from the call's
//     drain function and writes that instead.
//   - On socket error: tear down, exponential backoff (1s, 2s, 4s, ..., 30s
//     cap), reconnect, resume pacing. Audio in flight during disconnect is
//     dropped (live source — no replay).
//
// All public methods must be called from the asio thread that runs the owning
// io_context.
class IcecastSession : public std::enable_shared_from_this<IcecastSession> {
public:
    struct Config {
        std::string host;
        uint16_t port = 8000;
        std::string mountpoint;          // e.g. "/dispatch.mp3"
        std::string username = "source";
        std::string password;
        // Stream advertised in headers / icecast metadata
        std::string display_name;
        std::string description;
        std::string genre = "Public Safety";
        bool is_public = false;
        // Encoder params (must match the MP3 frames we feed it)
        uint32_t output_sample_rate = 22050;
        uint32_t bitrate_kbps = 64;
        int channels = 1;

        // "Now playing" metadata (pushed out-of-band to /admin/metadata).
        bool metadata_enabled = true;
        // Title template rendered per active call by the plugin. Supports the
        // tokens ${TALKGROUP} ${TALKGROUP_TAG} ${TAG} ${SYSTEM} ${FREQ} ${TIME}.
        std::string title_template;
        // Title pushed when the mount goes idle (no active call). Empty = leave
        // the previous title untouched.
        std::string idle_title;
        // Credentials for the /admin/metadata request. Default to the source
        // credentials above; override if your Icecast requires admin creds.
        std::string metadata_user = "source";
        std::string metadata_password;
    };

    // Source for the next MP3 frame to write on the wire. Returns the frame
    // bytes; an empty vector means "nothing to send this tick" (rare — pacer
    // should always have at least silence). Called once per pacer tick on the
    // asio thread.
    using FrameProducer = std::function<std::vector<uint8_t>()>;

    IcecastSession(asio::io_context& ioc, Config cfg);
    ~IcecastSession();

    IcecastSession(const IcecastSession&) = delete;
    IcecastSession& operator=(const IcecastSession&) = delete;

    const std::string& mountpoint() const { return cfg_.mountpoint; }
    bool is_connected() const { return state_ == State::kStreaming; }

    // Begins async connect → HTTP handshake → streaming. Idempotent.
    void start();

    // Install/replace the frame producer. The pacer calls it on every tick.
    // Pass nullptr to fall back to internal silence generator.
    void set_frame_producer(FrameProducer p);

    // Push a "now playing" title to Icecast via a short-lived, out-of-band HTTP
    // GET to /admin/metadata (does NOT touch the streaming socket). Best-effort:
    // failures are logged, never fatal. Must be called on the asio thread.
    void update_metadata(std::string title);

    // Request graceful shutdown.
    void close();

private:
    enum class State {
        kIdle, kResolving, kConnecting, kHandshaking, kStreaming,
        kBackoff, kClosing, kClosed
    };

    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void send_source_request();
    void on_handshake_write(beast::error_code ec, std::size_t);
    void read_handshake_response();
    void on_handshake_response(beast::error_code ec, std::size_t);

    void start_pacer();
    void on_pacer_tick(const boost::system::error_code& ec);
    void write_frame(std::vector<uint8_t> bytes);
    void on_write(beast::error_code ec, std::size_t);

    void schedule_reconnect();
    void on_backoff_elapsed(const boost::system::error_code& ec);

    void fail(const std::string& where, beast::error_code ec);

    asio::io_context& ioc_;
    Config cfg_;
    tcp::resolver resolver_;
    std::unique_ptr<tcp::socket> socket_;
    beast::flat_buffer read_buf_;
    asio::steady_timer pacer_;
    asio::steady_timer backoff_;

    FrameProducer producer_;
    // Encoder dedicated to producing silent MP3 frames when no call is active.
    // Persistent (not reset between gaps) so the bit reservoir stays primed and
    // each tick yields ~constant byte count.
    std::unique_ptr<Mp3FrameEncoder> silence_encoder_;
    std::deque<std::vector<uint8_t>> write_queue_;
    bool write_in_flight_ = false;

    // beast::http handshake artifacts. Held as members so they outlive the
    // async_write/async_read callbacks.
    std::unique_ptr<beast::http::request<beast::http::empty_body>> req_;
    std::unique_ptr<beast::http::response_parser<beast::http::empty_body>> resp_parser_;

    std::chrono::nanoseconds frame_period_{0};    // 1152 / output_sample_rate
    std::chrono::steady_clock::time_point next_tick_{};
    int backoff_attempt_ = 0;

    State state_ = State::kIdle;
};

}  // namespace icecast_bridge