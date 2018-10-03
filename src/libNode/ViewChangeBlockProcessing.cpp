/*
 * Copyright (c) 2018 Zilliqa
 * This source code is being disclosed to you solely for the purpose of your
 * participation in testing Zilliqa. You may view, compile and run the code for
 * that purpose and pursuant to the protocols and algorithms that are programmed
 * into, and intended by, the code. You may not do anything else with the code
 * without express permission from Zilliqa Research Pte. Ltd., including
 * modifying or publishing the code (or any part of it), and developing or
 * forming another public or private blockchain network. This source code is
 * provided 'as is' and no warranties are given as to title or non-infringement,
 * merchantability or fitness for purpose and, to the extent permitted by law,
 * all liability for your use of the code is disclaimed. Some programs in this
 * code are governed by the GNU General Public License v3.0 (available at
 * https://www.gnu.org/licenses/gpl-3.0.en.html) ('GPLv3'). The programs that
 * are governed by GPLv3.0 are those programs that are located in the folders
 * src/depends and tests/depends and which include a reference to GPLv3 in their
 * program files.
 */

#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libConsensus/ConsensusUser.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"

using namespace std;
using namespace boost::multiprecision;

void Node::UpdateDSCommiteeCompositionAfterVC()
{
    LOG_MARKER();

    lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
    m_mediator.m_DSCommittee->emplace_back(m_mediator.m_DSCommittee->front());
    m_mediator.m_DSCommittee->pop_front();
}

bool Node::VerifyVCBlockCoSignature(const VCBlock& vcblock)
{
    LOG_MARKER();

    unsigned int index = 0;
    unsigned int count = 0;

    const vector<bool>& B2 = vcblock.GetB2();
    if (m_mediator.m_DSCommittee->size() != B2.size())
    {
        LOG_GENERAL(WARNING,
                    "Mismatch: DS committee size = "
                        << m_mediator.m_DSCommittee->size()
                        << ", co-sig bitmap size = " << B2.size());
        return false;
    }

    // Generate the aggregated key
    vector<PubKey> keys;

    for (auto const& kv : *m_mediator.m_DSCommittee)
    {
        if (B2.at(index))
        {
            keys.emplace_back(kv.first);
            count++;
        }
        index++;
    }

    if (count != ConsensusCommon::NumForConsensus(B2.size()))
    {
        LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
        return false;
    }

    shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
    if (aggregatedKey == nullptr)
    {
        LOG_GENERAL(WARNING, "Aggregated key generation failed");
        return false;
    }

    // Verify the collective signature
    vector<unsigned char> message;
    vcblock.GetHeader().Serialize(message, 0);
    vcblock.GetCS1().Serialize(message, VCBlockHeader::SIZE);
    BitVector::SetBitVector(message, VCBlockHeader::SIZE + BLOCK_SIG_SIZE,
                            vcblock.GetB1());
    if (!Schnorr::GetInstance().Verify(message, 0, message.size(),
                                       vcblock.GetCS2(), *aggregatedKey))
    {
        LOG_GENERAL(WARNING, "Cosig verification failed. Pubkeys");
        for (auto& kv : keys)
        {
            LOG_GENERAL(WARNING, kv);
        }
        return false;
    }

    return true;
}

/**  TODO 
void Node::LogReceivedDSBlockDetails(const DSBlock& dsblock)
{
#ifdef IS_LOOKUP_NODE
        LOG_GENERAL(
            INFO,
            "m_VieWChangeDSEpochNo "
                << to_string(vcblock.GetHeader().GetVieWChangeDSEpochNo())
                       .c_str()
                << "\n"
                << "m_VieWChangeEpochNo: "
                << to_string(vcblock.GetHeader().GetViewChangeEpochNo()).c_str()
                << "\n"
                << "m_ViewChangeState: "
                << vcblock.GetHeader().GetViewChangeState() << "\n"
                << "m_CandidateLeaderIndex: "
                << to_string(vcblock.GetHeader().GetCandidateLeaderIndex())
                << "\n"
                << "m_CandidateLeaderNetworkInfo: "
                << vcblock.GetHeader().GetCandidateLeaderNetworkInfo() << "\n"
                << "m_CandidateLeaderPubKey: "
                << vcblock.GetHeader().GetCandidateLeaderPubKey() << "\n"
                << "m_VCCounter: "
                << to_string(vcblock.GetHeader().GetViewChangeCounter()).c_str()
                << "\n"
                << "m_Timestamp: " << vcblock.GetHeader().GetTimeStamp());
#endif // IS_LOOKUP_NODE
}
**/

bool Node::ProcessVCBlock(const vector<unsigned char>& message,
                          unsigned int cur_offset,
                          [[gnu::unused]] const Peer& from)
{
    LOG_MARKER();

    VCBlock vcblock;

    if (!Messenger::GetNodeVCBlock(message, cur_offset, vcblock))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Messenger::GetNodeVCBlock failed.");
        return false;
    }

    if (vcblock.GetHeader().GetViewChangeEpochNo()
        != m_mediator.m_currentEpochNum)
    {
        LOG_GENERAL(WARNING,
                    "Received wrong vcblock. cur epoch: "
                        << m_mediator.m_currentEpochNum << "vc epoch: "
                        << vcblock.GetHeader().GetViewChangeEpochNo());
        return false;
    }

    // TODO State machine check

    unsigned int newCandidateLeader
        = vcblock.GetHeader().GetViewChangeCounter();

    if (newCandidateLeader > m_mediator.m_DSCommittee->size())
    {
        LOG_GENERAL(WARNING,
                    "View change counter is more than size of ds commitee. "
                    "This may be due view of ds committee is wrong. "
                        << m_mediator.m_currentEpochNum << "vc epoch: "
                        << vcblock.GetHeader().GetViewChangeEpochNo());
        newCandidateLeader
            = newCandidateLeader % m_mediator.m_DSCommittee->size();
    }

    if (!(m_mediator.m_DSCommittee->at(newCandidateLeader).second
              == vcblock.GetHeader().GetCandidateLeaderNetworkInfo()
          && m_mediator.m_DSCommittee->at(newCandidateLeader).first
              == vcblock.GetHeader().GetCandidateLeaderPubKey()))
    {

        LOG_GENERAL(
            WARNING,
            "View change expectation mismatched "
            "expected new leader: "
                << m_mediator.m_DSCommittee->at(newCandidateLeader).second
                << "actual vc new leader "
                << vcblock.GetHeader().GetCandidateLeaderNetworkInfo());
        return false;
    }
    // TODO
    // LogReceivedVSBlockDetails(vcblock);

    // Check the signature of this VC block
    if (!VerifyVCBlockCoSignature(vcblock))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "VCBlock co-sig verification failed");
        return false;
    }

    for (unsigned int x = 0; x < newCandidateLeader; x++)
    {
        UpdateDSCommiteeCompositionAfterVC(); // TODO: If VC select a random leader, we need to change the way we update ds composition.
    }

    // TDOO
    // Add to block chain and Store the VC block to disk.
    // StoreVCBlockToDisk(dsblock);

    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "I am a node and my view of leader is successfully changed.");
    return true;
}
