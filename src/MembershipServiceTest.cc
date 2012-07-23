/* Copyright (c) 2011-2012 Stanford University
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

#include "TestUtil.h"
#include "BindTransport.h"
#include "CoordinatorServerList.h"
#include "MembershipClient.h"
#include "MembershipService.h"
#include "ServerId.h"
#include "ServerList.h"
#include "ServerList.pb.h"
#include "ServerListBuilder.h"
#include "ShortMacros.h"
#include "TransportManager.h"

namespace RAMCloud {

class MembershipServiceTest : public ::testing::Test {
  public:
    Context context;
    ServerId serverId;
    ServerList serverList;
    MembershipService service;
    BindTransport transport;
    TransportManager::MockRegistrar mockRegistrar;
    MembershipClient client;

    MembershipServiceTest()
        : context()
        , serverId(99, 2)
        , serverList(context)
        , service(serverId, serverList)
        , transport(context)
        , mockRegistrar(context, transport)
        , client(context)
    {
        transport.addService(service, "mock:host=member", MEMBERSHIP_SERVICE);
        context.serverList = &serverList;
        serverList.add(serverId, "mock:host=member", {PING_SERVICE}, 100);
    }

    DISALLOW_COPY_AND_ASSIGN(MembershipServiceTest);
};

TEST_F(MembershipServiceTest, getServerId) {
    serverId = ServerId(523, 234);
    EXPECT_EQ(ServerId(523, 234), client.getServerId(
        context.transportManager->getSession("mock:host=member")));
}

TEST_F(MembershipServiceTest, setServerList) {
    CoordinatorServerList source(context);
    ProtoBuf::ServerList update;
    ServerId id1 = source.add("mock:host=55", {MASTER_SERVICE, PING_SERVICE},
            100, update);
    ServerId id2 = source.add("mock:host=56", {MASTER_SERVICE, PING_SERVICE},
            100, update);
    ServerId id3 = source.add("mock:host=57", {MASTER_SERVICE, PING_SERVICE},
            100, update);
    ProtoBuf::ServerList fullList;
    source.serialize(fullList);
    MembershipClient::setServerList(context, serverId, fullList);
    EXPECT_STREQ("mock:host=55", serverList.getLocator(id1));
    EXPECT_STREQ("mock:host=56", serverList.getLocator(id2));
    EXPECT_STREQ("mock:host=57", serverList.getLocator(id3));
    EXPECT_FALSE(serverList.contains(serverId));
}

TEST_F(MembershipServiceTest, updateServerList) {
    CoordinatorServerList source(context);
    ProtoBuf::ServerList update;
    ServerId id1 = source.add("mock:host=55", {MASTER_SERVICE, PING_SERVICE},
            100, update);
    ServerId id2 = source.add("mock:host=56", {MASTER_SERVICE, PING_SERVICE},
            100, update);
    source.incrementVersion(update);
    MembershipClient::updateServerList(context, serverId, update);
    EXPECT_STREQ("mock:host=55", serverList.getLocator(id1));
    EXPECT_STREQ("mock:host=56", serverList.getLocator(id2));
    EXPECT_TRUE(serverList.contains(serverId));
}

}  // namespace RAMCloud
