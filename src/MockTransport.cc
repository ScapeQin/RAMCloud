/* Copyright (c) 2010-2012 Stanford University
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

#include "TestUtil.h"
#include "MockTransport.h"
#include "Rpc.h"

namespace RAMCloud {
uint32_t RAMCloud::MockTransport::sessionDeleteCount = 0;

//------------------------------
// MockTransport class
//------------------------------

/**
 * Construct a MockTransport.
 */
MockTransport::MockTransport(Context& context,
                             const ServiceLocator *serviceLocator)
            : context(context)
            , outputLog()
            , status(Status(STATUS_MAX_VALUE+1))
            , inputMessages()
            , lastNotifier(NULL)
            , serverSendCount(0)
            , clientSendCount(0)
            , clientRecvCount(0)
            , sessionCreateCount(0)
            , locatorString()
{
    if (serviceLocator != NULL) {
        locatorString = serviceLocator->getOriginalString();
    } else {
        locatorString = "mock:";
    }
}

/**
 * See Transport::getServiceLocator.
 */
string
MockTransport::getServiceLocator()
{
    return locatorString;
}

Transport::SessionRef
MockTransport::getSession(const ServiceLocator& serviceLocator,
        uint32_t timeoutMs)
{
    sessionCreateCount++;

    // magic hook to invoke failed getSession in testing
    if (strstr(serviceLocator.getOriginalString().c_str(),
            "host=error") != NULL)
        throw TransportException(HERE, "Failed to open session");

    MockSession* session = new MockSession(this, serviceLocator);
    session->setServiceLocator(serviceLocator.getOriginalString());
    return session;
}

Transport::SessionRef
MockTransport::getSession()
{
    MockSession* session =  new MockSession(this);
    session->setServiceLocator("test:");
    return session;
}

/**
 * Destructor for MockSession: just log the destruction.
 */
MockTransport::MockSession::~MockSession()
{
    MockTransport::sessionDeleteCount++;
}

/**
 * Abort this session; this method does nothing except create a
 * log message.
 */
void
MockTransport::MockSession::abort(const string& message)
{
    if (transport->outputLog.length() != 0) {
        transport->outputLog.append(" | ");
    }
    transport->outputLog.append("abort: ");
    transport->outputLog.append(message);
}

// See Transport::Session::cancelRequest for documentation.
void
MockTransport::MockSession::cancelRequest(RpcNotifier* notifier)
{
    if (transport->outputLog.length() != 0) {
        transport->outputLog.append(" | ");
    }
    transport->outputLog.append("cancel");
}

/**
 * Issue an RPC request using this transport.
 *
 * This is a fake method; it simply logs information about the request.
 *
 * \param payload
 *      Contents of the request message.
 * \param[out] response
 *      When a response arrives, the response message will be made
 *      available via this Buffer.
 *
 * \return  A pointer to the allocated space or \c NULL if there is not enough
 *          space in this Allocation.
 */
Transport::ClientRpc*
MockTransport::MockSession::clientSend(Buffer* payload, Buffer* response)
{
    if (transport->outputLog.length() != 0) {
        transport->outputLog.append(" | ");
    }
    transport->outputLog.append("clientSend: ");
    transport->outputLog.append(TestUtil::toString(payload));
    return new(response, MISC) MockClientRpc(transport, payload, response);
}

// See Transport::Session::release for documentation.
void
MockTransport::MockSession::release()
{
    delete this;
}

// See Transport::Session::sendRequest for documentation.
void
MockTransport::MockSession::sendRequest(Buffer* request, Buffer* response,
                RpcNotifier* notifier)
{
    response->reset();
    if (transport->outputLog.length() != 0) {
        transport->outputLog.append(" | ");
    }
    transport->outputLog.append("sendRequest: ");
    transport->outputLog.append(TestUtil::toString(request));
    if (!transport->inputMessages.empty()) {
        const char *resp = transport->inputMessages.front();
        transport->inputMessages.pop();
        if (resp == NULL) {
            notifier->failed();
        } else {
            response->fillFromString(resp);
            notifier->completed();
        }
    }
    transport->lastNotifier = notifier;
}

/**
 * Clear all testing RPC responses; see setInput().
 */
void
MockTransport::clearInput()
{
    while (!inputMessages.empty())
        inputMessages.pop();
}

/**
 * This method is invoked by tests to provide a string that will
 * be used to synthesize an input message the next time one is
 * needed (such as for an RPC result). The value NULL may be used when
 * one wants to throw a transport exception instead of a response.
 *
 * \param s
 *      A string representation of the contents of a buffer,
 *      in the format expected by Buffer::fillFromString.
 */
void
MockTransport::setInput(const char* s)
{
    inputMessages.push(s);
}

//-------------------------------------
// MockTransport::MockServerRpc class
//-------------------------------------

/**
 * Construct a MockServerRpc.
 *
 * \param transport
 *      The MockTransport object that this RPC is associated with.
 * \param message
 *      Describes contents of message (parsed by #fillFromString).
 */
MockTransport::MockServerRpc::MockServerRpc(MockTransport* transport,
                                            const char* message)
        : transport(transport)
{
    if (message != NULL) {
        requestPayload.fillFromString(message);
    }

    // set this to avoid handleRpc's sanity check
    epoch = 0;
}

/**
 * Send a reply for an RPC. This method just logs the contents of
 * the reply message and deletes this reply object.
 */
void
MockTransport::MockServerRpc::sendReply()
{
    if (transport->outputLog.length() != 0) {
        transport->outputLog.append(" | ");
    }
    transport->outputLog.append("serverReply: ");
    transport->outputLog.append(TestUtil::toString(&replyPayload));
    const RpcResponseCommon* responseHeader =
        replyPayload.getStart<RpcResponseCommon>();
    transport->status = (responseHeader != NULL) ? responseHeader->status :
            Status(STATUS_MAX_VALUE+1);
    delete this;
}

/**
 * Normally used to return ServiceLocator-like string associated
 * with the client that issued this RPC. It's not currently used
 * for testing, so return a blank string instead.
 */
string
MockTransport::MockServerRpc::getClientServiceLocator()
{
    return "";
}

//-------------------------------------
// MockTransport::MockClientRpc class
//-------------------------------------

/**
 * Construct a MockClientRpc.
 *
 * \param transport
 *      The MockTransport object that this RPC is associated with.
 * \param[out] request
 *      Buffer containing the request message.
 * \param[out] response
 *      Buffer in which the response message should be placed.
 */
MockTransport::MockClientRpc::MockClientRpc(MockTransport* transport,
                                            Buffer* request,
                                            Buffer* response)
        : Transport::ClientRpc(transport->context, request, response)
{
    if (transport->inputMessages.empty()) {
        markFinished("no responses enqueued for MockTransport");
        return;
    }
    const char *resp = transport->inputMessages.front();
    transport->inputMessages.pop();
    if (resp == NULL) {
        markFinished("testing");
        return;
    }
    response->fillFromString(resp);
    markFinished();
}

}  // namespace RAMCloud
