// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Peer.h"

#include "util/Logging.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/PeerMaster.h"
#include "herder/HerderGateway.h"
#include "database/Database.h"

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{
Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == ACCEPTOR ? CONNECTED : CONNECTING)
    , mRemoteListeningPort(-1)
{
    if (mRole == ACCEPTOR)
    {
        // Schedule a 'say hello' event at the next opportunity,
        // if we're the acceptor-role.
        mApp.getMainIOService().post([this]()
                                     {
                                         this->sendHello();
                                     });
    }
}

void
Peer::sendHello()
{
    StellarMessage msg;
    msg.type(HELLO);
    msg.hello().protocolVersion = mApp.getConfig().PROTOCOL_VERSION;
    msg.hello().versionStr = mApp.getConfig().VERSION_STR;

    sendMessage(msg);
}

void
Peer::connectHandler(const asio::error_code& error)
{
    if (error)
    {
        CLOG(WARNING, "Overlay") << "connectHandler error: " << error;
        drop();
    }
    else
    {
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type,
                   uint256 const& itemID)
{
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendFBAQuorumSet(FBAQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(FBA_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
}
void
Peer::sendGetTxSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}
void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_FBA_QUORUMSET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    // TODO.2 catch DB errors and malformed IPs

    // send top 50 peers we know about
    vector< pair<string, int> > peerList;
    mApp.getDatabase().loadPeers(50, peerList);
    StellarMessage newMsg;
    newMsg.type(PEERS);
    newMsg.peers().resize(peerList.size());
    for(int n = 0; n < peerList.size(); n++)
    {
        ipFromStr(peerList[n].first, newMsg.peers()[n].ip);
        newMsg.peers()[n].port = peerList[n].second;
    }
    sendMessage(newMsg);
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    CLOG(TRACE, "Overlay") << "sending stellarMessage";
    xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(msg));
    this->sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    StellarMessage sm;
    xdr::xdr_from_msg(msg, sm);
    recvMessage(sm);
}

void
Peer::recvMessage(StellarMessage const& stellarMsg)
{
    CLOG(TRACE, "Overlay") << "recv: " << stellarMsg.type();

    if (mState < GOT_HELLO && stellarMsg.type() != HELLO)
    {
        CLOG(WARNING, "Overlay") << "recv: " << stellarMsg.type()
                                 << " before hello";
        drop();
        return;
    }

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        this->recvHello(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        recvGetPeers(stellarMsg);
    }
    break;

    case PEERS:
    {
        recvPeers(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        recvTxSet(stellarMsg);
    }
    break;

    case GET_VALIDATIONS:
    {
        recvGetValidations(stellarMsg);
    }
    break;

    case VALIDATIONS:
    {
        recvValidations(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        recvTransaction(stellarMsg);
    }
    break;

    case GET_FBA_QUORUMSET:
    {
        recvGetFBAQuorumSet(stellarMsg);
    }
    break;

    case FBA_QUORUMSET:
    {
        recvFBAQuorumSet(stellarMsg);
    }
    break;

    case FBA_MESSAGE:
    {
        recvFBAMessage(stellarMsg);
    }
    break;
    case JSON_TRANSACTION:
    {
        assert(false);
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    switch (msg.dontHave().type)
    {
    case TX_SET:
        mApp.getHerderGateway().doesntHaveTxSet(msg.dontHave().reqHash,
                                                shared_from_this());
        break;
    case FBA_QUORUMSET:
        mApp.getHerderGateway().doesntHaveFBAQuorumSet(msg.dontHave().reqHash,
                                                       shared_from_this());
        break;
    case VALIDATIONS:
    default:
        break;
    }
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    TxSetFramePtr txSet =
        mApp.getHerderGateway().fetchTxSet(msg.txSetHash(), false);
    if (txSet)
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        sendMessage(newMsg);
    }
    else
    {
        sendDontHave(TX_SET, msg.txSetHash());
    }
}
void
Peer::recvTxSet(StellarMessage const& msg)
{
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(msg.txSet());
    mApp.getHerderGateway().recvTxSet(txSet);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction =
        TransactionFrame::makeTransactionFromWire(msg.transaction());
    if (transaction)
    { 
        // add it to our current set
        if (mApp.getHerderGateway().recvTransaction(transaction))
        {
            mApp.getOverlayGateway().broadcastMessage(msg, shared_from_this());
        }
    }
}

void
Peer::recvGetFBAQuorumSet(StellarMessage const& msg)
{
    FBAQuorumSetPtr qSet = 
        mApp.getHerderGateway().fetchFBAQuorumSet(msg.qSetHash(), false);
    if (qSet)
    {
        sendFBAQuorumSet(qSet);
    }
    else
    {
        sendDontHave(FBA_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvFBAQuorumSet(StellarMessage const& msg)
{
    FBAQuorumSetPtr qSet =
        std::make_shared<FBAQuorumSet>(msg.qSet());
    mApp.getHerderGateway().recvFBAQuorumSet(qSet);
}

void
Peer::recvFBAMessage(StellarMessage const& msg)
{
    FBAEnvelope envelope = msg.fbaMessage();

    // TODO(spolu): [ask jed] Check OK
    Hash envHash = sha512_256(xdr::xdr_to_msg(envelope));
    mApp.getOverlayGateway().recvFloodedMsg(envHash, msg,
                                            envelope.statement.slotIndex,
                                            shared_from_this());

    mApp.getHerderGateway().recvFBAEnvelope(envelope);
}

void
Peer::recvError(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvHello(StellarMessage const& msg)
{
    mRemoteProtocolVersion = msg.hello().protocolVersion;
    mRemoteVersion = msg.hello().versionStr;
    mRemoteListeningPort = msg.hello().port;
    CLOG(INFO, "Overlay") << "recvHello: " << mRemoteProtocolVersion << " "
                          << mRemoteVersion << " " << mRemoteListeningPort;
    mState = GOT_HELLO;
}

// returns false if string is malformed
bool Peer::ipFromStr(std::string ipStr,xdr::opaque_array<4U>& ret)
{
    std::stringstream ss(ipStr);
    std::string item;
    int n = 0;
    while(std::getline(ss, item, '.') && n<4) 
    {
        ret[n] = atoi(item.c_str());
        n++;
    }
    if(n==4)
        return true;

    return false;
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    for(auto peer : msg.peers())
    {
        stringstream ip;
        ip << (int)peer.ip[0] << "." << (int)peer.ip[1] << "." << (int)peer.ip[2] << "." << (int)peer.ip[3];
        mApp.getDatabase().addPeer(ip.str(), peer.port);
    }
}

void
Peer::recvGetValidations(StellarMessage const& msg)
{
    // TODO.3
}
void
Peer::recvValidations(StellarMessage const& msg)
{
    // TODO.3
}
}
