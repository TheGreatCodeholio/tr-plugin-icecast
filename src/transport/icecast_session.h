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

class IcecastSession : public std::enable_shared_from_this<IcecastSession> {
public:
    struct Config {
        std::string host;
        uint16_t port = 8000;
        std::string mountpoint;
        std::string username = "source";
        std::string password;
        std::string display_name;
        std::string description;
        std::string genre = "Public Safety";
        bool is_public = false;
        uint32_t output_sample_rate = 22050;
        uint32_t bitrate_kbps = 64;
        int channels = 1;
        float gain = 1.0f;
        // Credentials for the /admin/metadata endpoint. Icecast requires the
        // admin user (default "admin"), not the source user, for this endpoint.
        // admin_user defaults to "admin"; admin_password defaults to password.
        std::string admin_user = "admin";
        std::string admin_password;
        // Use legacy SOURCE method instead of HTTP PUT for Shoutcast/old
        // Icecast 1.x servers (e.g. Broadcastify). Sends:
        //   SOURCE /mount HTTP/1.0\r\n
        //   Authorization: Basic <base64>\r\n
        //   ...\r\n\r\n
        // instead of a standard HTTP PUT request.
        bool legacy_source = false;
        //   {talkgroup_display}, {talkgroup}, {talker_alias}, {time},
        //   {short_name}, {freq}
        // {talker_alias} resolves to the unit tag if found, the numeric src ID
        // if not, and collapses with surrounding whitespace if src ID is also 0.
        std::string metadata_format  = "TG: {talkgroup_tag} ({talkgroup}) {talker_alias} {time}";
        // Stream title pushed when no call is active on this mount.
        std::string metadata_standby = "Standby";
    };

    using FrameProducer = std::function<std::vector<uint8_t>()>;

    IcecastSession(asio::io_context& ioc, Config cfg);
    ~IcecastSession();

    IcecastSession(const IcecastSession&) = delete;
    IcecastSession& operator=(const IcecastSession&) = delete;

    const std::string& mountpoint() const { return cfg_.mountpoint; }
    bool is_connected() const { return state_ == State::kStreaming; }

    void start();
    void set_frame_producer(FrameProducer p);

    // Push an ICY StreamTitle update to Icecast via the out-of-band admin
    // metadata endpoint: GET /admin/metadata?mount=<mount>&mode=updinfo&song=<title>
    // Fire-and-forget; any connection error is logged and ignored.
    void set_metadata(const std::string& title);

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
    std::unique_ptr<Mp3FrameEncoder> silence_encoder_;
    std::deque<std::vector<uint8_t>> write_queue_;
    bool write_in_flight_ = false;

    std::unique_ptr<beast::http::request<beast::http::empty_body>> req_;
    std::unique_ptr<beast::http::response_parser<beast::http::empty_body>> resp_parser_;
    std::string legacy_handshake_buf_;  // raw SOURCE request buffer for legacy mode

    std::chrono::nanoseconds frame_period_{0};
    std::chrono::steady_clock::time_point next_tick_{};
    int backoff_attempt_ = 0;

    State state_ = State::kIdle;
};

}  // namespace icecast_bridge
