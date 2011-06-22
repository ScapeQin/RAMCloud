/* Copyright (c) 2010-2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * Implementation of an Infiniband reliable transport layer using reliable
 * connected queue pairs. Handshaking is done over IP/UDP and addressing
 * is based on that, i.e. addresses look like normal IP/UDP addresses
 * because the infiniband queue pair set up is bootstrapped over UDP.
 *
 * The transport uses common pools of receive and transmit buffers that
 * are pre-registered with the HCA for direct access. All receive buffers
 * are placed on two shared receive queues (one for issuing RPCs and one for
 * servicing RPCs), which avoids having to allocate buffers to individual
 * receive queues for each client queue pair (this would be costly for many
 * queue pairs, and wasteful if they're idle). The shared receive queues can be
 * associated with many queue pairs, and each shared receive queue has its own
 * completion queue.
 *
 * In short, the receive path looks like the following:
 *  - As a server, we have just one completion queue for all incoming client
 *    queue pairs.
 *  - As a client, we have just one completion queue for all outgoing client
 *    queue pairs.
 *
 * For the transmit path, we have one completion queue for all cases, since
 * we currently do synchronous sends.
 *
 * Each receive and transmit buffer is sized large enough for the maximum
 * possible RPC size for simplicity. Note that if a node sends to another node
 * that does not have a sufficiently large receive buffer at the head of its
 * receive queue, _both_ ends will get an error (IBV_WC_REM_INV_REQ_ERR on the
 * sender, and IBV_WC_LOC_LEN_ERR on the receiver)! The HCA will _not_ search
 * the receive queue to find a larger posted buffer, nor will it scatter the
 * incoming data over multiple posted buffers. You have been warned.
 *
 * To reference the buffer associated with each work queue element on the shared
 * receive queue, we stash pointers in the 64-bit `wr_id' field of the work
 * request.
 *
 * Connected queue pairs require some bootstrapping, which we do as follows:
 *  - The server maintains a UDP listen port.
 *  - Clients establish QPs by sending their tuples to the server as a request.
 *    Tuples are basically (address, queue pair number, sequence number),
 *    similar to TCP. Think of this as TCP's SYN packet.
 *  - Servers receive client tuples, create an associated queue pair, and
 *    reply via UDP with their QP's tuple. Think of this as TCP's SYN/ACK.
 *  - Clients receive the server's tuple reply and complete their queue pair
 *    setup. Communication over infiniband is ready to go. 
 *
 * Of course, using UDP means these things can get lost. We should have a
 * mechanism for cleaning up halfway-completed QPs that occur when clients
 * die before completing or never get the server's UDP response. Similarly,
 * clients right now block forever if the request is lost. They should time
 * out and retry, although at what level retries should occur isn't clear.
 */

/*
 * Random Notes:
 *  1) ibv_reg_mr() takes about 30usec to register one 4k page on the E5620.
 *     8MB takes about 1.25msec.  This implies that we can't afford to register
 *     on the fly.
 */

#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/scoped_ptr.hpp>

#include "Common.h"
#include "CycleCounter.h"
#include "Metrics.h"
#include "BenchUtil.h"
#include "TimeCounter.h"
#include "Transport.h"
#include "InfRcTransport.h"
#include "IpAddress.h"
#include "ServiceLocator.h"
#include "ServiceManager.h"

#define check_error_null(x, s)                              \
    do {                                                    \
        if ((x) == NULL) {                                  \
            LOG(ERROR, "%s", s);                            \
            throw TransportException(HERE, errno);          \
        }                                                   \
    } while (0)

namespace RAMCloud {

//------------------------------
// InfRcTransport class
//------------------------------

/**
 * Construct a InfRcTransport.
 *
 * \param sl
 *      The ServiceLocator describing which HCA to use and the IP/UDP
 *      address and port numbers to use for handshaking. If NULL,
 *      the transport will be configured for client use only.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::InfRcTransport(const ServiceLocator *sl)
    : realInfiniband(),
      infiniband(),
      rxBuffers(),
      txBuffers(),
      freeTxBuffers(),
      serverSrq(NULL),
      clientSrq(NULL),
      serverRxCq(NULL),
      clientRxCq(NULL),
      commonTxCq(NULL),
      ibPhysicalPort(1),
      lid(0),
      serverSetupSocket(-1),
      clientSetupSocket(-1),
      queuePairMap(),
      clientSendQueue(),
      numUsedClientSrqBuffers(MAX_SHARED_RX_QUEUE_DEPTH),
      outstandingRpcs(),
      clientRpcsActiveTime(),
      locatorString(),
      poller(this),
      serverConnectHandler(),
      logMemoryBase(0),
      logMemoryBytes(0),
      logMemoryRegion(0)
{
    const char *ibDeviceName = NULL;

    if (sl != NULL) {
        locatorString = sl->getOriginalString();

        try {
            ibDeviceName   = sl->getOption<const char *>("dev");
        } catch (ServiceLocator::NoSuchKeyException& e) {}

        try {
            ibPhysicalPort = sl->getOption<int>("devport");
        } catch (ServiceLocator::NoSuchKeyException& e) {}
    }

    infiniband = realInfiniband.construct(ibDeviceName);

    // Step 1:
    //  Set up the udp sockets we use for out-of-band infiniband handshaking.

    // For clients, the kernel will automatically assign a dynamic port on
    // first use.
    clientSetupSocket = socket(PF_INET, SOCK_DGRAM, 0);
    if (clientSetupSocket == -1) {
        LOG(ERROR, "failed to create client socket");
        throw TransportException(HERE, "client socket failed");
    }
    try {
        setNonBlocking(clientSetupSocket);
    } catch (...) {
        close(clientSetupSocket);
        throw;
    }

    // If this is a server, create a server setup socket and bind it.
    if (sl != NULL) {
        IpAddress address(*sl);

        serverSetupSocket = socket(PF_INET, SOCK_DGRAM, 0);
        if (serverSetupSocket == -1) {
            LOG(ERROR, "failed to create server socket");
            throw TransportException(HERE, "server socket failed");
        }

        if (bind(serverSetupSocket, &address.address,
          sizeof(address.address))) {
            close(serverSetupSocket);
            serverSetupSocket = -1;
            LOG(ERROR, "failed to bind socket");
            throw TransportException(HERE, "socket failed");
        }

        try {
            setNonBlocking(serverSetupSocket);
        } catch (...) {
            close(serverSetupSocket);
            serverSetupSocket = -1;
            throw;
        }

        LOG(NOTICE, "InfRc listening on UDP: %s", address.toString().c_str());
        serverConnectHandler.construct(serverSetupSocket, this);
    }

    // Step 2:
    //  Set up the initial verbs necessities: open the device, allocate
    //  protection domain, create shared receive queue, register buffers.

    lid = infiniband->getLid(ibPhysicalPort);

    // create two shared receive queues. all client queue pairs use one and all
    // server queue pairs use the other. we post receive buffer work requests
    // to these queues only. the motiviation is to avoid having to post at
    // least one buffer to every single queue pair (we may have thousands of
    // them with megabyte buffers).
    serverSrq = infiniband->createSharedReceiveQueue(MAX_SHARED_RX_QUEUE_DEPTH,
                                                     MAX_SHARED_RX_SGE_COUNT);
    check_error_null(serverSrq,
                     "failed to create server shared receive queue");
    clientSrq = infiniband->createSharedReceiveQueue(MAX_SHARED_RX_QUEUE_DEPTH,
                                                     MAX_SHARED_RX_SGE_COUNT);
    check_error_null(clientSrq,
                     "failed to create client shared receive queue");

    rxBuffers.construct(infiniband->pd,
                        getMaxRpcSize(),
                        uint32_t(MAX_SHARED_RX_QUEUE_DEPTH * 2));
    uint32_t i = 0;
    foreach (auto& bd, *rxBuffers) {
        if (i < MAX_SHARED_RX_QUEUE_DEPTH)
            postSrqReceiveAndKickTransmit(serverSrq, &bd);
        else
            postSrqReceiveAndKickTransmit(clientSrq, &bd);
        ++i;
    }
    assert(numUsedClientSrqBuffers == 0);

    txBuffers.construct(infiniband->pd,
                        getMaxRpcSize(),
                        uint32_t(MAX_TX_QUEUE_DEPTH));
    foreach (auto& bd, *txBuffers)
        freeTxBuffers.push_back(&bd);

    // create completion queues for server receive, client receive, and
    // server/client transmit
    serverRxCq = infiniband->createCompletionQueue(MAX_SHARED_RX_QUEUE_DEPTH);
    check_error_null(serverRxCq,
                     "failed to create server receive completion queue");

    clientRxCq = infiniband->createCompletionQueue(MAX_SHARED_RX_QUEUE_DEPTH);
    check_error_null(clientRxCq,
                     "failed to create client receive completion queue");

    commonTxCq = infiniband->createCompletionQueue(MAX_TX_QUEUE_DEPTH);
    check_error_null(commonTxCq,
                     "failed to create transmit completion queue");
}

/**
 * Destructor for InfRcTransport.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::~InfRcTransport()
{
}

template<typename Infiniband>
void
InfRcTransport<Infiniband>::setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        LOG(ERROR, "fcntl F_GETFL failed");
        throw TransportException(HERE, "fnctl failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        LOG(ERROR, "fcntl F_SETFL failed");
        throw TransportException(HERE, "fnctl failed");
    }
}

/**
 * Construct a Session object for the public #getSession() interface.
 *
 * \param transport
 *      The transport this Session will be associated with.
 * \param sl
 *      The ServiceLocator describing the server to communicate with.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::InfRCSession::InfRCSession(
    InfRcTransport *transport, const ServiceLocator& sl)
    : transport(transport),
      qp(NULL)
{
    IpAddress address(sl);

    // create and set up a new queue pair for this client
    // TODO(ongaro): This probably doesn't need to allocate memory
    qp = transport->clientTrySetupQueuePair(address);
}

/**
 * Destroy the Session.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::InfRCSession::release()
{
    delete this;
}

/**
 * Issue an RPC request using infiniband->
 *
 * \param request
 *      Contents of the request message.
 * \param[out] response
 *      When a response arrives, the response message will be made
 *      available via this Buffer.
 *
 * \return  A pointer to the allocated space or \c NULL if there is not enough
 *          space in this Allocation.
 */
template<typename Infiniband>
Transport::ClientRpc*
InfRcTransport<Infiniband>::InfRCSession::clientSend(Buffer* request,
                                                     Buffer* response)
{
    InfRcTransport *t = transport;

    if (request->getTotalLength() > t->getMaxRpcSize()) {
        throw TransportException(HERE,
             format("client request exceeds maximum rpc size "
                    "(attempted %u bytes, maximum %u bytes)",
                    request->getTotalLength(),
                    t->getMaxRpcSize()));
    }

    // Construct our ClientRpc in the response Buffer.
    //
    // We do this because we're loaning one of our registered receive buffers
    // to the caller of wait() and need to issue it back to the HCA when
    // they're done with it.
    ClientRpc *rpc = new(response, MISC) ClientRpc(transport, this,
                                                   request, response,
                                                   generateRandom());
    rpc->sendOrQueue();
    return rpc;
}

/**
 * Attempt to exchange QueuePair set up information by sending to the server
 * and waiting for an appropriate response. Only one request is sent for each
 * invocation, but the method may receive multiple reponses (e.g. delayed
 * responses to a previous invocation). It only returns on a matched response,
 * or if time runs out.
 *
 * \param sin
 *      The server to send to.
 * \param outgoingQpt
 *      Pointer to the QueuePairTuple to send to the server.
 * \param incomingQpt
 *      Pointer to space in which to store the QueuePairTuple returned by
 *      the server.
 * \param usTimeout
 *      Timeout to wait for a server response in microseconds.
 * \return
 *      true if a valid response was received within the specified amount of
 *      time, else false if either nothing comes back in time, or the responses
 *      received did not match the request (this can happen if responses are
 *      delayed, rather than lost).
 * \throw TransportException
 *      An exception is thrown if any of the socket system calls fail for
 *      some strange reason.
 */
template<typename Infiniband>
bool
InfRcTransport<Infiniband>::clientTryExchangeQueuePairs(struct sockaddr_in *sin,
    QueuePairTuple *outgoingQpt, QueuePairTuple *incomingQpt,
    uint32_t usTimeout)
{
    bool haveSent = false;

    while (1) {
        TimeCounter startTime;

        if (!haveSent) {
            ssize_t len = sendto(clientSetupSocket, outgoingQpt,
                sizeof(*outgoingQpt), 0, reinterpret_cast<sockaddr *>(sin),
                sizeof(*sin));
            if (len == -1) {
                if (errno != EINTR && errno != EAGAIN) {
                    LOG(ERROR, "sendto returned error %d: %s",
                        errno, strerror(errno));
                    throw TransportException(HERE, errno);
                }
            } else if (len != sizeof(*outgoingQpt)) {
                LOG(ERROR, "sendto returned bad length (%Zd) while "
                    "sending to ip: [%s] port: [%d]", len,
                    inet_ntoa(sin->sin_addr), HTONS(sin->sin_port));
                throw TransportException(HERE, errno);
            } else {
                haveSent = true;
            }
        }

        socklen_t sinlen = sizeof(sin);
        ssize_t len = recvfrom(clientSetupSocket, incomingQpt,
            sizeof(*incomingQpt), 0,
            reinterpret_cast<sockaddr *>(&sin), &sinlen);
        if (len == -1) {
            if (errno != EINTR && errno != EAGAIN) {
                LOG(ERROR, "recvfrom returned error %d: %s",
                    errno, strerror(errno));
                throw TransportException(HERE, errno);
            }
        } else if (len != sizeof(*incomingQpt)) {
            LOG(ERROR, "recvfrom returned bad length (%Zd) while "
                "receiving from ip: [%s] port: [%d]", len,
                inet_ntoa(sin->sin_addr), HTONS(sin->sin_port));
            throw TransportException(HERE, errno);
        } else {
            if (outgoingQpt->getNonce() == incomingQpt->getNonce())
                return true;

            LOG(WARNING,
                "received nonce doesn't match (0x%016lx != 0x%016lx)",
                outgoingQpt->getNonce(), incomingQpt->getNonce());
        }

        uint32_t elapsedUs = downCast<uint32_t>(startTime.stop() / 1000);
        if (elapsedUs >= usTimeout)
            return false;
        usTimeout -= elapsedUs;

        // TODO(ongaro): The following isn't safe, at a minimum because some
        // other stack frame can start using clientSetupSocket.
        //
        // We need to call the dispatcher in order to let other event handlers
        // run (this is particularly important if the server we are trying to
        // connect to is us).
        if (dispatch->isDispatchThread()) {
            dispatch->poll();
        }
    }
}

/**
 * Attempt to set up a QueuePair with the given server. The client
 * allocates a QueuePair and sends the necessary tuple to the
 * server to begin the handshake. The server then replies with its
 * QueuePair tuple information. This is all done over IP/UDP.
 */
template<typename Infiniband>
typename Infiniband::QueuePair*
InfRcTransport<Infiniband>::clientTrySetupQueuePair(IpAddress& address)
{
    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&address.address);

    // Create a new QueuePair and send its parameters to the server so it
    // can create its qp and reply with its parameters.
    QueuePair *qp = infiniband->createQueuePair(IBV_QPT_RC,
                                                ibPhysicalPort, clientSrq,
                                                commonTxCq, clientRxCq,
                                                MAX_TX_QUEUE_DEPTH,
                                                MAX_SHARED_RX_QUEUE_DEPTH);

    for (uint32_t i = 0; i < QP_EXCHANGE_MAX_TIMEOUTS; i++) {
        QueuePairTuple outgoingQpt(downCast<uint16_t>(lid),
                                   qp->getLocalQpNumber(),
                                   qp->getInitialPsn(), generateRandom());
        QueuePairTuple incomingQpt;
        bool gotResponse;

        try {
            gotResponse = clientTryExchangeQueuePairs(sin, &outgoingQpt,
                                                      &incomingQpt,
                                                      QP_EXCHANGE_USEC_TIMEOUT);
        } catch (...) {
            delete qp;
            throw;
        }

        if (!gotResponse) {
            LOG(WARNING, "timed out waiting for response; retrying");
            ++metrics->transport.retrySessionOpenCount;
            continue;
        }

        // plumb up our queue pair with the server's parameters.
        qp->plumb(&incomingQpt);
        return qp;
    }

    LOG(WARNING, "failed to exchange with server (%s) within allotted "
        "%u microseconds (sent request %u times)",
        address.toString().c_str(),
        QP_EXCHANGE_USEC_TIMEOUT * QP_EXCHANGE_MAX_TIMEOUTS,
        QP_EXCHANGE_MAX_TIMEOUTS);
    delete qp;
    throw TransportException(HERE, "failed to connect to host");
}

/**
 * This method is invoked by the dispatcher when #serverSetupSocket becomes
 * readable. It attempts to set up QueuePair with a connecting remote
 * client.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::ServerConnectHandler::handleFileEvent()
{
    sockaddr_in sin;
    socklen_t sinlen = sizeof(sin);
    QueuePairTuple incomingQpt;

    ssize_t len = recvfrom(transport->serverSetupSocket, &incomingQpt,
        sizeof(incomingQpt), 0, reinterpret_cast<sockaddr *>(&sin), &sinlen);
    if (len <= -1) {
        if (errno == EAGAIN)
            return;
        LOG(ERROR, "recvfrom failed");
        throw TransportException(HERE, "recvfrom failed");
    } else if (len != sizeof(incomingQpt)) {
        LOG(WARNING, "recvfrom got a strange incoming size: %Zd", len);
        return;
    }

    // create a new queue pair, set it up according to our client's parameters,
    // and feed back our lid, qpn, and psn information so they can complete
    // the out-of-band handshake.

    // XXX- we should look up the QueuePair first using incomingQpt, just to
    //      be sure, esp. if we use an unreliable means of handshaking, in
    //      which case the response to the client request could have been lost.

    QueuePair *qp = transport->infiniband->createQueuePair(
            IBV_QPT_RC,
            transport->ibPhysicalPort,
            transport->serverSrq,
            transport->commonTxCq,
            transport->serverRxCq,
            MAX_TX_QUEUE_DEPTH,
            MAX_SHARED_RX_QUEUE_DEPTH);
    qp->plumb(&incomingQpt);

    // now send the client back our queue pair information so they can
    // complete the initialisation.
    QueuePairTuple outgoingQpt(downCast<uint16_t>(transport->lid),
                               qp->getLocalQpNumber(),
                               qp->getInitialPsn(), incomingQpt.getNonce());
    len = sendto(transport->serverSetupSocket, &outgoingQpt,
            sizeof(outgoingQpt), 0, reinterpret_cast<sockaddr *>(&sin),
            sinlen);
    if (len != sizeof(outgoingQpt)) {
        LOG(WARNING, "sendto failed, len = %Zd", len);
        delete qp;
        return;
    }

    // maintain the qpn -> qp mapping
    transport->queuePairMap[qp->getLocalQpNumber()] = qp;
}

/**
 * Add the given BufferDescriptor to the given shared receive queue.
 * If a previous transmit was buffered due to lack of receive buffers,
 * this method will kick off a transmission.
 *
 * \param[in] srq
 *      The shared receive queue on which to enqueue this BufferDescriptor.
 * \param[in] bd
 *      The BufferDescriptor to enqueue.
 * \throw
 *      TransportException if posting to the queue fails.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::postSrqReceiveAndKickTransmit(ibv_srq* srq,
    BufferDescriptor *bd)
{
    infiniband->postSrqReceive(srq, bd);

    // TODO(ongaro): This condition is hacky. One idea is to wrap ibv_srq in an
    // object and make this a virtual method instead.
    if (srq == clientSrq) {
        --numUsedClientSrqBuffers;
        if (!clientSendQueue.empty()) {
            ClientRpc& rpc = clientSendQueue.front();
            clientSendQueue.pop_front();
            LOG(DEBUG, "Dequeued request with nonce %016lx", rpc.nonce);
            rpc.sendOrQueue();
        }
    }
}

/**
 * Return a free transmit buffer, wrapped by its corresponding
 * BufferDescriptor. If there are none, block until one is available.
 *
 * TODO(rumble): Any errors from previous transmissions are basically
 *               thrown on the floor, though we do log them. We need
 *               to think a bit more about how this 'fire-and-forget'
 *               behaviour impacts our Transport API.
 */
template<typename Infiniband>
typename Infiniband::BufferDescriptor*
InfRcTransport<Infiniband>::getTransmitBuffer()
{
    CycleCounter<uint64_t> timeThis2;
    // if we've drained our free tx buffer pool, we must wait.
    while (freeTxBuffers.empty()) {

        ibv_wc retArray[MAX_TX_QUEUE_DEPTH];
        CycleCounter<uint64_t> timeThis;
        int n = infiniband->pollCompletionQueue(commonTxCq,
                                                MAX_TX_QUEUE_DEPTH,
                                                retArray);
        uint64_t gtbPollNanos = cyclesToNanoseconds(timeThis.stop());
        serverStats.gtbPollNanos += gtbPollNanos;
        serverStats.gtbPollCount++;

        if (0 >= n) {
             serverStats.gtbPollZeroNCount++;
             serverStats.gtbPollZeroNanos += gtbPollNanos;
        } else {
             serverStats.gtbPollNonZeroNAvg += n;
             serverStats.gtbPollNonZeroNanos += gtbPollNanos;
        }

        for (int i = 0; i < n; i++) {
            BufferDescriptor* bd =
                reinterpret_cast<BufferDescriptor*>(retArray[i].wr_id);
            freeTxBuffers.push_back(bd);

            if (retArray[i].status != IBV_WC_SUCCESS) {
                LOG(ERROR, "Transmit failed: %s",
                    infiniband->wcStatusToString(retArray[i].status));
            }
        }

    }

    BufferDescriptor* bd = freeTxBuffers.back();
    freeTxBuffers.pop_back();

    serverStats.infrcGetTxBufferNanos += timeThis2.stop();
    return bd;
}

/**
 * Obtain the maximum rpc size. This is limited by the infiniband
 * specification to 2GB(!), though we artificially limit it to a
 * little more than a segment size to avoid allocating too much 
 * space in RX buffers.
 */
template<typename Infiniband>
uint32_t
InfRcTransport<Infiniband>::getMaxRpcSize() const
{
    return MAX_RPC_SIZE;
}


template<typename Infiniband>
string
InfRcTransport<Infiniband>::getServiceLocator()
{
    return locatorString;
}

//-------------------------------------
// InfRcTransport::ServerRpc class
//-------------------------------------

/**
 * Construct a ServerRpc.
 * The input message is taken from transport->inputMessage, if
 * it contains data.
 *
 * \param transport
 *      The InfRcTransport object that this RPC is associated with.
 * \param qp
 *      The QueuePair associated with this RPC request.
 * \param nonce
 *      Uniquely identifies this RPC.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::ServerRpc::ServerRpc(InfRcTransport* transport,
                                                 QueuePair* qp,
                                                 uint64_t nonce)
    : transport(transport),
      qp(qp),
      nonce(nonce)
{
}

/**
 * Send a reply for an RPC.
 *
 * Transmits are done using a copy into a pre-registered HCA buffer.
 * The function blocks until the HCA returns success or failure.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::ServerRpc::sendReply()
{
    CycleCounter<uint64_t> timeThis1;
    CycleCounter<Metric> _(&metrics->transport.transmit.ticks);
    ++metrics->transport.transmit.messageCount;
    ++metrics->transport.transmit.packetCount;
    LOG(DEBUG, "Sending response with nonce %016lx", nonce);
    // "delete this;" on our way out of the method
    boost::scoped_ptr<InfRcTransport::ServerRpc> suicide(this);

    InfRcTransport *t = transport;

    if (replyPayload.getTotalLength() > t->getMaxRpcSize()) {
        throw TransportException(HERE,
             format("server response exceeds maximum rpc size "
                    "(attempted %u bytes, maximum %u bytes)",
                    replyPayload.getTotalLength(),
                    t->getMaxRpcSize()));
    }

    BufferDescriptor* bd = t->getTransmitBuffer();
    serverStats.infrcGetTxCount++;
    new(&replyPayload, PREPEND) Header(nonce);
    {
        CycleCounter<Metric> copyTicks(&metrics->transport.transmit.copyTicks);
        replyPayload.copy(0, replyPayload.getTotalLength(), bd->buffer);
    }
    metrics->transport.transmit.iovecCount += replyPayload.getNumberChunks();
    metrics->transport.transmit.byteCount += replyPayload.getTotalLength();
    t->infiniband->postSend(qp, bd, replyPayload.getTotalLength());
    replyPayload.truncateFront(sizeof(Header)); // for politeness
    LOG(DEBUG, "Sent response with nonce %016lx", nonce);
    serverStats.infrcSendReplyNanos += cyclesToNanoseconds(timeThis1.stop());
}

//-------------------------------------
// InfRcTransport::ClientRpc class
//-------------------------------------

/**
 * Construct a ClientRpc.
 *
 * \param transport
 *      The InfRcTransport object that this RPC is associated with.
 * \param session
 *      The session this RPC is associated with.
 * \param request
 *      Request payload.
 * \param[out] response
 *      Buffer in which the response message should be placed.
 * \param nonce
 *      Uniquely identifies this RPC.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::ClientRpc::ClientRpc(InfRcTransport* transport,
                                     InfRCSession* session,
                                     Buffer* request,
                                     Buffer* response,
                                     uint64_t nonce)
    : transport(transport),
      session(session),
      request(request),
      response(response),
      nonce(nonce),
      state(PENDING),
      queueEntries()
{

}

/**
 * This is a hack for doing zero-copy sends of log data to backups
 * during recovery. Diego has a cleaner solution, but we want this
 * quickly for the paper. Afterwards we'll have to bulldoze the Inf
 * stuff and write something clean.
 */
template<typename Infiniband>
bool
InfRcTransport<Infiniband>::ClientRpc::tryZeroCopy(Buffer* request)
{
    InfRcTransport* const t = transport;
    if (t->logMemoryBase != 0 && request->getNumberChunks() == 2) {
        Buffer::Iterator it(*request);
        it.next();
        const uintptr_t addr = reinterpret_cast<const uintptr_t>(it.getData());
        if (addr >= t->logMemoryBase &&
          (addr + it.getLength()) < (t->logMemoryBase + t->logMemoryBytes)) {
            uint32_t hdrBytes = it.getTotalLength() - it.getLength();
//LOG(NOTICE, "ZERO COPYING WRITE FROM LOG: total: %u bytes, hdr: %lu bytes, 0copy: %u bytes\n", request->getTotalLength(), hdrBytes, it.getLength()); //NOLINT 
            BufferDescriptor* bd = t->getTransmitBuffer();
            {
                CycleCounter<Metric>
                    copyTicks(&metrics->transport.transmit.copyTicks);
                request->copy(0, hdrBytes, bd->buffer);
            }
            metrics->transport.transmit.iovecCount +=
                request->getNumberChunks();
            metrics->transport.transmit.byteCount += request->getTotalLength();
            LOG(DEBUG, "Sending 0-copy request");
            t->infiniband->postSendZeroCopy(session->qp, bd,
                hdrBytes, it.getData(), it.getLength(), t->logMemoryRegion);
            return true;
        }
    }

    return false;
}

/**
 * Send the RPC request out onto the network if there is a receive buffer
 * available for its response, or queue it for transmission otherwise.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::ClientRpc::sendOrQueue()
{
    assert(state == PENDING);
    InfRcTransport* const t = transport;
    if (t->numUsedClientSrqBuffers < MAX_SHARED_RX_QUEUE_DEPTH) {
        // send out the request
        if (t->outstandingRpcs.empty()) {
            t->clientRpcsActiveTime.construct(
                &metrics->transport.clientRpcsActiveTicks);
        }
        CycleCounter<Metric> _(&metrics->transport.transmit.ticks);
        ++metrics->transport.transmit.messageCount;
        ++metrics->transport.transmit.packetCount;
        new(request, PREPEND) Header(nonce);

        if (!tryZeroCopy(request)) {
            BufferDescriptor* bd = t->getTransmitBuffer();
            {
                CycleCounter<Metric>
                    copyTicks(&metrics->transport.transmit.copyTicks);
                request->copy(0, request->getTotalLength(), bd->buffer);
            }
            metrics->transport.transmit.iovecCount +=
                request->getNumberChunks();
            metrics->transport.transmit.byteCount += request->getTotalLength();
            LOG(DEBUG, "Sending request with nonce %016lx", nonce);
            t->infiniband->postSend(session->qp, bd,
                                    request->getTotalLength());
        }
        request->truncateFront(sizeof(Header)); // for politeness

        t->outstandingRpcs.push_back(*this);
        ++t->numUsedClientSrqBuffers;
        state = REQUEST_SENT;
        LOG(DEBUG, "Sent request with nonce %016lx", nonce);
    } else {
        // no available receive buffers
        t->clientSendQueue.push_back(*this);
        LOG(DEBUG, "Queued send request with nonce %016lx", nonce);
    }
}

/**
 * This method is invoked by the dispatcher's inner polling loop; it
 * checks for incoming RPC requests and responses and processes them.
 *
 * \return
 *      True if we were able to do anything useful, false if there was
 *      no meaningful data.
 */
template<typename Infiniband>
void
InfRcTransport<Infiniband>::Poller::poll()
{
    InfRcTransport* t = transport;
    ibv_wc wc;

    // First check for responses to requests that we have made.
    if (!t->outstandingRpcs.empty()) {
        while (t->infiniband->pollCompletionQueue(t->clientRxCq, 1, &wc) > 0) {
            CycleCounter<Metric> receiveTicks;
            BufferDescriptor *bd =
                        reinterpret_cast<BufferDescriptor *>(wc.wr_id);
            if (wc.status != IBV_WC_SUCCESS) {
                LOG(ERROR, "wc.status(%d:%s) != IBV_WC_SUCCESS",
                    wc.status, t->infiniband->wcStatusToString(wc.status));
                t->postSrqReceiveAndKickTransmit(t->clientSrq, bd);
                throw TransportException(HERE, wc.status);
            }

            Header& header(*reinterpret_cast<Header*>(bd->buffer));
            LOG(DEBUG, "Received response with nonce %016lx", header.nonce);
            foreach (ClientRpc& rpc, t->outstandingRpcs) {
                if (rpc.nonce != header.nonce)
                    continue;
                t->outstandingRpcs.erase(t->outstandingRpcs.iterator_to(rpc));
                uint32_t len = wc.byte_len - downCast<uint32_t>(sizeof(header));
                if (t->numUsedClientSrqBuffers >=
                        MAX_SHARED_RX_QUEUE_DEPTH / 2) {
                    // clientSrq is low on buffers, better return this one
                    LOG(DEBUG, "Copy and immediately return clientSrq buffer");
                    memcpy(new(rpc.response, APPEND) char[len],
                           bd->buffer + sizeof(header),
                           len);
                    t->postSrqReceiveAndKickTransmit(t->clientSrq, bd);
                } else {
                    // rpc will hold one of clientSrq's buffers until
                    // rpc.response is destroyed
                    LOG(DEBUG, "Hang onto clientSrq buffer");
                    PayloadChunk::appendToBuffer(rpc.response,
                                                 bd->buffer + sizeof(header),
                                                 len, t, t->clientSrq, bd);
                }
                rpc.state = ClientRpc::RESPONSE_RECEIVED;
                rpc.markFinished();
                ++metrics->transport.receive.messageCount;
                ++metrics->transport.receive.packetCount;
                metrics->transport.receive.iovecCount +=
                    rpc.response->getNumberChunks();
                metrics->transport.receive.byteCount +=
                    rpc.response->getTotalLength();
                metrics->transport.receive.ticks += receiveTicks.stop();
                if (t->outstandingRpcs.empty())
                    t->clientRpcsActiveTime.destroy();
                goto next;
            }
            LOG(WARNING, "dropped packet because no nonce matched %016lx",
                         header.nonce);
      next: { /* pass */ }
        }
    }

    // Next, check for incoming RPC requests (assuming that we are a server).
    if (t->serverSetupSocket >= 0) {
        CycleCounter<Metric> receiveTicks;
        if (t->infiniband->pollCompletionQueue(t->serverRxCq, 1, &wc) >= 1) {
            if (t->queuePairMap.find(wc.qp_num) == t->queuePairMap.end()) {
                LOG(ERROR, "failed to find qp_num in map");
                goto done;
            }

            QueuePair *qp = t->queuePairMap[wc.qp_num];

            BufferDescriptor* bd =
                reinterpret_cast<BufferDescriptor*>(wc.wr_id);

            if (wc.status != IBV_WC_SUCCESS) {
                LOG(ERROR, "failed to receive rpc!");
                t->postSrqReceiveAndKickTransmit(t->serverSrq, bd);
                goto done;
            }

            Header& header(*reinterpret_cast<Header*>(bd->buffer));
            ServerRpc *r = new ServerRpc(t, qp, header.nonce);
            PayloadChunk::appendToBuffer(&r->requestPayload,
                bd->buffer + downCast<uint32_t>(sizeof(header)),
                wc.byte_len - downCast<uint32_t>(sizeof(header)),
                t, t->serverSrq, bd);
            LOG(DEBUG, "Received request with nonce %016lx", header.nonce);
            serviceManager->handleRpc(r);
            ++metrics->transport.receive.messageCount;
            ++metrics->transport.receive.packetCount;
            metrics->transport.receive.iovecCount +=
                r->requestPayload.getNumberChunks();
            metrics->transport.receive.byteCount +=
                r->requestPayload.getTotalLength();
            metrics->transport.receive.ticks += receiveTicks.stop();
        }
    }
  done:
    return;
}

//-------------------------------------
// InfRcTransport::PayloadChunk class
//-------------------------------------

/**
 * Append a subregion of payload data which releases the memory to the
 * HCA when its containing Buffer is destroyed.
 *
 * \param buffer
 *      The Buffer to append the data to.
 * \param data
 *      The address of the data to appear in the Buffer.
 * \param dataLength
 *      The length in bytes of the region starting at data that is a
 *      subregion of the payload.
 * \param transport
 *      The transport that owns the provided BufferDescriptor 'bd'.
 * \param srq
 *      The shared receive queue which bd will be returned to.
 * \param bd 
 *      The BufferDescriptor to return to the HCA on Buffer destruction.
 */
template<typename Infiniband>
typename InfRcTransport<Infiniband>::PayloadChunk*
InfRcTransport<Infiniband>::PayloadChunk::prependToBuffer(Buffer* buffer,
                                             char* data,
                                             uint32_t dataLength,
                                             InfRcTransport* transport,
                                             ibv_srq* srq,
                                             BufferDescriptor* bd)
{
    PayloadChunk* chunk =
        new(buffer, CHUNK) PayloadChunk(data, dataLength, transport, srq, bd);
    Buffer::Chunk::prependChunkToBuffer(buffer, chunk);
    return chunk;
}

/**
 * Prepend a subregion of payload data which releases the memory to the
 * HCA when its containing Buffer is destroyed.
 *
 * \param buffer
 *      The Buffer to prepend the data to.
 * \param data
 *      The address of the data to appear in the Buffer.
 * \param dataLength
 *      The length in bytes of the region starting at data that is a
 *      subregion of the payload.
 * \param transport
 *      The transport that owns the provided BufferDescriptor 'bd'.
 * \param srq
 *      The shared receive queue which bd will be returned to.
 * \param bd
 *      The BufferDescriptor to return to the HCA on Buffer destruction.
 */
template<typename Infiniband>
typename InfRcTransport<Infiniband>::PayloadChunk*
InfRcTransport<Infiniband>::PayloadChunk::appendToBuffer(Buffer* buffer,
                                            char* data,
                                            uint32_t dataLength,
                                            InfRcTransport* transport,
                                            ibv_srq* srq,
                                            BufferDescriptor* bd)
{
    PayloadChunk* chunk =
        new(buffer, CHUNK) PayloadChunk(data, dataLength, transport, srq, bd);
    Buffer::Chunk::appendChunkToBuffer(buffer, chunk);
    return chunk;
}

/// Returns memory to the HCA once the Chunk is discarded.
template<typename Infiniband>
InfRcTransport<Infiniband>::PayloadChunk::~PayloadChunk()
{
    transport->postSrqReceiveAndKickTransmit(srq, bd);
}

/**
 * Construct a PayloadChunk which will release it's resources to the
 * HCA its containing Buffer is destroyed.
 *
 * \param data
 *      The address of the data to appear in the Buffer.
 * \param dataLength
 *      The length in bytes of the region starting at data that is a
 *      subregion of the payload.
 * \param transport
 *      The transport that owns the provided BufferDescriptor 'bd'.
 * \param srq
 *      The shared receive queue which bd will be returned to.
 * \param bd 
 *      The BufferDescriptor to return to the HCA on Buffer destruction.
 */
template<typename Infiniband>
InfRcTransport<Infiniband>::PayloadChunk::PayloadChunk(void* data,
                                          uint32_t dataLength,
                                          InfRcTransport *transport,
                                          ibv_srq* srq,
                                          BufferDescriptor* bd)
    : Buffer::Chunk(data, dataLength),
      transport(transport),
      srq(srq),
      bd(bd)
{
}

template class InfRcTransport<RealInfiniband>;

}  // namespace RAMCloud
