/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "bytes/details/io_fragment.h"
#include "bytes/iobuf.h"
#include "http/chunk_encoding.h"
#include "http/iobuf_body.h"
#include "http/logger.h"
#include "rpc/transport.h"
#include "rpc/types.h"
#include "seastarx.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/timer.hh>

#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/optional/optional.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <stdexcept>
#include <string>

namespace http {

using http_response
  = boost::beast::http::response<boost::beast::http::string_body>;
using http_request
  = boost::beast::http::request<boost::beast::http::string_body>;
using http_serializer
  = boost::beast::http::request_serializer<boost::beast::http::string_body>;

/// Http client
class client : protected rpc::base_transport {
    enum {
        protocol_version = 11,
    };

public:
    using request_header = boost::beast::http::request_header<>;
    using response_header = boost::beast::http::response_header<>;
    using response_parser = boost::beast::http::response_parser<iobuf_body>;
    using field = boost::beast::http::field;
    using verb = boost::beast::http::verb;

    explicit client(const rpc::base_transport::configuration& cfg);
    client(
      const rpc::base_transport::configuration& cfg,
      const ss::abort_source& as);

    ss::future<> shutdown();

    // Response state machine
    class response_stream final
      : public ss::enable_shared_from_this<response_stream> {
    public:
        /// C-tor can only be called by http_request
        explicit response_stream(client* client);

        response_stream(response_stream&&) = delete;
        response_stream(response_stream const&) = delete;
        response_stream& operator=(response_stream const&) = delete;
        response_stream operator=(response_stream&&) = delete;
        ~response_stream() override = default;

        /// \brief Shutdown connection gracefully
        ss::future<> shutdown();

        /// Return true if the whole http payload is received and parsed
        bool is_done() const;

        /// Return true if the header parsing is done
        bool is_header_done() const;

        /// Access response headers (should only be called if is_headers_done()
        /// == true)
        response_header const& get_headers() const;

        /// Prefetch HTTP headers. Read data from the socket until the header is
        /// received and parsed (is_headers_done = true).
        ///
        /// \return future that becomes ready when the header is received
        ss::future<> prefetch_headers();

        /// Recv new portion of the payload, this method should be called untill
        /// is_done returns false. It's possible to get an empty iobuf which
        /// should be ignored (deosn't mean EOF).
        /// The method doesn't return bytes that belong to HTTP header or chunk
        /// headers.
        ///
        /// \return future that becomes ready when the next portion of data is
        /// received
        ss::future<iobuf> recv_some();

        /// Returns input_stream that can be used to fetch response body.
        /// Can be used instead of 'recv_some'.
        ss::input_stream<char> as_input_stream();

    private:
        client* _client;
        response_parser _parser;
        iobuf _buffer; /// store incomplete tail elements
        iobuf _prefetch;
    };

    using response_stream_ref = ss::shared_ptr<response_stream>;

    // Request state machine
    // can only be created by the http_client
    class request_stream final
      : public ss::enable_shared_from_this<request_stream> {
    public:
        explicit request_stream(client* client, request_header&& hdr);

        request_stream(request_stream&&) = delete;
        request_stream(request_stream const&) = delete;
        request_stream& operator=(request_stream const&) = delete;
        request_stream operator=(request_stream&&) = delete;
        ~request_stream() override = default;

        /// Send data, if heders weren't sent they should be sent first
        /// followed by the data. BufferSeq is supposed to be an iobuf
        /// or temporary_buffer<char>.
        ss::future<> send_some(iobuf&& seq);
        ss::future<> send_some(ss::temporary_buffer<char>&& buf);

        // True if done, false otherwise
        bool is_done();

        // Wait until remaining data will be transmitted
        ss::future<> send_eof();

        /// Returns output_stream that can be used to send request headers and
        /// the body. Can be used instead of 'send_some' and 'send_eof'.
        ss::output_stream<char> as_output_stream();

    private:
        client* _client;
        http_request _request;
        http_serializer _serializer;
        chunked_encoder _chunk_encode;
        ss::gate _gate;

        enum {
            max_chunk_size = 32_KiB,
        };
    };

    using request_stream_ref = ss::shared_ptr<request_stream>;
    using request_response_t
      = std::tuple<request_stream_ref, response_stream_ref>;

    // Make http_request, if the transport is not yet connected it will connect
    // first otherwise the future will resolve immediately.
    ss::future<request_response_t> make_request(request_header&& header);

    /// Utility function that executes request with the body and returns
    /// stream. Returned future becomes ready when the body is sent.
    /// Using the stream returned by the future client can pull response.
    ///
    /// \param header is a prepred request header
    /// \param input in an input stream that contains request body octets
    /// \param limits is a set of limitation for a query
    /// \returns response stream future
    ss::future<response_stream_ref>
    request(request_header&& header, ss::input_stream<char>& input);
    ss::future<response_stream_ref> request(request_header&& header);

private:
    template<class BufferSeq>
    static ss::future<>
    forward(rpc::batched_output_stream& stream, BufferSeq&& seq);

    /// Throw exception if _as is aborted
    void check() const;

    const ss::abort_source* _as;
};

template<class BufferSeq>
inline ss::future<>
client::forward(rpc::batched_output_stream& stream, BufferSeq&& seq) {
    auto scattered = iobuf_as_scattered(std::forward<BufferSeq>(seq));
    return stream.write(std::move(scattered));
}
} // namespace http