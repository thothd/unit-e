// Copyright (c) 2018 The unit-e core developers
// Copyright (c) 2017 The Particl Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <esperanza/miner.h>
#include <esperanza/stakethread.h>
#include <esperanza/validation.h>
#include <esperanza/stakingstate.h>
#include <miner.h>
#include <util.h>
#include <net.h>
#include <chainparams.h>
#include <validation.h>

namespace esperanza {

//! Pointers to all the stake threads.
static std::vector<StakeThread *> m_stakeThreads;

//! Flag indicating whether any one stake thread is currently active or not.
static std::atomic<bool> m_isStaking;

//! Flag to interrupt the stake threads.
static std::atomic<bool> m_stopMinerProcess;

static std::atomic<bool> m_tryToSync;

static int64_t m_minStakeInterval;

static int64_t m_minerSleep;

static std::atomic<int64_t> m_timeLastStake;

StakeThread::StakeThread(std::string name, std::thread &thread) : m_name(name), m_thread(std::move(thread)) {};

void StakeThread::Shutdown() {
  if (m_stakeThreads.size() < 1 || m_stopMinerProcess) {
    // no threads created or already flagged to stop
    return;
  }
  LogPrint(BCLog::POS, "ShutdownThreadStakeMiner\n");
  m_stopMinerProcess = true;
  for (auto t : m_stakeThreads) {
    {
      std::lock_guard<std::mutex> lock(t->m_mtxMinerProc);
      t->m_wakeMinerProc = true;
    }
    t->m_condMinerProc.notify_all();
    t->m_thread.join();
    delete t;
  }
  m_stakeThreads.clear();
}

void StakeThread::Wake(CWallet *wallet) {
  // Call when chain is synced, wallet unlocked or balance changed
  const size_t stakeThreadIndex = wallet->GetWalletExtension().m_stakeThreadIndex;
  LogPrint(BCLog::POS, "WakeThreadStakeMiner thread %d\n", stakeThreadIndex);
  if (stakeThreadIndex >= m_stakeThreads.size()) {
    return; // stake unit test
  }
  StakeThread *t = m_stakeThreads[stakeThreadIndex];
  wallet->GetWalletExtension().m_lastCoinStakeSearchTime = 0;

  {
    std::lock_guard<std::mutex> lock(t->m_mtxMinerProc);
    t->m_wakeMinerProc = true;
  }

  t->m_condMinerProc.notify_all();
}

bool StakeThread::IsStopped() {
  return m_stopMinerProcess;
}

void StakeThread::Start(size_t nThreadID, std::vector<CWallet *> &vpwallets, size_t nStart, size_t nEnd) {
  LogPrintf("Starting staking thread %d, %d wallet%s.\n", nThreadID, nEnd - nStart, (nEnd - nStart) > 1 ? "s" : "");

  int nBestHeight; // TODO: set from new block signal?
  int64_t nBestTime;

  int nLastImportHeight = ::Params().EsperanzaParams().GetLastImportHeight();

  if (!esperanza::g_config.m_staking) {
    LogPrint(BCLog::POS, "%s: -staking is false.\n", __func__);
    return;
  }

  CScript coinbaseScript;
  while (!m_stopMinerProcess) {
    if (fReindex || fImporting) {
      m_isStaking = false;
      LogPrint(BCLog::POS, "%s: Block import/reindex.\n", __func__);
      condWaitFor(nThreadID, 30000);
      continue;
    }
    if (m_tryToSync) {
      m_tryToSync = false;

      if (g_connman->GetNodeCount() < 3 || nBestHeight < esperanza::GetNumBlocksOfPeers()) {
        m_isStaking = false;
        LogPrint(BCLog::POS, "%s: TryToSync\n", __func__);
        condWaitFor(nThreadID, 30000);
        continue;
      }
    }
    if (g_connman->GetNodeCount() == 0 || IsInitialBlockDownload()) {
      m_isStaking = false;
      m_tryToSync = true;
      LogPrint(BCLog::POS, "%s: IsInitialBlockDownload\n", __func__);
      condWaitFor(nThreadID, 2000);
      continue;
    }

    {
      LOCK(cs_main);
      nBestHeight = chainActive.Height();
      nBestTime = chainActive.Tip()->nTime;
    }

    if (nBestHeight < esperanza::GetNumBlocksOfPeers() - 1) {
      m_isStaking = false;
      LogPrint(BCLog::POS,
               "%s: nBestHeight < GetNumBlocksOfPeers(), %d, %d\n",
               __func__,
               nBestHeight,
               esperanza::GetNumBlocksOfPeers());
      condWaitFor(nThreadID, m_minerSleep * 4);
      continue;
    }

    if (m_minStakeInterval > 0 && m_timeLastStake + m_minStakeInterval > GetTime()) {
      LogPrint(BCLog::POS, "%s: Rate limited to 1 / %d seconds.\n", __func__, m_minStakeInterval);
      condWaitFor(nThreadID, m_minStakeInterval * 500); // nMinStakeInterval / 2 seconds
      continue;
    }

    int64_t nTime = GetAdjustedTime();
    int64_t nMask = ::Params().EsperanzaParams().GetStakeTimestampMask(nBestHeight + 1);
    int64_t nSearchTime = nTime & ~nMask;
    if (nSearchTime <= nBestTime) {
      if (nTime < nBestTime) {
        LogPrint(BCLog::POS, "%s: Can't stake before last block time.\n", __func__);
        condWaitFor(nThreadID, std::min(1000 + (nBestTime - nTime) * 1000, (int64_t) 30000));
        continue;
      }
      int64_t nNextSearch = nSearchTime + nMask;
      condWaitFor(nThreadID, std::min(m_minerSleep + (nNextSearch - nTime) * 1000, (int64_t) 10000));
      continue;
    }

    std::unique_ptr<CBlockTemplate> pblocktemplate;

    size_t nWaitFor = 60000;
    for (size_t i = nStart; i < nEnd; ++i) {
      CWallet *pwallet = vpwallets[i];
      esperanza::WalletExtension &stakingWallet = pwallet->GetWalletExtension();

      if (nSearchTime <= stakingWallet.m_lastCoinStakeSearchTime) {
        nWaitFor = std::min(nWaitFor, (size_t) m_minerSleep);
        continue;
      }

      if (stakingWallet.m_stakeLimitHeight > 0 && nBestHeight >= stakingWallet.m_stakeLimitHeight) {
        stakingWallet.m_stakingState = esperanza::StakingState::NOT_STAKING_LIMITED;
        nWaitFor = std::min(nWaitFor, (size_t) 30000);
        continue;
      }

      if (pwallet->IsLocked()) {
        stakingWallet.m_stakingState = esperanza::StakingState::NOT_STAKING_LOCKED;
        nWaitFor = std::min(nWaitFor, (size_t) 30000);
        continue;
      }

      if (stakingWallet.GetStakeableBalance() <= stakingWallet.m_reserveBalance) {
        pwallet->GetWalletExtension().m_stakingState = esperanza::StakingState::NOT_STAKING_BALANCE;
        nWaitFor = std::min(nWaitFor, (size_t) 60000);
        stakingWallet.m_lastCoinStakeSearchTime = nSearchTime + 60;
        LogPrint(BCLog::POS, "%s: Wallet %d, low balance.\n", __func__, i);
        continue;
      }

      if (!pblocktemplate.get()) {
        pblocktemplate = BlockAssembler(::Params()).CreateNewBlock(coinbaseScript, true);
        if (!pblocktemplate.get()) {
          m_isStaking = false;
          nWaitFor = std::min(nWaitFor, (size_t) m_minerSleep);
          LogPrint(BCLog::POS, "%s: Couldn't create new block.\n", __func__);
          continue;
        }

        if (nBestHeight + 1 <= nLastImportHeight
            && !ImportOutputs(pblocktemplate.get(), nBestHeight + 1)) {
          m_isStaking = false;
          nWaitFor = std::min(nWaitFor, (size_t) 30000);
          LogPrint(BCLog::POS, "%s: ImportOutputs failed.\n", __func__);
          continue;
        }
      }

      stakingWallet.m_stakingState = esperanza::StakingState::IS_STAKING;
      nWaitFor = m_minerSleep;
      m_isStaking = true;
      if (stakingWallet.SignBlock(pblocktemplate.get(), nBestHeight + 1, nSearchTime)) {
        CBlock *pblock = &pblocktemplate->block;
        if (CheckStake(pblock)) {
          m_timeLastStake = GetTime();
          break;
        }
      } else {
        int nRequiredDepth =
            std::min((int) (::Params().EsperanzaParams().GetStakeMinConfirmations() - 1), (int) (nBestHeight / 2));
        if (stakingWallet.m_deepestTxnDepth < nRequiredDepth - 4) {
          stakingWallet.m_stakingState = esperanza::StakingState::NOT_STAKING_DEPTH;
          size_t nSleep = (nRequiredDepth - stakingWallet.m_deepestTxnDepth) / 4;
          nWaitFor = std::min(nWaitFor, (size_t) (nSleep * 1000));
          stakingWallet.m_lastCoinStakeSearchTime = nSearchTime + nSleep;
          LogPrint(BCLog::POS,
                   "%s: Wallet %d, no outputs with required depth, sleeping for %ds.\n",
                   __func__,
                   i,
                   nSleep);
          continue;
        }
      }
    }

    condWaitFor(nThreadID, nWaitFor);
  }
}

void StakeThread::StartStaking(const esperanza::Config &config, const std::vector<CWallet *> &wallets) {
  if (!config.m_staking) {
    LogPrintf("Staking disabled.\n");
    return;
  }
  const size_t numberOfWallets = wallets.size();
  if (numberOfWallets < 1) {
    LogPrintf("No wallets loaded, staking disabled.\n");
    return;
  }
  const size_t numberOfThreads = std::min(numberOfWallets, config.m_numberOfStakeThreads);
  const size_t walletsPerThread = numberOfWallets / numberOfThreads;
  for (size_t i = 0; i < wallets.size(); i++) {
    const size_t start = numberOfThreads * i;
    const size_t end = (i == numberOfThreads - 1) ? numberOfWallets : walletsPerThread * (i + 1);
    const std::string threadName(strprintf("miner%d", i));
    std::thread thread(&TraceThread<std::function<void()> >,
                       threadName.c_str(),
                       std::function<void()>(std::bind(&Start, i, wallets, start, end)));

    StakeThread *stakeThread = new StakeThread(threadName, thread);

    m_stakeThreads.push_back(stakeThread);
    wallets[i]->GetWalletExtension().m_stakeThreadIndex = i;
  }
}

void StakeThread::condWaitFor(size_t threadID, int ms) {
  assert(m_stakeThreads.size() > threadID);
  StakeThread *t = m_stakeThreads[threadID];
  t->condWaitFor(ms);
}

void StakeThread::condWaitFor(int ms) {
  std::unique_lock<std::mutex> lock(m_mtxMinerProc);
  m_wakeMinerProc = false;
  m_condMinerProc.wait_for(lock, std::chrono::milliseconds(ms), [this] { return this->m_wakeMinerProc; });
}

} // namespace esperanza