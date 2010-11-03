/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_SERVER_H
#define RAMCLOUD_SERVER_H

#include "Common.h"
#include "ClientException.h"
#include "Metrics.h"
#include "Rpc.h"
#include "TransportManager.h"

namespace RAMCloud {

/**
 * A base class for RPC servers. Although this class is meant to be subclassed,
 * it serves PINGs so you can use it as a placeholder to aid in development.
 */
class Server {
  public:

    /**
     * A really annoying class that lets you respond to an RPC.
     * It makes sure the response is only sent once and that the counterValue
     * is set.
     */
    class Responder {
      public:
        explicit Responder(Transport::ServerRpc& rpc) : rpc(&rpc) {}

        bool hasResponded() {
            return (rpc == NULL);
        }

        /// Respond to the RPC.
        void operator()() {
            RpcResponseCommon* responseCommon = const_cast<RpcResponseCommon*>(
                rpc->replyPayload.getStart<RpcResponseCommon>());
            if (responseCommon == NULL) {
                responseCommon =
                    new(&rpc->replyPayload, APPEND) RpcResponseCommon;
                responseCommon->status = STATUS_RESPONSE_FORMAT_ERROR;
            }
            Metrics::mark(MARK_RPC_PROCESSING_END);
            responseCommon->counterValue = Metrics::read();
            rpc->sendReply();
            rpc = NULL;
        }

        /// Respond to the RPC with the given status.
        void operator()(Status status) {
            RpcResponseCommon* responseCommon = const_cast<RpcResponseCommon*>(
                rpc->replyPayload.getStart<RpcResponseCommon>());
            if (responseCommon == NULL) {
                responseCommon =
                    new(&rpc->replyPayload, APPEND) RpcResponseCommon;
            }
            responseCommon->status = status;
            Metrics::mark(MARK_RPC_PROCESSING_END);
            responseCommon->counterValue = Metrics::read();
            rpc->sendReply();
            rpc = NULL;
        }

      private:
        Transport::ServerRpc* rpc;
        DISALLOW_COPY_AND_ASSIGN(Responder);
    };

    Server() {}
    virtual ~Server() {}
    virtual void run();
    VIRTUAL_FOR_TESTING void dispatch(RpcType type,
                                      Transport::ServerRpc& rpc,
                                      Responder& responder);

    void ping(const PingRpc::Request& reqHdr,
              PingRpc::Response& respHdr,
              Transport::ServerRpc& rpc);

  protected:
    const char*
    getString(Buffer& buffer, uint32_t offset, uint32_t length) const;


    /**
     * Helper function to be used in dispatch.
     * Extracts the request from the RPC, allocates and zeros space for the
     * response, and calls the handler.
     * \tparam Rpc
     *      An RPC struct (e.g., PingRpc).
     * \tparam S
     *      The class which defines \a handler and is a subclass of Server.
     * \tparam handler
     *      The method of \a S which executes an RPC.
     */
    template <typename Rpc, typename S,
              void (S::*handler)(const typename Rpc::Request&,
                                 typename Rpc::Response&,
                                 Transport::ServerRpc&)>
    void
    callHandler(Transport::ServerRpc& rpc) {
        assert(rpc.replyPayload.getTotalLength() == 0);
        const typename Rpc::Request* reqHdr =
            rpc.recvPayload.getStart<typename Rpc::Request>();
        if (reqHdr == NULL)
            throw MessageTooShortError();
        typename Rpc::Response* respHdr =
            new(&rpc.replyPayload, APPEND) typename Rpc::Response;
        /* Clear the response header, so that unused fields are zero;
         * this makes tests more reproducible, and it is also needed
         * to avoid possible security problems where random server
         * info could leak out to clients through unused packet
         * fields. */
        memset(respHdr, 0, sizeof(*respHdr));
        (static_cast<S*>(this)->*handler)(*reqHdr, *respHdr, rpc);
    }

    /**
     * Almost identical to callHandler above,
     * but \a handler takes a Responder as an argument.
     * The responder is a functor used to reply to the RPC before returning
     * from the handler. This is useful for avoiding some deadlock situations
     * between different single-threaded servers. After responding, the
     * request, response, and RPC parameters are no longer safe to access.
     */
    template <typename Rpc, typename S,
              void (S::*handler)(const typename Rpc::Request&,
                                 typename Rpc::Response&,
                                 Transport::ServerRpc&,
                                 Responder&)>
    void
    callHandler(Transport::ServerRpc& rpc, Responder& responder) {
        assert(rpc.replyPayload.getTotalLength() == 0);
        const typename Rpc::Request* reqHdr =
            rpc.recvPayload.getStart<typename Rpc::Request>();
        if (reqHdr == NULL)
            throw MessageTooShortError();
        typename Rpc::Response* respHdr =
            new(&rpc.replyPayload, APPEND) typename Rpc::Response;
        memset(respHdr, 0, sizeof(*respHdr));
        (static_cast<S*>(this)->*handler)(*reqHdr, *respHdr, rpc, responder);
    }

    /**
     * Wait for an incoming RPC request, dispatch it, and send a response.
     */
    template<typename S>
    void
    handleRpc() {
        Transport::ServerRpc& rpc(*transportManager.serverRecv());
        Responder responder(rpc);
        const RpcRequestCommon* header;
        header = rpc.recvPayload.getStart<RpcRequestCommon>();
        if (header == NULL) {
            responder(STATUS_MESSAGE_TOO_SHORT);
            return;
        }
        Metrics::setup(header->perfCounter);
        Metrics::mark(MARK_RPC_PROCESSING_BEGIN);
        try {
            static_cast<S*>(this)->dispatch(header->type, rpc, responder);
        } catch (ClientException& e) {
            if (responder.hasResponded())
                throw;
            responder(e.status);
            return;
        }
        if (!responder.hasResponded())
            responder();
    }

  private:
    friend class ServerTest;
    friend class BindTransport;
    DISALLOW_COPY_AND_ASSIGN(Server);
};


} // end RAMCloud

#endif  // RAMCLOUD_SERVER_H
