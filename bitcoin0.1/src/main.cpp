// Copyright (c) 2009 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"
#include "sha.h"

//
// Global state
//

CCriticalSection cs_main;

map<uint256, CTransaction> mapTransactions;
CCriticalSection cs_mapTransactions;
unsigned int nTransactionsUpdated = 0;
map<COutPoint, CInPoint> mapNextTx;

map<uint256, CBlockIndex *> mapBlockIndex;
const uint256 hashGenesisBlock("0x000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
CBlockIndex *pindexGenesisBlock = NULL;
int nBestHeight = -1;
uint256 hashBestChain = 0;
CBlockIndex *pindexBest = NULL;

map<uint256, CBlock *> mapOrphanBlocks;
multimap<uint256, CBlock *> mapOrphanBlocksByPrev;

map<uint256, CDataStream *> mapOrphanTransactions;
multimap<uint256, CDataStream *> mapOrphanTransactionsByPrev;

map<uint256, CWalletTx> mapWallet;
vector<pair<uint256, bool>> vWalletUpdated;
CCriticalSection cs_mapWallet;

map<vector<unsigned char>, CPrivKey> mapKeys;
map<uint160, vector<unsigned char>> mapPubKeys;
CCriticalSection cs_mapKeys;
CKey keyUser;

string strSetDataDir;
int nDropMessagesTest = 0;

//SATOSHI_S
// Settings
//SATOSHI_END
//Set to true in ui.cpp
int fGenerateBitcoins;
int64 nTransactionFee = 0;
CAddress addrIncoming;

//////////////////////////////////////////////////////////////////////////////
//
// mapKeys
//

bool AddKey(const CKey &key)
{
    CRITICAL_BLOCK(cs_mapKeys)
    {
        mapKeys[key.GetPubKey()] = key.GetPrivKey();
        mapPubKeys[Hash160(key.GetPubKey())] = key.GetPubKey();
    }
    return CWalletDB().WriteKey(key.GetPubKey(), key.GetPrivKey());
}

vector<unsigned char> GenerateNewKey()
{
    CKey key;
    key.MakeNewKey();
    if (!AddKey(key))
        throw runtime_error("GenerateNewKey() : AddKey failed\n");
    return key.GetPubKey();
}

//////////////////////////////////////////////////////////////////////////////
//
// mapWallet
//

//Tries to add transaction parameter to mapWallet. if it's a new transaction, or if the transaction is  already in mapWallet but has been updated, write the transaction to disk.
bool AddToWallet(const CWalletTx &wtxIn)
{
    //This method is called from 'AddToWalletIfMine', 'ProcessMessage', and 'CommitTransactionSpent'

    uint256 hash = wtxIn.GetHash();
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //SATOSHI_START
        // Inserts only if not already there, returns tx inserted or tx found
        //SATOSHI_END
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));

        //Gets the hash to wallet transaction map iterator from the pair, and gets the iterator that presumably points to the recently added transaction in the map. It's then dereferenced and the wallet is extracted.
        CWalletTx &wtx = (*ret.first).second;

        //I guess the second value in the pair represents whether this transaction was entered into 'mapWallet' before
        bool fInsertedNew = ret.second;

        //Set the time of the transaction if it was just inserted into 'mapWallet'
        if (fInsertedNew)
            wtx.nTimeReceived = GetAdjustedTime();

        //SATOSHI_START
        //// debug print
        //SATOSHI_END
        printf("AddToWallet %s  %s\n", wtxIn.GetHash().ToString().substr(0, 6).c_str(), fInsertedNew ? "new" : "update");

        //if the transaction is not new
        if (!fInsertedNew)
        {
            //SATOSHI_START
            // Merge
            //SATOSHI_END
            bool fUpdated = false;
            if (wtxIn.hashBlock != 0 && wtxIn.hashBlock != wtx.hashBlock)
            {
                wtx.hashBlock = wtxIn.hashBlock;
                fUpdated = true;
            }
            if (wtxIn.nIndex != -1 && (wtxIn.vMerkleBranch != wtx.vMerkleBranch || wtxIn.nIndex != wtx.nIndex))
            {
                wtx.vMerkleBranch = wtxIn.vMerkleBranch;
                wtx.nIndex = wtxIn.nIndex;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
            if (wtxIn.fSpent && wtxIn.fSpent != wtx.fSpent)
            {
                wtx.fSpent = wtxIn.fSpent;
                fUpdated = true;
            }

            //if there's no update to perform, just return true
            if (!fUpdated)
                return true;
        }

        //SATOSHI_START
        // Write to disk
        //SATOSHI_END
        if (!wtx.WriteToDisk())
            return false;

        // Notify UI
        vWalletUpdated.push_back(make_pair(hash, fInsertedNew));
    }

    // Refresh UI
    MainFrameRepaint();
    return true;
}

//If general transaction passed in (of type CTransaction) returns true for it's 'IsMine' method, then the transaction will get made into a CWalletTx which will have a merkle branch set if the transaction is in a block.
bool AddToWalletIfMine(const CTransaction &tx, const CBlock *pblock)
{
    if (tx.IsMine() || mapWallet.count(tx.GetHash()))
    {
        CWalletTx wtx(tx);
        // Get merkle branch if transaction was found in a block
        if (pblock)
            wtx.SetMerkleBranch(pblock);
        return AddToWallet(wtx);
    }
    return true;
}

//Calls mapWallet.erase using the hash, and CWalletDB.EaraseTx using hash
bool EraseFromWallet(uint256 hash)
{
    CRITICAL_BLOCK(cs_mapWallet)
    {
        if (mapWallet.erase(hash))
            CWalletDB().EraseTx(hash);
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

//Adds transaction represented as CDataStream to mapOrphanTransactions (a hash-to-CDataStrem map) and adds a reference to the orphan transaction's CDataStream using its previous transaction hashes
void AddOrphanTx(const CDataStream &vMsg)
{
    //Declares transaction
    CTransaction tx;

    //Reading data stream into transaction
    CDataStream(vMsg) >> tx;

    //Get hash of transaction
    uint256 hash = tx.GetHash();
    
    //if transaction is already in maporphanTransactions
    if (mapOrphanTransactions.count(hash))
        //return
        return;

        //The hash in mapOrphanTransaction will be linked to a CDataStream
    CDataStream *pvMsg = mapOrphanTransactions[hash] = new CDataStream(vMsg);

    //For each transaction in input for this orphan transaction
    foreach (const CTxIn &txin, tx.vin)
        //Insert the orphan transaction using it's lost parent as key
        mapOrphanTransactionsByPrev.insert(make_pair(txin.prevout.hash, pvMsg));
}


//Erase orphan transaction and all source hashes that lead to orphan transactions in mapOrphanTransactions and mapOrphanTransactionsByPrev respectively
void EraseOrphanTx(uint256 hash)
{
    //return if there's no orphan transaction with that hash
    if (!mapOrphanTransactions.count(hash))
        return;

    //Get the orphan transaction
    const CDataStream *pvMsg = mapOrphanTransactions[hash];
    CTransaction tx;

    //Read the orphan transaction as a CDataStream into a reguar CTransaction
    CDataStream(*pvMsg) >> tx;

    //For ever input transaction
    foreach (const CTxIn &txin, tx.vin)
    {
        //For all transactions from mapOrphanTransactionsByPrev wih a hash value greater than or equal to the hash and will stop when the hash is greater than the hash
        //Maybe this is the only way to retrieve the source transaction from multimap
        for (multimap<uint256, CDataStream *>::iterator mi = mapOrphanTransactionsByPrev.lower_bound(txin.prevout.hash);
             mi != mapOrphanTransactionsByPrev.upper_bound(txin.prevout.hash);)
        {
            //If the prev hash in maptransactionPrev points to orphan
            if ((*mi).second == pvMsg)
                //erase the prev transaction at iterator
                mapOrphanTransactionsByPrev.erase(mi++);
            else
                mi++;
        }
    }

    //Delete orphan transaction reference
    delete pvMsg;
    //erase the hash from map
    mapOrphanTransactions.erase(hash);
}

//////////////////////////////////////////////////////////////////////////////
//
// CTransaction
//

//Retrieves source transaction of this transaction from mapWallet. Calls 'IsMine' on the vout of the retrieved input transaction.
bool CTxIn::IsMine() const
{
    //Add a lock to cs_mapWallet
    //cs_mapWallet works like a mutex
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //This line tries to make an iterator point to the transaction that powers this one
        //prevout has metadata to the source transaction from TxIn
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(prevout.hash);

        //If the source transaction wasn't found, the iterator reaches the end of mapWallet
        if (mi != mapWallet.end())
        {
            //After transaction is retrieved, verify the payout by calling 'IsMine' on vout

            //prev is the source transaction (CWalletTx)
            const CWalletTx &prev = (*mi).second;

            //If the input transaction's reference to the source transaction's output index is valid
            if (prevout.n < prev.vout.size())
                //get the output of the source transaction that powers this one
                if (prev.vout[prevout.n].IsMine())
                    return true;
        }
    }
    return false;
}

//gets the source transaction using the prevout hash (since this is output metadata). looks at the output of source transaction to see if 'isMine' is true and locates the correct output of the source transaction using 'prevout.n'. if it is true, return the 'nValue' from output slot
int64 CTxIn::GetDebit() const
{
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //Get the source transactions
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(prevout.hash);

        //if its valid transaction
        if (mi != mapWallet.end())
        {
            //get the transaction (I think '.first' is the hash code)
            const CWalletTx &prev = (*mi).second;

            //If the output slot on the source transaction is valid
            if (prevout.n < prev.vout.size())
                if (prev.vout[prevout.n].IsMine())
                    //Get the value of the output
                    return prev.vout[prevout.n].nValue;
        }
    }
    return 0;
}

int64 CWalletTx::GetTxTime() const
{
    //fTimeReceivedIsTxTime is turned true on 'submitOrder' action of 'ProcessMessage' method, and turned true in 'createTransaction'. It represents whether a valid value is in 'nTimeReceived'
    //HashBlock should be the block that the transaction ended up in. It's defined on the MerkleTx that CWalletTx derives from
    if (!fTimeReceivedIsTxTime && hashBlock != 0)
    {

        //SATOSHI_START
        // If we did not receive the transaction directly, we rely on the block's
        // time to figure out when it happened.  We use the median over a range
        // of blocks to try to filter out inaccurate block times.
        //SATOSHI_END
        map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex *pindex = (*mi).second;
            if (pindex)
                return pindex->GetMedianTime();
        }
    }
    return nTimeReceived;
}

//If not fClient, get the block transaction is located in,get merkle branch of transaction at block index using the block method, sets the transaction merkle branch to that, and returns block height
//If fClient but has a hashblock assigned, just return height of block
int CMerkleTx::SetMerkleBranch(const CBlock *pblock)
{
    //fClient is set to true
    //How is global fClient changed?
    if (fClient)
    {
        if (hashBlock == 0)
            return 0;
    }
    else
    {
        CBlock blockTmp;

        //Defaults to null. pblock is found in parameter
        if (pblock == NULL)
        {
            //SATOSHI_START
            // Load the block this tx is in
            //SATOSHI_END
            CTxIndex txindex;

            //gets txIndex of db for the current instances transaction, using its hash
            if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
                return 0;

            //Reads the block using this transaction's index information (found using transaction hash) into blockTmp
            if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, true))
                return 0;

            //pblock (ref to current block) is set to the reference of the block corresponding to this transaction
            pblock = &blockTmp;
        }

        //SATOSHI_START
        // Update the tx's hashBlock
        //SATOSHI_END
        hashBlock = pblock->GetHash();

        //SATOSHI_START
        // Locate the transaction
        //SATOSHI_END

        //Iterating through all transactions (vtx is list of transactions) in block
        for (nIndex = 0; nIndex < pblock->vtx.size(); nIndex++)
            //if the block we located contains this transaction, break
            if (pblock->vtx[nIndex] == *(CTransaction *)this)
                break;

        //Otherwise if we didn't see this transaction in the block
        if (nIndex == pblock->vtx.size())
        {
            //The vector of hashes (vector<uint256>) for this transaction is cleared
            vMerkleBranch.clear();
            //Index is set to -1
            nIndex = -1;
            //Error is logged
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        //SATOSHI_START
        // Fill in merkle branch
        //SATOSHI_END

        //The MerkleBranch for this transaction is equal to the MerkleBranch for the block it's held within
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    //SATOSHI_START
    // Is the tx in a block that's in the main chain
    //SATOSHI_END
    //Gets the iterator pointing at the block that contains this transaction by looking at the block hash taken from above
    map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(hashBlock);

    //If the block index for the block containing this transaction isn't found
    if (mi == mapBlockIndex.end())
        //return
        return 0;

    //Get the block index
    CBlockIndex *pindex = (*mi).second;

    //Validation making sure index exists and it's in the main chain
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    //return the difference in height between this block index and the index corresponding to longest chain
    return pindexBest->nHeight - pindex->nHeight + 1;
}

//Adds whole transactions from source transaction in vin to vtxPrev on this CWalletTx
void CWalletTx::AddSupportingTransactions(CTxDB &txdb)
{

    //I genuinely don't know what is in this
    //What is vtxPrev?
    vtxPrev.clear();

    //Why is depth at 3?
    const int COPY_DEPTH = 3;

    //SetMerkleBranch sets the branch of this transaction by looking at the branch assigned to it in the context of a block. It also returns the depth
    if (SetMerkleBranch() < COPY_DEPTH)
    {
        //queue of hashes
        vector<uint256> vWorkQueue;

        //for each input transaction
        foreach (const CTxIn &txin, vin)
            //we're getting the hash of the output from the source transaction
            vWorkQueue.push_back(txin.prevout.hash);

        //SATOSHI_START
        // This critsect is OK because txdb is already open
        //SATOSHI_END
        CRITICAL_BLOCK(cs_mapWallet)
        {
            //Making a new wallet that; hash to merkle transaction
            map<uint256, const CMerkleTx *> mapWalletPrev;

            //set of hashes?
            set<uint256> setAlreadyDone;

            //For every index of work queue, which has all source transaction hashes for his transaction (identified using txin)
            for (int i = 0; i < vWorkQueue.size(); i++)
            {
                //We get the hash of a source transaction output
                uint256 hash = vWorkQueue[i];

                //We see if the hash is in set of seen, and continue if it is
                if (setAlreadyDone.count(hash))
                    continue;

                //we put the hash in set
                setAlreadyDone.insert(hash);

                //a new transaction is made
                CMerkleTx tx;

                //We count how many times this hash is in mapWallet (which should be only once since it's a map?)
                //Maybe just checking presence of hash
                if (mapWallet.count(hash))
                {
                    //newly made tansaction variable is equal to the retrieval of hash. So this is set to a source transaction
                    tx = mapWallet[hash];

                    //For each merkle transaction from vtxPrev of source transaction
                    foreach (const CMerkleTx &txWalletPrev, mapWallet[hash].vtxPrev)
                        //Adds vtxPrev on the source transaction to the local mapWalletPrev
                        mapWalletPrev[txWalletPrev.GetHash()] = &txWalletPrev;
                }
                //if mapWalletPrev has this transaction
                //If it hits this, I guess the transaction was a source transaction of one of the original source transactions
                else if (mapWalletPrev.count(hash))
                {
                    //Then set the source transaction to the transaction value from there
                    tx = *mapWalletPrev[hash];
                }
                else if (!fClient && txdb.ReadDiskTx(hash, tx))
                {
                    ;
                }
                else
                {
                    printf("ERROR: AddSupportingTransactions() : unsupported transaction\n");
                    continue;
                }

                //Get depth of transaction (which looks at block height)
                int nDepth = tx.SetMerkleBranch();
                //Adds the source transaction to vtxPrev in the ideal case (when first condition above is hit)
                vtxPrev.push_back(tx);

                //What is nDepth? If it's less than COPY_DEPTH
                if (nDepth < COPY_DEPTH)
                    //Add more things to the work queue this is iterating through. By more things, it means to add the source transactions of the current source transaction (which is probably why v was made; as a cache).

                    //Things in mapWalletPrev seems to not have to have been in curren mapWallet
                    foreach (const CTxIn &txin, tx.vin)
                        vWorkQueue.push_back(txin.prevout.hash);
            }
        }
    }

    //Latest transactions first?
    reverse(vtxPrev.begin(), vtxPrev.end());
}

//Check transaction with validation method, make all source transactions point to this one in their vout (using connect inputs), and add this transaction to the memory pool. if the transaction was already added to the memory pool, update it somehow
bool CTransaction::AcceptTransaction(CTxDB &txdb, bool fCheckInputs, bool *pfMissingInputs)
{
    //if the reference is true? Maybe there's implicit de-referencing?
    if (pfMissingInputs)
        //then set to false
        *pfMissingInputs = false;

    //SATOSHI_START
    // Coinbase is only valid in a block, not as a loose transaction
    //SATOSHI_END
    //Whenever this is true inside a transaction, throw an error?
    //Could also be named as 'throwIfInAcceptTransactionAndIsCoinbase' :p </s>

    //IsCoinBase evaluates to '(vin.size() == 1 && vin[0].prevout.IsNull());' which means:
    /*
        If there is only one source transaction and the reference to the output of the source transaction doesn't lead to anything
    */
    if (IsCoinBase())
        return error("AcceptTransaction() : coinbase as individual tx");

    //What is CheckTransaction?
    //It's defined on CTransaction. Basically does basic validation for transaction. One of the things it checks for is if this transaction is a coinbase. It's also checked above and probably kept separate because of the need for more descriptive error messages (although he could've put the error messages in the one returned for full context)
    if (!CheckTransaction())
        return error("AcceptTransaction() : CheckTransaction failed");

    //SATOSHI_START
    // Do we already have it?
    //SATOSHI_END
    uint256 hash = GetHash();
    CRITICAL_BLOCK(cs_mapTransactions)
    //What is mapTransactions?
    //Return false if the current transaction hash isn't in mapTransactions.
    //SIDENOTE: Why not just use GetHash everywhere and cache value in the method? Why would I prefer this? We can build up familiarity with the method and give the reader an immediate understanding whenever they see the invocation (wheras the variable names could vary and have to be checked that it was assigned to 'GetHash')
    if (mapTransactions.count(hash))
        return false;
    //If parameter is true
    if (fCheckInputs)
        //And the transaction db has the hash of this transaction
        if (txdb.ContainsTx(hash))
            //just return false
            return false;

    //SATOSHI_START
    // Check for conflicts with in-memory transactions
    //SATOSHI_END
    //Pointer to transaction made
    CTransaction *ptxOld = NULL;

    //For every input transaction
    for (int i = 0; i < vin.size(); i++)
    {
        //Get the metadata of the source transaction's output transaction (which is what 'prevout' is)
        COutPoint outpoint = vin[i].prevout;

        //What is 'mapNextTx'?
        //Seems to be a map from a source transaction output to current transaction. Only place I see this is populated is in 'AddToMemoryPool' which I believe should only get called from here.
        //Check if the metadata to source transaction output is present in 'mapNextTx'
        if (mapNextTx.count(outpoint))
        {
            //SATOSHI_START
            // Allow replacing with a newer version of the same transaction
            //SATOSHI_END
            //Why does i have to be zero to continue?
            //If this has an input that has been put in mapNextTx, then it may mean this is being processed again. The case of i=0 is okay because...idk
            //Maybe there is signigicance in first index of vin? Satoshi comment talks about 'replacing'
            if (i != 0)
                return false;

            //Get the output's ptx?
            //What is mapNextTx point to?
            ptxOld = mapNextTx[outpoint].ptx;

            //If this transaction is NOT newer than this other transaction (so mapNextTx maps COutPoint to CTransaction)
            if (!IsNewerThan(*ptxOld))
                //this transaction is not accepted
                return false;

            //for all input transactions
            for (int i = 0; i < vin.size(); i++)
            {
                //get source output metadata
                COutPoint outpoint = vin[i].prevout;

                //if it's NOT in mapNextTx or it's not equal to transaction found in mapNextOld where 'i' was 0

                //Alternatively, if there's a source transaction's output not in mapNextTx, return false. In which case would ptxOld not have been retrieved from a vin reference that wasn't in mapNextTx?
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    //SATOSHi_START
    // Check against previous transactions
    //SATOSHI_END
    map<uint256, CTxIndex> mapUnused;

    //fees is 0?
    int64 nFees = 0;

    //If parameter says check inputs and not able to ConnectInputs (ConnectInputs is false) using txdb and new mapUnused
    if (fCheckInputs && !ConnectInputs(txdb, mapUnused, CDiskTxPos(1, 1, 1), 0, nFees, false, false))
    {
        //set missing inputs to true
        if (pfMissingInputs)
            *pfMissingInputs = true;
        return error("AcceptTransaction() : ConnectInputs failed %s", hash.ToString().substr(0, 6).c_str());
    }

    //SATOSH_START
    // Store transaction in memory
    //SATOSHI_END
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        //if first input transaction went through the if statement above
        if (ptxOld)
        {
            printf("mapTransaction.erase(%s) replacing with new version\n", ptxOld->GetHash().ToString().c_str());
            //replace it's entry by first erasing before adding
            mapTransactions.erase(ptxOld->GetHash());
        }

        //adds current transaction to memory pool
        AddToMemoryPool();
    }

    //SATOSHI_START
    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    //SATOSHI_END
    if (ptxOld)
        EraseFromWallet(ptxOld->GetHash());

    printf("AcceptTransaction(): accepted %s\n", hash.ToString().substr(0, 6).c_str());
    return true;
}

//Adds this transaction to maptransactions and adds it to mapNextTx as well using the prevout reference of each input (not prevout hash...)
//Memory pool is local map
bool CTransaction::AddToMemoryPool()
{

    //SATOSHI_START
    // Add to memory pool without checking anything.  Don't call this directly,
    // call AcceptTransaction to properly check the transaction first.
    //SATOSHI_END
    //Essentially should be private, but it's in herited so has to be protect :/
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        uint256 hash = GetHash();
        //Sets the current transacion into mapTransacions
        mapTransactions[hash] = *this;

        //And adds all input transaction's output metadata mapped to a CInPoint with a reference of this transaction and 'i'
        for (int i = 0; i < vin.size(); i++)
            mapNextTx[vin[i].prevout] = CInPoint(&mapTransactions[hash], i);
        //Global count of how many transactions were updated from memory pool
        nTransactionsUpdated++;
    }
    return true;
}

//Removes the transaction from mapTransactions and erases source transaction references to it from mapNextTx (which takes in a source transaction's prevout).
bool CTransaction::RemoveFromMemoryPool()
{
    //SATOSHI_START
    // Remove transaction from memory pool
    //SATOSHI_END
    CRITICAL_BLOCK(cs_mapTransactions)
    {

        //self-explanatory, it erases all prevouts for source vins in both mapNextTx and mapTransactions
        foreach (const CTxIn &txin, vin)
            mapNextTx.erase(txin.prevout);
        mapTransactions.erase(GetHash());
        nTransactionsUpdated++;
    }
    return true;
}

//Get Block transaction is part of and make sure it's in main chain, then checks the merkle branch of the block with this transaction hash if not already done, then returns heigh of main branch - (height of this branch + 1)
int CMerkleTx::GetDepthInMainChain() const
{
    //Not going to worry about wht these values are in this context
    //Actually we've seen hashBlock get set in 'SetMerkleBranch' I believe
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    //SATOSHI_START
    // Find the block it claims to be in
    //SATOSHI_END
    //Satoshi's comment is apt here. Uses hashblock
    map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(hashBlock);

    //If reference is equal to end, retuen
    if (mi == mapBlockIndex.end())
        return 0;

    //Get block pointer
    CBlockIndex *pindex = (*mi).second;

    //if it's not in main chain, just terminate
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    //SATOSHI_START
    // Make sure the merkle branch connects to this block
    //SATOSHI_END

    //I don't know wher fMerkleVerified is made true, but if it's not true (which means it's initialized and hasn't been through this method since it's the only place I see that can change fMerkleVerified to true)
    if (!fMerkleVerified)
    {
        //Ah, I see here is where we can verify

        //If the hash of the merkle transaction is no bueno from a merklebranch (on this current transaction) that can't generate the hashMerkleRoot
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            //finish
            return 0;

        //Otherwise it is verified
        fMerkleVerified = true;
    }

    //retrun height using pindexes
    //pindex is block pointer
    return pindexBest->nHeight - pindex->nHeight + 1;
}

//Only works for coinbase transaction and returns difference of depth in main chain to '120'.  If the main chain is really long and makes the value negative, the max function will bring it up to 0
int CMerkleTx::GetBlocksToMaturity() const
{
    //If there is one source transaction that doesn't lead anywhere
    if (!IsCoinBase())
        return 0;

    //Waht is COINBASE_MATURITY?
    //Still have to understand how Merkle depth is calculated
    return max(0, (COINBASE_MATURITY + 20) - GetDepthInMainChain());
}

//Accepts the transaction for a fClient if the transaction is not in a main chain and is able to successfully clientConnectInputs, and accepts the transaction unconditionally if not an fClient. Check inputs is set to tru when accepting the transaction if not client.
bool CMerkleTx::AcceptTransaction(CTxDB &txdb, bool fCheckInputs)
{

    //Weird that fCheckInputs is a bool since a boolen prefix is usually 'b' in hungarian notation

    //What is an fClient?
    //If client?
    if (fClient)
    {
        //Transaction must be in mainchain and not have client connect inputs to be accepted?
        if (!IsInMainChain() && !ClientConnectInputs())
            return false;

        //Inputs need not be checked if it's a client?
        return CTransaction::AcceptTransaction(txdb, false);
    }
    else
    {
        //Calls base class's accept tranaction if not client
        return CTransaction::AcceptTransaction(txdb, fCheckInputs);
    }
}

//Accepts all source transactions, then accepts this instance transaction
bool CWalletTx::AcceptWalletTransaction(CTxDB &txdb, bool fCheckInputs)
{
    //locks mapTransactions using 'cs_mapTransactions'
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        //iterating through all source transactions
        foreach (CMerkleTx &tx, vtxPrev)
        {
            //if the source transaction isn't a coinbase (which I think is just the first transaction in a block?)
            if (!tx.IsCoinBase())
            {
                //Get the hash of the transaction
                uint256 hash = tx.GetHash();

                //If the source transaction can't be found in local mapTransactions or txdb
                if (!mapTransactions.count(hash) && !txdb.ContainsTx(hash))
                    //Just accept the transaction to transfer it
                    tx.AcceptTransaction(txdb, fCheckInputs);
            }
        }

        //If this transaction isn't a coinbase
        if (!IsCoinBase())
            //accept the transaction
            return AcceptTransaction(txdb, fCheckInputs);
    }
    return true;
}

//Looks through all transactions in walletMap and accepts the ones that aren't in the transaction db
void ReacceptWalletTransactions()
{
    //SATOSHI_START
    // Reaccept any txes of ours that aren't already in a block
    //SATOSHI_END

    //open transaction db to read it?
    CTxDB txdb("r");
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //for each entry in mapWallet
        foreach (PAIRTYPE(const uint256, CWalletTx) & item, mapWallet)
        {
            //Take the transaction object
            CWalletTx &wtx = item.second;

            //if it's not a coinbase and if it's not present in the transaction db
            if (!wtx.IsCoinBase() && !txdb.ContainsTx(wtx.GetHash()))
                //Accept the transaction
                wtx.AcceptWalletTransaction(txdb, false);
        }
    }
}

//Relay all source transactions  (using RelayMessage) if not in db, then try to relay this transaction
void CWalletTx::RelayWalletTransaction(CTxDB &txdb)
{

    //For every source transaction
    foreach (const CMerkleTx &tx, vtxPrev)
    {
        //If the transaction isn't a coinbase
        if (!tx.IsCoinBase())
        {
            //Get the hash
            uint256 hash = tx.GetHash();
            //If the transaction db doesn't contain the hash
            if (!txdb.ContainsTx(hash))
                //Relay Message it (I imagine this means to broadcast?)
                RelayMessage(CInv(MSG_TX, hash), (CTransaction)tx);
        }
    }

    //If not coin base
    if (!IsCoinBase())
    {
        //get hash of this transaction
        uint256 hash = GetHash();

        //if db doesn't contain this transaction
        if (!txdb.ContainsTx(hash))
        {
            printf("Relaying wtx %s\n", hash.ToString().substr(0, 6).c_str());
            //relay this transaction
            RelayMessage(CInv(MSG_TX, hash), (CTransaction) * this);
        }
    }
}

//Tries to call RelayWalletTransaction in order on every wallet in mapWallet
void RelayWalletTransactions()
{

    //This is static so should be 0-initialized
    static int64 nLastTime;

    //If the time difference between now and the last time this was run is less than 600(ms?)
    if (GetTime() - nLastTime < 10 * 60)
        //terminate
        return;

    //Change the last time this was run to now
    nLastTime = GetTime();

    //SATOSHI_START
    // Rebroadcast any of our txes that aren't in a block yet
    //SATOSHI_END
    printf("RelayWalletTransactions()\n");
    CTxDB txdb("r");
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //SATOSHI_START
        // Sort them in chronological order
        //SATOSHI_END

        //Not sure how multimap works
        multimap<unsigned int, CWalletTx *> mapSorted;

        //for each transaction in wallet
        foreach (PAIRTYPE(const uint256, CWalletTx) & item, mapWallet)
        {
            //get the transaction
            CWalletTx &wtx = item.second;
            //and put it in mapSorted
            mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        //For every sorted transaction
        foreach (PAIRTYPE(const unsigned int, CWalletTx *) & item, mapSorted)
        {
            //get the transaction
            CWalletTx &wtx = *item.second;
            //and relay the transaction to db
            wtx.RelayWalletTransaction(txdb);
        }
    }
}
//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

//Calls another 'ReadFromDisk' method with destructured parameters
bool CBlock::ReadFromDisk(const CBlockIndex *pblockindex, bool fReadTransactions)
{
    //Just destructures and calls another from of 'ReadFromDisk'
    return ReadFromDisk(pblockindex->nFile, pblockindex->nBlockPos, fReadTransactions);
}

//Gets the hashPrevBlock property on the block from mapOrphanBlocks until it can't find any more previous blocks (hence finding root block)
uint256 GetOrphanRoot(const CBlock *pblock)
{
    //SATOSHI_START
    // Work back to the first block in the orphan chain
    //SATOSHI_END
    //What is mapOrphanBlocks?
    //Looks like a global variable like mapWallet and mapTransaction. Why orphan?

    //As long as a previous block exists
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        //retireve it
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];

    //get hash once you can't go to another previous block
    return pblock->GetHash();
}

//Add on subsidy onto parameter fees
int64 CBlock::GetBlockValue(int64 nFees) const
{
    //subsidy is 50 coins?
    int64 nSubsidy = 50 * COIN;

    //SATOSHI_START
    // Subsidy is cut in half every 4 years
    //SATOSHI_END
    //Not sure what 210000 is, especially in the context of nBestHeight
    nSubsidy >>= (nBestHeight / 210000);

    return nSubsidy + nFees;
}

//caclulates the time between pindexFirst and pindexLast, and calculates bnNew which is nActualTimespan/nTargetTimespan after the 'setCompact' is called on it using pindexLast. Also enforce bNew has to have a cap of bnProofOfWorkLimit
unsigned int GetNextWorkRequired(const CBlockIndex *pindexLast)
{

    //As Satoshi's comment says, this represents 2 weeks in seconds
    const unsigned int nTargetTimespan = 14 * 24 * 60 * 60; //<STAOSHI_START>// two weeks</SATOSHI_END>

    //600 seconds (10 minutes) would be keeping up with the target timespan above
    const unsigned int nTargetSpacing = 10 * 60;



    //nInterval comes out to the value 2016. This is how many blocks satoshi would like mined within a 2 week interval which should be about a block every 10 minutes
    //desiredTimespan/blocksMined = targetSpacing 
    //Where blocksMined = nInterval
    const unsigned int nInterval = nTargetTimespan / nTargetSpacing;

    //SATOSHI_START
    // Genesis block
    //SATOSHI_END

    //Is pindexLast the last block "mined"?
    //I guess it being null means it's the genesis block as per satoshi comment
    //It's the parameter and is a block pointer
    if (pindexLast == NULL)
        //What is bnProofofWorkLimit?
        //Intialized to 127 bits
        return bnProofOfWorkLimit.GetCompact();

    //SATOSHI_START
    // Only change once per interval
    //SATOSHI_END

    //If pindexLast (the lastBlock?) has a height that's not divisible by the interval
    if ((pindexLast->nHeight + 1) % nInterval != 0)
        //return the pindexLast's 'nBits'
        return pindexLast->nBits;

    //SATOSHI_START
    // Go back by what we want to be 14 days worth of blocks
    //SATOSHI_END

    //pindexFIrst becomes the last?
    const CBlockIndex *pindexFirst = pindexLast;
    //For when pindexFirst is valid and i is less than nInterval
    for (int i = 0; pindexFirst && i < nInterval - 1; i++)
        //Keep moving pIndexFirst to the previous block; kind of like that orphan block root method
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    //SATOSHI_START
    // Limit adjustment step
    //SATOSHI_END

    //The time between the linked blocks?
    unsigned int nActualTimespan = pindexLast->nTime - pindexFirst->nTime;
    printf("  nActualTimespan = %d  before bounds\n", nActualTimespan);

    //it looks like the timespan has to be divisible by 4? (at least if nActualTimespan is an integer)
    //Or is it saying the actual timespan has to be within a range of 4 times higher and lower than the target
    if (nActualTimespan < nTargetTimespan / 4)
        nActualTimespan = nTargetTimespan / 4;
    if (nActualTimespan > nTargetTimespan * 4)
        nActualTimespan = nTargetTimespan * 4;

    //SATOSHI_START
    // Retarget
    //SATOSHI_END

    //Makes a big number
    CBigNum bnNew;
    //Not sure what SetCOmpact does but it takes in the last block's nBits?
    //SetCompact just reads the nBits into the bignum 'bnNew'
    bnNew.SetCompact(pindexLast->nBits);

    //It multiplies it by the actual timespan
    bnNew *= nActualTimespan;

    //And divides it by target timespan. Not sure what this acheives...nTargetTimespan is 2 weeks
    bnNew /= nTargetTimespan;

    //bNew is old_difficulty*(block_addition_rate) where block_addition_rate = time_taken_to_actually_make_2weeks_of_blocks/2weeks
    //slows in slow mining and fastens?

    //if bnNew which presumably represens the time exeeds bnProofOfWorkLimit (time limit?)
    if (bnNew > bnProofOfWorkLimit)
        //Then set the bnNew o the proof of work limit
        bnNew = bnProofOfWorkLimit;

    //SATOSHI_START
    /// debug print
    //SATOSHI_END
    printf("\n\n\nGetNextWorkRequired RETARGET *****\n");
    printf("nTargetTimespan = %d    nActualTimespan = %d\n", nTargetTimespan, nActualTimespan);
    printf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    printf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    //Gets compact...
    return bnNew.GetCompact();
}

//For non-coinbase transactions, this clears all source transaction indexes 'vSpent' and erases the index to this instance transaction from txdb using the reference to this transaction
bool CTransaction::DisconnectInputs(CTxDB &txdb)
{

    //SATOSHI_START
    // Relinquish previous transactions' spent pointers
    ////SATOSHI_END
    if (!IsCoinBase())
    {
        //for each input transaction
        foreach (const CTxIn &txin, vin)
        {
            //gets its prevout (output of source transaction)
            COutPoint prevout = txin.prevout;

            //SATOSHI_START
            // Get prev txindex from disk
            ////SATOSHI_END

            //gets an index for source transaction
            CTxIndex txindex;
            //reads it from db
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            //I guess the index can be queried to get vSpent.
            //What is prevout.n, the index?
            //What is vSpent?
            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");
            //SATOSHI_START
            // Mark outpoint as not spent
            ////SATOSHI_END
            //Mark the index's vSpent at that index as null
            txindex.vSpent[prevout.n].SetNull();
            //SATOSHI_START
            // Write back
            ////SATOSHI_END

            //update the transaction as not having vSpent
            txdb.UpdateTxIndex(prevout.hash, txindex);
        }
    }

    //SATOSHI_START
    // Remove transaction from index
    //SATOSHI_END
    //Try to erase transaction
    if (!txdb.EraseTxIndex(*this))
        return error("DisconnectInputs() : EraseTxPos failed");

    return true;
}

//If not coinbase
//For every source transaction try to get it's txindex, error out if (it can't find it and fBlock or fMiner are true).
//Retrieve source transaction found in mapTransactions. Try to read txindex again using the retrieved transaction if not prreviously found. 
//Verify the signature on prevTxIndex and vin, and mark the txindex as spent and update the source transaction with the index in txDb or maptestPool
//Also try and update ref to nFees
bool CTransaction::ConnectInputs(CTxDB &txdb, map<uint256, CTxIndex> &mapTestPool, CDiskTxPos posThisTx, int nHeight, int64 &nFees, bool fBlock, bool fMiner, int64 nMinFee)
{
    //SATOSHI_START
    // Take over previous transactions' spent pointers
    //SATOSHI_END
    //If not coinbase
    if (!IsCoinBase())
    {
        int64 nValueIn = 0;
        //For all inputs
        for (int i = 0; i < vin.size(); i++)
        {
            //Get the output metadata from source transaction
            COutPoint prevout = vin[i].prevout;

            //SATOSHI_START
            // Read txindex
            //SATOSHI_END
            //Declare transaction index
            CTxIndex txindex;
            //Set fFound to true? If this is unconditionally hit, why initialize it as variable?
            bool fFound = true;

            //If fMiner and mapTestPool has the source transaction
            if (fMiner && mapTestPool.count(prevout.hash))
            {
                //SATOSHI_START
                // Get txindex from current proposed changes
                //S_END
                //mapTestPool is proposed changes?
                txindex = mapTestPool[prevout.hash];
            }
            else
            {
                //S_START
                // Read txindex from txdb
                //S_END
                //If only one or none of above conditions hold true, we can't read it from mapTestPool and need to get it from txdb. The return value is fFound.
                fFound = txdb.ReadTxIndex(prevout.hash, txindex);
            }

            //If source transaction was NOT found and parameters fBlock and fMiner are true, then return false. But in the case of fMiner, then there's just an error thrown.
            if (!fFound && (fBlock || fMiner))
                return fMiner ? false : error("ConnectInputs() : %s prev tx %s index entry not found", GetHash().ToString().substr(0, 6).c_str(), prevout.hash.ToString().substr(0, 6).c_str());

            //S_START
            // Read txPrev
            //S__END
            //Make an empty Transaction
            CTransaction txPrev;
            //If not found and not fBlock or fMiner from above, or if the transaction index's position is (1,1,1) (idk significance of that; maybe it's default if txindex wasn't modified from declaration above)
            //txindex is default if not fMiner and source not in mapTestPool
            if (!fFound || txindex.pos == CDiskTxPos(1, 1, 1))
            {
                //S_START
                // Get prev tx from single transactions in memory
                //S_END
                CRITICAL_BLOCK(cs_mapTransactions)
                {
                    //If the transaction is not found in mapTRansactions, throw an error
                    if (!mapTransactions.count(prevout.hash))
                        return error("ConnectInputs() : %s mapTransactions prev not found %s", GetHash().ToString().substr(0, 6).c_str(), prevout.hash.ToString().substr(0, 6).c_str());
                    //Get the whole previous transaction from mapTransactions
                    txPrev = mapTransactions[prevout.hash];
                }
                //If fFound is false
                if (!fFound)
                    //The transaction index's vspent is resized to the output of
                    txindex.vSpent.resize(txPrev.vout.size());
            }
            else
            {
                //S
                // Get prev tx from disk
                //S_E
                //Read the transaction at txindex.pos into default declared txPrev
                if (!txPrev.ReadFromDisk(txindex.pos))
                    return error("ConnectInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString().substr(0, 6).c_str(), prevout.hash.ToString().substr(0, 6).c_str());
            }

            //Make sure the reference index 'n' to txPrev's output is valid and within vSpent range.
            //What is difference between txPrev 'vout' and the txindex 'vSpent'?
            if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
                return error("ConnectInputs() : %s prevout.n out of range %d %d %d", GetHash().ToString().substr(0, 6).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size());

            //S
            // If prev is coinbase, check that it's matured
            //S_E
            //If the previous transaction is a coinbase
            if (txPrev.IsCoinBase())
                //for every block index where we iterate to the previous
                for (CBlockIndex *pindex = pindexBest; pindex && nBestHeight - pindex->nHeight < COINBASE_MATURITY - 1; pindex = pindex->pprev)
                    //if the pindex blockPos is equal to previous transaction's blockPos and pindex's nFile is the same as previous transaction's index's
                    if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                        //return an error
                        return error("ConnectInputs() : tried to spend coinbase at depth %d", nBestHeight - pindex->nHeight);

            //S
            // Verify signature
            //S_E
            //Call verify signature on this transaction's previous transaction. 'i' is vin index. Probably want to make sure vin[i] metadata matches with it's transaction saved in mapWallet
            if (!VerifySignature(txPrev, *this, i))
                return error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString().substr(0, 6).c_str());

            //S
            // Check for conflicts
            //S_E
            //The input transaction's prevout shouldn't have a NON-null spot in vSpent. I guess this is where vSpent is made
            if (!txindex.vSpent[prevout.n].IsNull())
                return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString().substr(0, 6).c_str(), txindex.vSpent[prevout.n].ToString().c_str());

            //S
            // Mark outpoints as spent
            //S_E
            //vSpent is set on the previous transaction index using disk transaction index of this transaction?
            txindex.vSpent[prevout.n] = posThisTx;

            //S
            // Write back
            //S_E
            //Mark the transaction index for source transacion in txdb if fBlock
            if (fBlock)
                txdb.UpdateTxIndex(prevout.hash, txindex);
            //Mark the transaction index in mapTestPool if Miner
            else if (fMiner)
                mapTestPool[prevout.hash] = txindex;

            //local val initialized before loop is increased by previous transaction's value
            nValueIn += txPrev.vout[prevout.n].nValue;
        }

        //S
        // Tally transaction fees
        //S_E
        //Transaction fees are how much all inputs equal together minus the output of the transaction.
        int64 nTxFee = nValueIn - GetValueOut();
        //transaction fee can't be negative
        if (nTxFee < 0)
            return error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString().substr(0, 6).c_str());
        //transaction fee can't be less than minimum
        if (nTxFee < nMinFee)
            return false;
        nFees += nTxFee;
    }

    //What is fBlock?
    if (fBlock)
    {
        //SATOSHI_START
        // Add transaction to disk index
        //SATOSHI_END
        if (!txdb.AddTxIndex(*this, posThisTx, nHeight))
            return error("ConnectInputs() : AddTxPos failed");
    }
    //What is fMiner?
    else if (fMiner)
    {
        //SATOSHI_START
        // Add transaction to test pool
        //SATOSHI_END
        mapTestPool[GetHash()] = CTxIndex(CDiskTxPos(1, 1, 1), vout.size());
    }

    return true;
}

//Just makes sure all the source transactions are valid and add up to the output value of this transaction
bool CTransaction::ClientConnectInputs()
{
    //If it's coinbase, return
    if (IsCoinBase())
        return false;

    //SATOSHI_START
    // Take over previous transactions' spent pointers
    //SATOSHI_END
    CRITICAL_BLOCK(cs_mapTransactions)
    {
        //reuse
        //initialize value in
        int64 nValueIn = 0;
        //iterate through source transactions
        for (int i = 0; i < vin.size(); i++)
        {
            //SATOSHI_START
            // Get prev tx from single transactions in memory
            //SATOSHI_END
            //Get metadata for source transaction output
            COutPoint prevout = vin[i].prevout;
            //If the hash of source transaction isnt isnt in in-memory transaction store
            if (!mapTransactions.count(prevout.hash))
                //return
                return false;

            //Get the previous transaction
            CTransaction &txPrev = mapTransactions[prevout.hash];

            //If the source transaction's output index is greater than the source transactions outputs
            if (prevout.n >= txPrev.vout.size())
                //return
                return false;

            //SATOSHI_START
            // Verify signature
            //SATOSHI_END
            //If the signature for the source transaction isn't valid then error out
            if (!VerifySignature(txPrev, *this, i))
                return error("ConnectInputs() : VerifySignature failed");

            //SATOSHI_START
            ///// this is redundant with the mapNextTx stuff, not sure which I want to get rid of
            ///// this has to go away now that posNext is gone
            // // Check for conflicts
            // if (!txPrev.vout[prevout.n].posNext.IsNull())
            //     return error("ConnectInputs() : prev tx already used");
            //
            // // Flag outpoints as used
            // txPrev.vout[prevout.n].posNext = posThisTx;
            //SATOSHI_END

            //Add up all the source transaction values
            nValueIn += txPrev.vout[prevout.n].nValue;
        }
        //If the value of the current transaction is greater than sum of source
        if (GetValueOut() > nValueIn)
            //return false, otherwise return true below
            return false;
    }

    return true;
}

//Calls disconnectInputs on all block transactions and updates the previous block's next block hash to 0 on the transaction db
bool CBlock::DisconnectBlock(CTxDB &txdb, CBlockIndex *pindex)
{
    //SATOSHI_START
    // Disconnect in reverse order
    //SATOSHI_END
    //for all transactions in block
    for (int i = vtx.size() - 1; i >= 0; i--)
        //call disconnect inputs on them
        if (!vtx[i].DisconnectInputs(txdb))
            //return false if it can disconnect some input
            return false;

    //SATOSHI_START
    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    //SATOSHI_END
    //pindex is this block pointer?
    if (pindex->pprev)
    {
        //Yeah, this gets previous block
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        //sets previous block's next hash to 0
        blockindexPrev.hashNext = 0;
        //and updates previous block
        txdb.WriteBlockIndex(blockindexPrev);
    }

    return true;
}

//Connect all the transactions in block with inputs, attaches previous block to current, and looks for transactions that belong to user
bool CBlock::ConnectBlock(CTxDB &txdb, CBlockIndex *pindex)
{
    //SATOSHI_START
    //// issue here: it doesn't know the version
    //SATOSHI_END

    //The block passed in is summed with it's serialized size plus it's compact size (a function of the size of it's transactions?)
    //This gets some transaction pointer I presume
    unsigned int nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK) - 1 + GetSizeOfCompactSize(vtx.size());

    //A hash map from hash to transaction index is created
    map<uint256, CTxIndex> mapUnused;
    //fees initialized to 0
    int64 nFees = 0;

    //for each transaction in block
    foreach (CTransaction &tx, vtx)
    {
        //It intiializes a disk pointer to the passed in block pointer.
        //What is 'nFile'?
        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        //I guess the transaction pointer is retrieved by calculating the transaction size?
        nTxPos += ::GetSerializeSize(tx, SER_DISK);

        //Use the block tansaction reference to connect inputs
        if (!tx.ConnectInputs(txdb, mapUnused, posThisTx, pindex->nHeight, nFees, true, false))
            return false;
    }

    //If the first transaction of the block (coinbase?) is bigger than the fees
    if (vtx[0].GetValueOut() > GetBlockValue(nFees))
        //return
        return false;

    //SATOSHI_START
    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    //SATOSHI_END

    //Connect previous block to this one?
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        txdb.WriteBlockIndex(blockindexPrev);
    }

    //SATOSHI_START
    // Watch for transactions paying to me
    //SATOSHI_END

    //Self-explanatory
    foreach (CTransaction &tx, vtx)
        AddToWalletIfMine(tx, this);

    return true;
}

//Gets the forking point from pindexBest to pIndexNew, disconnects all the blocks from fork to best, connects all blocks from fork to new, but 'resurrects' blocks from disconnected blocks into memory and deletes transactions from the newly connected blocks.
bool Reorganize(CTxDB &txdb, CBlockIndex *pindexNew)
{
    printf("*** REORGANIZE ***\n");

    //SATOSHI
    // Find the fork
    //S_E

    //Initialize 2 block indexes where one is attached to global var? and the other to param
    CBlockIndex *pfork = pindexBest;
    CBlockIndex *plonger = pindexNew;

    //If global is not equal to param/new block?
    //Maybe this while loop makes sure both pointers point to a blocks that are part of the same chain
    while (pfork != plonger)
    {
        //Move pfork (global) to previous ptr if possible (error if not)
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");

        //While new one has height bigger than pFork (global)
        while (plonger->nHeight > pfork->nHeight)
            //Keep moving the newer one back
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
    }

    //pfork is equal to plonger here, and plonger is no longer used
    //plonger should be equal to shared root?

    //SATOSHI_START
    // List of what to disconnect
    //SAtoshi_END

    //Block index pointer vector
    vector<CBlockIndex *> vDisconnect;

    //Add all block indexes from pindexBest (starts from old) to pfork (which was moved to smaller height)
    //*disconnects all blocks from current tip of chain to forking point
    for (CBlockIndex *pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    //S
    // List of what to connect
    //S_END
    //Vector of indexes
    vector<CBlockIndex *> vConnect;

    //for every prev block from pindexNew
    //*gets all new blocks from forked block
    for (CBlockIndex *pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        //Add to vConnect
        vConnect.push_back(pindex);

    //reverse order of things added
    reverse(vConnect.begin(), vConnect.end());

    //S
    // Disconnect shorter branch
    //S_END

    //Reesurrect is an interesting name for a transaction list
    vector<CTransaction> vResurrect;

    //For each block index of things to disconnect
    foreach (CBlockIndex *pindex, vDisconnect)
    {
        //Make a block assume value of the disconnect block index
        CBlock block;
        if (!block.ReadFromDisk(pindex->nFile, pindex->nBlockPos, true))
            return error("Reorganize() : ReadFromDisk for disconnect failed");

        //disconnect the block from txDb using index
        if (!block.DisconnectBlock(txdb, pindex))
            return error("Reorganize() : DisconnectBlock failed");

        //S
        // Queue memory transactions to resurrect
        //S_END

        //For each of the blocks transactions
        foreach (const CTransaction &tx, block.vtx)
            if (!tx.IsCoinBase())
                //add the transactions in resurrect
                vResurrect.push_back(tx);
    }

    //S
    // Connect longer branch
    //S_END

    //Make list of transactions to delete
    vector<CTransaction> vDelete;

    //For every index in vConnect (which has every block from pindexNew to pfork)
    for (int i = 0; i < vConnect.size(); i++)
    {
        //Get the block ref
        CBlockIndex *pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex->nFile, pindex->nBlockPos, true))
            return error("Reorganize() : ReadFromDisk for connect failed");

        //Connext the block to txDb using its index
        if (!block.ConnectBlock(txdb, pindex))
        {
            //S
            // Invalid block, delete the rest of this branch
            //S_E ***

            //If wasn't able to connect block, aborty
            txdb.TxnAbort();
            //for every block from this block to rest of blocks
            for (int j = i; j < vConnect.size(); j++)
            {
                //erase the block from db
                CBlockIndex *pindex = vConnect[j];
                pindex->EraseBlockFromDisk();
                txdb.EraseBlockIndex(pindex->GetBlockHash());
                mapBlockIndex.erase(pindex->GetBlockHash());
                delete pindex;
            }
            //and return error
            return error("Reorganize() : ConnectBlock failed");
        }

        //S_ST
        // Queue memory transactions to delete
        //S_END

        //For each transaction in block
        foreach (const CTransaction &tx, block.vtx)
            //queue for deletion
            vDelete.push_back(tx);
    }

    //I guess this is picking the new longest chain?
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    //SATOSHI_START
    // Commit now because resurrecting could take some time
    //SATOSHI_END

    //commit whatever is in txdb (whatever that means)
    txdb.TxnCommit();

    //SATOSHI_START
    // Disconnect shorter branch
    //SATOSHI_END

    //for every block in disconnect
    foreach (CBlockIndex *pindex, vDisconnect)
        //if block has a previous
        if (pindex->pprev)
            //disconnect previous block from this one
            pindex->pprev->pnext = NULL;

    //SATOSHI_S
    // Connect longer branch
    //S_E
    //for each in vConnect
    foreach (CBlockIndex *pindex, vConnect)
        //if it has a previous
        if (pindex->pprev)
            //connect the previous to this one (if not forward connected?)
            pindex->pprev->pnext = pindex;

    //S_
    // Resurrect memory transactions that were in the disconnected branch
    //S_E

    //Accept every transaction in vRessurect
    foreach (CTransaction &tx, vResurrect)
        tx.AcceptTransaction(txdb, false);

    //S
    // Delete redundant memory transactions that are in the connected branch
    //S_E

    //Remove every transaction in vDelete
    foreach (CTransaction &tx, vDelete)
        tx.RemoveFromMemoryPool();

    return true;
}

//Add block index to 'mapBlockIndex' and to transaction db (which has block index handling methods). If it's the new longest block then write it as the best chain, making it genesis block if no genesis, and connecting it to previous blocks as necessary. Then commit to db, close db, and relay wallet transactions
bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos)
{
    //S
    // Check for duplicate
    //S_E

    //Gets the hash
    uint256 hash = GetHash();

    //Checks if hash is in in-memory map and errors out if it exists
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString().substr(0, 14).c_str());

    //S
    // Construct new block index object
    //S_E

    //Creates a new block index using parameters
    //What is nFile and nBlockPos?
    CBlockIndex *pindexNew = new CBlockIndex(nFile, nBlockPos, *this);

    //If it couldn't be created, error out
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");

    //Make an iterator that points to where the new block index was inserted into mapBlockIndex
    map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;

    //Have the index hold the reference to hash of this block
    pindexNew->phashBlock = &((*mi).first);

    //Get the previous hash block using hashPrevBlock from this block member
    map<uint256, CBlockIndex *>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);

    //If the previous isn't equal to the end of the mapBlockIndex
    //Although idk how it could since we just inserted current block index
    if (miPrev != mapBlockIndex.end())
    {
        //Set the block index for this block's previous pointer to the previous block index that was found
        pindexNew->pprev = (*miPrev).second;

        //Set the height to the previous's height + 1
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }

    //Create instance of the transaction db
    CTxDB txdb;

    //Begin the transaction?
    txdb.TxnBegin();

    //Writes the blockIndex that was created to memory using the CDiskBlockIndex
    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));

    //S
    // New best
    //S_E

    //If the newly inserted block has the best height
    //What is nBestHeight and where is it taken from?
    if (pindexNew->nHeight > nBestHeight)
    {

        //If the genesis block is null and the hash of this is equal to the genesis block
        if (pindexGenesisBlock == NULL && hash == hashGenesisBlock)
        {
            //Make the genesis block equal to this new block index
            pindexGenesisBlock = pindexNew;
            //Write the hash best chain with this index
            txdb.WriteHashBestChain(hash);
        }

        //If genesis defined and hashPrev is the best chain
        else if (hashPrevBlock == hashBestChain)
        {
            //S
            // Adding to current best branch
            //S_E

            //connect this block with it's new block index entry and write it as best chain
            if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
            {
                //Error handling logic
                txdb.TxnAbort();
                pindexNew->EraseBlockFromDisk();
                mapBlockIndex.erase(pindexNew->GetBlockHash());
                delete pindexNew;
                return error("AddToBlockIndex() : ConnectBlock failed");
            }

            //commit the transaction
            txdb.TxnCommit();
            //set this block index's previous's next to this block index (forward attach)
            pindexNew->pprev->pnext = pindexNew;

            //S
            // Delete redundant memory transactions
            //S_END

            //For each transaction in block, remove it from the memory pool
            foreach (CTransaction &tx, vtx)
                tx.RemoveFromMemoryPool();
        }
        else
        {
            //If this block is not genesis and previous isn't the best chain

            //S
            // New best branch
            //S_E

            //Reorganize the transaction db with this new block index
            if (!Reorganize(txdb, pindexNew))
            {
                txdb.TxnAbort();
                return error("AddToBlockIndex() : Reorganize failed");
            }
        }

        //S
        // New best link
        //S_W

        //Make this block the best chain?
        hashBestChain = hash;
        //Make this index the 'best'
        pindexBest = pindexNew;
        //Make height the best....
        nBestHeight = pindexBest->nHeight;
        //Say there was one more transaction updated
        nTransactionsUpdated++;
        printf("AddToBlockIndex: new best=%s  height=%d\n", hashBestChain.ToString().substr(0, 14).c_str(), nBestHeight);
    }

    //commit the transaction
    txdb.TxnCommit();
    //close the db
    txdb.Close();

    //S
    // Relay wallet transactions that haven't gotten in yet
    //S_END
    //If this block's index is the new best (this block has the best height), then relay wallet transactions
    if (pindexNew == pindexBest)
        RelayWalletTransactions();

    MainFrameRepaint();
    return true;
}

//Checks the block has only one coinbase at beginning, checks blocks aren't empty of transactions (at least coinbase must be there), checks time in block isnt more than 2 hr in future, and checks hashes and merkle trees
bool CBlock::CheckBlock() const
{
    //S
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.
    //S_E

    // Size limits
    //S_E

    //if block is empty or has too many transactions
    if (vtx.empty() || vtx.size() > MAX_SIZE || ::GetSerializeSize(*this, SER_DISK) > MAX_SIZE)
        //error out
        return error("CheckBlock() : size limits failed");

    // Check timestamp
    //S_E

    //If the time of the block is oo far in the future (2 hrs)
    if (nTime > GetAdjustedTime() + 2 * 60 * 60)
        //reject it
        return error("CheckBlock() : block timestamp too far in the future");

    //S
    // First transaction must be coinbase, the rest must not be
    //S_E

    //Checking *again* for emptty block and that the first transaction is a coinbase
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return error("CheckBlock() : first tx is not coinbase");

    //make sure the rest aren't coinbasws
    for (int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return error("CheckBlock() : more than one coinbase");

    // Check transactions
    //S_E

    //call check transaction on each in block
    foreach (const CTransaction &tx, vtx)
        if (!tx.CheckTransaction())
            return error("CheckBlock() : CheckTransaction failed");

    // Check proof of work matches claimed amount
    //S_E

    //make sure bits in transaction are 'lower'/have more 0s than limit
    if (CBigNum().SetCompact(nBits) > bnProofOfWorkLimit)
        return error("CheckBlock() : nBits below minimum work");

    //check computed hash
    if (GetHash() > CBigNum().SetCompact(nBits).getuint256())
        return error("CheckBlock() : hash doesn't match nBits");

    // Check merkleroot
    //S_E
    if (hashMerkleRoot != BuildMerkleTree())
        return error("CheckBlock() : hashMerkleRoot mismatch");

    return true;
}

//makes sure the block isn't earlier than median past time of last block, check the proof of work with regards to last block is correct, writes to disk and block index, relays inventory of hash if hash is equal to hashBestChain
bool CBlock::AcceptBlock()
{
    //SATOSHI_START
    // Check for duplicate
    //SATOSHI_END

    //Get hash of block
    uint256 hash = GetHash();

    //If map block index has the hash
    if (mapBlockIndex.count(hash))
        //Block has already been accepted
        return error("AcceptBlock() : block already in mapBlockIndex");

    //SATOSHI_START
    // Get prev block index
    //SATOSHI_END

    //Get the previous block index using the hash of the previous block on the class property
    map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(hashPrevBlock);

    //If the prev block pointer is not found
    if (mi == mapBlockIndex.end())
        //error out
        return error("AcceptBlock() : prev block not found");

    //Get the block index otherwise
    CBlockIndex *pindexPrev = (*mi).second;

    //SATOSHI_START
    // Check timestamp against prev
    //SATOSHI_END

    // If the blcok time stamp is before the median time past of the previous block
    if (nTime <= pindexPrev->GetMedianTimePast())
        //error out
        return error("AcceptBlock() : block's timestamp is too early");

    //SATOSHI_START
    // Check proof of work
    //SATOSHI_END

    //Make sure the bits of the block is equal to the proof of work required from looking at the previous index (number should have continuous subsequence of 1s at the end)
    if (nBits != GetNextWorkRequired(pindexPrev))
        //error out if the proof of work isn't correct
        return error("AcceptBlock() : incorrect proof of work");

    //SATOSHI_START
    // Write block to history file
    //SATOSHI_END

    //Get a int to represent some file
    unsigned int nFile;

    //get an int to represent the block position
    unsigned int nBlockPos;

    //If unable to write to disk with the File and block pos (with what block-specific info?)
    //WriteToDisk is defined in CBlock
    if (!WriteToDisk(!fClient, nFile, nBlockPos))
        //error out
        return error("AcceptBlock() : WriteToDisk failed");

    //If the block index wasn't able to get added using File and BlockPos
    //AddToBlockIndex is defined in CBlock
    if (!AddToBlockIndex(nFile, nBlockPos))
        //Error out
        return error("AcceptBlock() : AddToBlockIndex failed");

    if (hashBestChain == hash)
        RelayInventory(CInv(MSG_BLOCK, hash));

    // // Add atoms to user reviews for coins created
    // vector<unsigned char> vchPubKey;
    // if (ExtractPubKey(vtx[0].vout[0].scriptPubKey, false, vchPubKey))
    // {
    //     unsigned short nAtom = GetRand(USHRT_MAX - 100) + 100;
    //     vector<unsigned short> vAtoms(1, nAtom);
    //     AddAtomsAndPropagate(Hash(vchPubKey.begin(), vchPubKey.end()), vAtoms, true);
    // }

    return true;
}

//try and accept the block if not already, and add it to orphan blocks if the prev/parent isnt in memory store. After accepted, find any orphans needing this block as parent
bool ProcessBlock(CNode *pfrom, CBlock *pblock)
{
    //S
    // Check for duplicate
    //S_E

    //Satoshi comment sufficient. Error out if the hash of this block can be found in block storages
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString().substr(0, 14).c_str());
    if (mapOrphanBlocks.count(hash))
        return error("ProcessBlock() : already have block (orphan) %s", hash.ToString().substr(0, 14).c_str());

    //S
    // Preliminary checks
    //S_E

    //Checks validity of block
    if (!pblock->CheckBlock())
    {
        delete pblock;
        return error("ProcessBlock() : CheckBlock FAILED");
    }

    //S
    // If don't already have its previous block, shunt it off to holding area until we get it
    //S_E

    //Check previous block is in local storage
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {

        //Try and insert prev into orphan blocks
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().substr(0, 14).c_str());
        mapOrphanBlocks.insert(make_pair(hash, pblock));
        mapOrphanBlocksByPrev.insert(make_pair(pblock->hashPrevBlock, pblock));

        //S
        // Ask this guy to fill in what we're missing
        //S_E

        //Have source transaction send a message?
        if (pfrom)
            pfrom->PushMessage("getblocks", CBlockLocator(pindexBest), GetOrphanRoot(pblock));
        return true;
    }

    //S
    // Store to disk
    //S_E

    //If prev in local storage, accept currently processing block
    if (!pblock->AcceptBlock())
    {
        delete pblock;
        return error("ProcessBlock() : AcceptBlock FAILED");
    }
    delete pblock;

    //S
    // Recursively process any orphan blocks that depended on this one
    //S_E
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);

    //iterating through current block hash
    for (int i = 0; i < vWorkQueue.size(); i++)
    {
        //get hash
        uint256 hashPrev = vWorkQueue[i];

        //for all  orphans with this hash
        for (multimap<uint256, CBlock *>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            //Get the orphan block val
            CBlock *pblockOrphan = (*mi).second;

            //Accept the orphan
            if (pblockOrphan->AcceptBlock())
                //put its hash to queue
                vWorkQueue.push_back(pblockOrphan->GetHash());
            //erase the orphan
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    printf("ProcessBlock: ACCEPTED\n");
    return true;
}

template <typename Stream>
bool ScanMessageStart(Stream &s)
{
    // Scan ahead to the next pchMessageStart, which should normally be immediately
    // at the file pointer.  Leaves file pointer at end of pchMessageStart.
    s.clear(0);
    short prevmask = s.exceptions(0);
    const char *p = BEGIN(pchMessageStart);
    try
    {
        loop
        {
            char c;
            s.read(&c, 1);
            if (s.fail())
            {
                s.clear(0);
                s.exceptions(prevmask);
                return false;
            }
            if (*p != c)
                p = BEGIN(pchMessageStart);
            if (*p == c)
            {
                if (++p == END(pchMessageStart))
                {
                    s.clear(0);
                    s.exceptions(prevmask);
                    return true;
                }
            }
        }
    }
    catch (...)
    {
        s.clear(0);
        s.exceptions(prevmask);
        return false;
    }
}

string GetAppDir()
{
    string strDir;
    if (!strSetDataDir.empty())
    {
        strDir = strSetDataDir;
    }
    else if (getenv("APPDATA"))
    {
        strDir = strprintf("%s\\Bitcoin", getenv("APPDATA"));
    }
    else if (getenv("USERPROFILE"))
    {
        string strAppData = strprintf("%s\\Application Data", getenv("USERPROFILE"));
        static bool fMkdirDone;
        if (!fMkdirDone)
        {
            fMkdirDone = true;
            _mkdir(strAppData.c_str());
        }
        strDir = strprintf("%s\\Bitcoin", strAppData.c_str());
    }
    else
    {
        return ".";
    }
    static bool fMkdirDone;
    if (!fMkdirDone)
    {
        fMkdirDone = true;
        _mkdir(strDir.c_str());
    }
    return strDir;
}

FILE *OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char *pszMode)
{
    if (nFile == -1)
        return NULL;
    FILE *file = fopen(strprintf("%s\\blk%04d.dat", GetAppDir().c_str(), nFile).c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE *AppendBlockFile(unsigned int &nFileRet)
{
    nFileRet = 0;
    loop
    {
        FILE *file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
            return NULL;
        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;
        // FAT32 filesize max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < 0x7F000000 - MAX_SIZE)
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }
        fclose(file);
        nCurrentBlockFile++;
    }
}

bool LoadBlockIndex(bool fAllowNew)
{
    //
    // Load block index
    //
    CTxDB txdb("cr");
    if (!txdb.LoadBlockIndex())
        return false;
    txdb.Close();

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        // Genesis Block:
        // GetHash()      = 0x000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f
        // hashMerkleRoot = 0x4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b
        // txNew.vin[0].scriptSig     = 486604799 4 0x736B6E616220726F662074756F6C69616220646E6F63657320666F206B6E697262206E6F20726F6C6C65636E61684320393030322F6E614A2F33302073656D695420656854
        // txNew.vout[0].nValue       = 5000000000
        // txNew.vout[0].scriptPubKey = 0x5F1DF16B2B704C8A578D0BBAF74D385CDE12C11EE50455F3C438EF4C3FBCF649B6DE611FEAE06279A60939E028A8D65C10B73071A6F16719274855FEB0FD8A6704 OP_CHECKSIG
        // block.nVersion = 1
        // block.nTime    = 1231006505
        // block.nBits    = 0x1d00ffff
        // block.nNonce   = 2083236893
        // CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
        //   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
        //     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
        //     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
        //   vMerkleTree: 4a5e1e

        // Genesis block
        char *pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
        CTransaction txNew;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<unsigned char>((unsigned char *)pszTimestamp, (unsigned char *)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = 50 * COIN;
        txNew.vout[0].scriptPubKey = CScript() << CBigNum("0x5F1DF16B2B704C8A578D0BBAF74D385CDE12C11EE50455F3C438EF4C3FBCF649B6DE611FEAE06279A60939E028A8D65C10B73071A6F16719274855FEB0FD8A6704") << OP_CHECKSIG;
        CBlock block;
        block.vtx.push_back(txNew);
        block.hashPrevBlock = 0;
        block.hashMerkleRoot = block.BuildMerkleTree();
        block.nVersion = 1;
        block.nTime = 1231006505;
        block.nBits = 0x1d00ffff;
        block.nNonce = 2083236893;

        //// debug print, delete this later
        printf("%s\n", block.GetHash().ToString().c_str());
        printf("%s\n", block.hashMerkleRoot.ToString().c_str());
        printf("%s\n", hashGenesisBlock.ToString().c_str());
        txNew.vout[0].scriptPubKey.print();
        block.print();
        assert(block.hashMerkleRoot == uint256("0x4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"));

        assert(block.GetHash() == hashGenesisBlock);

        // Start new block file
        unsigned int nFile;
        unsigned int nBlockPos;
        if (!block.WriteToDisk(!fClient, nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos))
            return error("LoadBlockIndex() : genesis block not accepted");
    }

    return true;
}

void PrintBlockTree()
{
    // precompute tree structure
    map<CBlockIndex *, vector<CBlockIndex *>> mapNext;
    for (map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex *pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex *>> vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex *pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol - 1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
        }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex, true);
        printf("%d (%u,%u) %s  %s  tx %d",
               pindex->nHeight,
               pindex->nFile,
               pindex->nBlockPos,
               block.GetHash().ToString().substr(0, 14).c_str(),
               DateTimeStr(block.nTime).c_str(),
               block.vtx.size());

        CRITICAL_BLOCK(cs_mapWallet)
        {
            if (mapWallet.count(block.vtx[0].GetHash()))
            {
                CWalletTx &wtx = mapWallet[block.vtx[0].GetHash()];
                printf("    mine:  %d  %d  %d", wtx.GetDepthInMainChain(), wtx.GetBlocksToMaturity(), wtx.GetCredit());
            }
        }
        printf("\n");

        // put the main timechain first
        vector<CBlockIndex *> &vNext = mapNext[pindex];
        for (int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol + i, vNext[i]));
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


//Returns true if the hash of the CInv is present in the respective data structure of the type of CInv (ex. in mapTransactions if type is MSG_TX)
bool AlreadyHave(CTxDB &txdb, const CInv &inv)
{
    //return different values depending on type of inventory
    switch (inv.type)
    {
    //If the type is a transaction
    case MSG_TX:
        //return true if the has is in mapTransaction or in transaction db
        return mapTransactions.count(inv.hash) || txdb.ContainsTx(inv.hash);
    //If it's a block
    case MSG_BLOCK:
        //Return if it's in mapBlockIndex or orphan blocks
        return mapBlockIndex.count(inv.hash) || mapOrphanBlocks.count(inv.hash);
    case MSG_REVIEW:
        return true;
    case MSG_PRODUCT:
        return mapProducts.count(inv.hash);
    }
    //SATOSHI_START
    // Don't know what it is, just say we already got one
    //SATOSHI_END
    return true;
}

//Looks for properly formatted messages in the message stream from node being processed and feeds it into ProcessMessage
bool ProcessMessages(CNode *pfrom)
{
    //Get the data stream from node
    CDataStream &vRecv = pfrom->vRecv;
    if (vRecv.empty())
        return true;
    printf("ProcessMessages(%d bytes)\n", vRecv.size());

    //SATOSHI_START
    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (x) data
    //
    //SATOSHI_END

    //Forever loop (which is ok because this is on a separate thread)
    loop
    {
        //SATOSHI_START
        // Scan for message start
        //SATOSHI_END

        //Look for unqiue markers to denote beginning and end of data
        CDataStream::iterator pstart = search(vRecv.begin(), vRecv.end(), BEGIN(pchMessageStart), END(pchMessageStart));

        //If the end of whole message minus the searched position is less than the size of a header message
        if (vRecv.end() - pstart < sizeof(CMessageHeader))
        {
            //And the size of vRecv is bigger than the message header (which it should be because it includes body)
            if (vRecv.size() > sizeof(CMessageHeader))
            {
                //Error out
                printf("\n\nPROCESSMESSAGE MESSAGESTART NOT FOUND\n\n");
                vRecv.erase(vRecv.begin(), vRecv.end() - sizeof(CMessageHeader));
            }
            break;
        }

        //if the start of the searched place minus the beginning is bigger than 0
        if (pstart - vRecv.begin() > 0)
            //Bytes of the message was skipped (or else it would be equal to 0)
            printf("\n\nPROCESSMESSAGE SKIPPED %d BYTES\n\n", pstart - vRecv.begin());
        
        //Erase the unneded parts of message
        vRecv.erase(vRecv.begin(), pstart);

        //SATOSHI_START
        // Read header
        //SATOSHI_END

        //Make a new message header
        CMessageHeader hdr;

        //Put what is in vRecv into header (removing from vRecv since it's a stream)
        vRecv >> hdr;

        //If the header is not valid
        if (!hdr.IsValid())
        {
            //log an error and continue to next iteration of loop
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            continue;
        }

        //Get command from header
        string strCommand = hdr.GetCommand();


        //SATOSHI_START
        // Message size
        //SATOSHI_END

        //Get whole message size
        unsigned int nMessageSize = hdr.nMessageSize;

        //If the message size is bigger than what was read in the buffer
        if (nMessageSize > vRecv.size())
        {

            //SATOSHI_START
            // Rewind and wait for rest of message
            ///// need a mechanism to give up waiting for overlong message size error
            //SATOSHI_END


            printf("MESSAGE-BREAK 2\n");
            //Put it back in the data stream and wait
            vRecv.insert(vRecv.begin(), BEGIN(hdr), END(hdr));
            Sleep(100);
            break;
        }

        //SATOSHI_START
        // Copy message to its own buffer
        //SATOSHI_END

        //Read in whole message into vMsg using header's size metadata
        CDataStream vMsg(vRecv.begin(), vRecv.begin() + nMessageSize, vRecv.nType, vRecv.nVersion);
        
        //Move string past message size?
        vRecv.ignore(nMessageSize);


        //SATOSHI_START
        // Process message
        //SATOSHI_END

        //what is fRet?
        bool fRet = false;
        try
        {
            //Check for shut down
            CheckForShutdown(2);
            //Mutex on main
            CRITICAL_BLOCK(cs_main)
            //Process message using pfrom, the node reference, the comand in the header, and the whole message
            fRet = ProcessMessage(pfrom, strCommand, vMsg);
            //What is checkForShutdown?
            CheckForShutdown(2);
        }
        CATCH_PRINT_EXCEPTION("ProcessMessage()")
        //If not fRet (where fRet means returned successfully?)
        if (!fRet)
            printf("ProcessMessage(%s, %d bytes) from %s to %s FAILED\n", strCommand.c_str(), nMessageSize, pfrom->addr.ToString().c_str(), addrLocalHost.ToString().c_str());
    }

    vRecv.Compact();
    return true;
}

bool ProcessMessage(CNode *pfrom, string strCommand, CDataStream &vRecv)
{
    //int-to-vector of chars that has something to do with keys
    static map<unsigned int, vector<unsigned char>> mapReuseKey;

    printf("received: %-12s (%d bytes)  ", strCommand.c_str(), vRecv.size());

    //for every i (byte) of the data stream size
    for (int i = 0; i < min(vRecv.size(), (unsigned int)25); i++)
        printf("%02x ", vRecv[i] & 0xff);
    printf("\n");

    //if dropMessagesTest is non zero, then see if a random number to it is 0
    if (nDropMessagesTest > 0 && GetRand(nDropMessagesTest) == 0)
    {
        printf("dropmessages DROPPING RECV MESSAGE\n");
        return true;
    }

    //If the command is version
    if (strCommand == "version")
    {
        //SATOSHI_START
        // Can only do this once
        //SATOSHI_END

        //If the node's version isn't equal to 0, return
        //I guess here it should be uninitialized since we do it later on?
        if (pfrom->nVersion != 0)
            return false;

        //declare nTIme
        int64 nTime;

        //Declare address
        CAddress addrMe;

        //vRecv populates each one with version message
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

        //If the version is 0, return false?
        if (pfrom->nVersion == 0)
            return false;

        //Set a version for a sender and receiver?
        pfrom->vSend.SetVersion(min(pfrom->nVersion, VERSION));
        pfrom->vRecv.SetVersion(min(pfrom->nVersion, VERSION));

        //Whether pfrom is a client depends on if 'nServices' masked with the NODE_NETWORK (which is an enum with int value '1'). nServices defaults to 0 so this should pretty much always be true
        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        //if client is true
        if (pfrom->fClient)
        {
            //Doing bitwise or mutation with 'nType'
            pfrom->vSend.nType |= SER_BLOCKHEADERONLY;
            pfrom->vRecv.nType |= SER_BLOCKHEADERONLY;
        }


        //What is AddTimeData?
        //Why does it need the ip?
        AddTimeData(pfrom->addr.ip, nTime);

        //SATOSHI_START
        // Ask the first connected node for block updates
        //SATIOSHI_END

        //bool representing whether blocks asked for? Idk
        static bool fAskedForBlocks;

        //If blocks weren't asked for (which should be the case in the first run-through) and if the node isn't a client?
        if (!fAskedForBlocks && !pfrom->fClient)
        {
            //Asked for blocks is true if didn't ask for blocks and the source node isn't a client?
            fAskedForBlocks = true;

            //Give message to source node saying to ask for blocks
            //Also using pindexBest (best block of longest chain?)
            pfrom->PushMessage("getblocks", CBlockLocator(pindexBest), uint256(0));
        }

        printf("version addrMe = %s\n", addrMe.ToString().c_str());
    }

    //If the version of source node is 0
    else if (pfrom->nVersion == 0)
    {
        //SATOSHI_START
        // Must have a version message before anything else
        //SATOSHI_END

        //return because I guess version should be non-zero to issue commands?
        return false;
    }

    //If command is 'addr' then look through vector of addresses represented in the message and add each one to address db. Add it to 'setAddrKnown' for source node and add it to other known node's list of vAddrToSend if not on their 'setAddrKnown'
    else if (strCommand == "addr")
    {

        //Initialize vector of address
        vector<CAddress> vAddr;

        //Pipe message into address vector
        vRecv >> vAddr;

        //SATOSHI_START
        // Store the new addresses
        //SATOSHI_END

        //Declare local address db
        CAddrDB addrdb;

        //For each address received in message
        foreach (const CAddress &addr, vAddr)
        {
            //if shutdown, return early
            if (fShutdown)
                return true;

            //If the address from vAddr was able to get added successfully
            if (AddAddress(addrdb, addr))
            {
                //SATOSHI_START
                // Put on lists to send to other nodes
                //SATOSHI_END

                //Add the address to the node's setAddrKnown
                pfrom->setAddrKnown.insert(addr);
                CRITICAL_BLOCK(cs_vNodes)

                //For each known node
                foreach (CNode *pnode, vNodes)
                    //If the node doesn't know about the address just sent to us
                    if (!pnode->setAddrKnown.count(addr))
                        //Add it to that nodes addresses
                        pnode->vAddrToSend.push_back(addr);
            }
        }
    }

    //if the command was 'inv' then see if inventory hash exists in the data store corresponding to the inventory type. Ask for the inventory from source node if not present in data store, and try and get blocks for orphan blocks if not
    else if (strCommand == "inv")
    {

        //Declare a Vector of CInv
        //What is CInv??
        //I looked at the class and CInv has 'type' and 'hash' as public properties..So I still have no clue what this is...
        vector<CInv> vInv;

        //Read into vector from message
        vRecv >> vInv;

        //Open transaction db in read mode
        CTxDB txdb("r");

        //For each Cinv in vector read from message
        foreach (const CInv &inv, vInv)
        {
            //Shutdown seems like short circuit for all commands
            if (fShutdown)
                return true;

            //Ahh, I guess CInv stands for Inventory?
            //Add 
            pfrom->AddInventoryKnown(inv);

            //check to see if already have inventory in one of data stores (mainly looking through transactions, blocks)
            bool fAlreadyHave = AlreadyHave(txdb, inv);
            printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            //If I don't already have
            if (!fAlreadyHave)
                //Ask for the inventory from node being processed
                pfrom->AskFor(inv);
            //If AlreadyHave is true and inventory is in our orphan block
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash))
                //Try and send a message to get blocks from the orphan block's root (probably for goal of reorganizing chains)
                pfrom->PushMessage("getblocks", CBlockLocator(pindexBest), GetOrphanRoot(mapOrphanBlocks[inv.hash]));
        }
    }

    //If the command is 'getdata', read in inventory vector and for each inventory, if it's a block, send block to source node and if it's another known type send the source node data from mapRelay ( a map that takes inventory as key)
    else if (strCommand == "getdata")
    {
        //Read in a vector inventory
        vector<CInv> vInv;
        vRecv >> vInv;

        //for each inventory in vector
        foreach (const CInv &inv, vInv)
        {
            //shotdown if shutdown is true
            if (fShutdown)
                return true;
            printf("received getdata for: %s\n", inv.ToString().c_str());

            //if the inventory type is a block
            if (inv.type == MSG_BLOCK)
            {
                //SATOSHI_START
                // Send block from disk
                //SATOSHI_END

                //Try and get block from local structure
                map<uint256, CBlockIndex *>::iterator mi = mapBlockIndex.find(inv.hash);

                //if a block was found
                if (mi != mapBlockIndex.end())
                {
                    //SATOSHI_START
                    //// could optimize this to send header straight from blockindex for client
                    //SATOSHI_END

                    //Declare a block
                    CBlock block;

                    //Load block from index using knowledge of whether source node is a client
                    block.ReadFromDisk((*mi).second, !pfrom->fClient);

                    //Sned message 'block' to source node?
                    pfrom->PushMessage("block", block);
                }
            }
            //If it's a recognizable type (since AlreadyHave shows that only a few types are defined for type inv)
            else if (inv.IsKnownType())
            {
                //S
                // Send stream from relay memory
                //S_E
                //lock mapRelay
                CRITICAL_BLOCK(cs_mapRelay)
                {
                    //Try to find the inventory in mapRelay
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    //If it;s found
                    if (mi != mapRelay.end())
                        //Send the command of the inventory and the associated data to source node
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                }
            }
        }
    }

    //if the command is 'getblocks'
    //queues blocks to send to requesting node
    else if (strCommand == "getblocks")
    {
        //declare a block locator
        CBlockLocator locator;

        //hashStop is the hash of the transaction you want to retrieve from beginning
        uint256 hashStop;

        //Use vRecv CDataStream to desrialize locator, then use vRecv buffer to deserialize into hashStop
        vRecv >> locator >> hashStop;

        //SATOSHI_START
        // Find the first block the caller has in the main chain
        //SATOSHI_END
        CBlockIndex *pindex = locator.GetBlockIndex();

        //SATOSHI_START
        // Send the rest of the chain
        //SATOSHI_END

        //If the first block of mainchain was found
        if (pindex)
            //Get the block after it
            pindex = pindex->pnext;

        //log the current block number and where the stopping point is
        printf("getblocks %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0, 14).c_str());

        //for every block to the end
        for (; pindex; pindex = pindex->pnext)
        {
            //If pindex's block hash is equal to the stopping hash
            if (pindex->GetBlockHash() == hashStop)
            {
                //log
                printf("  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString().substr(0, 14).c_str());

                //break out of the loop
                break;
            }

            //SATOSHI_START
            // Bypass setInventoryKnown in case an inventory message got lost
            //SATOSHI_END

            //lock the inventory of the node requesting data
            CRITICAL_BLOCK(pfrom->cs_inventory)
            {
                //inventory is made as a block message (indicated as MSG_BLOCK) and the block hash is atached to inventory as well
                CInv inv(MSG_BLOCK, pindex->GetBlockHash());

                //SATOSHI_START
                // returns true if wasn't already contained in the set
                //SATOSHI_END

                //If the inventory is NOT already in the known2 set
                if (pfrom->setInventoryKnown2.insert(inv).second)
                {
                    //erase the inventory from the known inventory
                    pfrom->setInventoryKnown.erase(inv);

                    //push the inventory to send
                    pfrom->vInventoryToSend.push_back(inv);
                }
            }
        }
    }

    //if the command is a transaction
    //So if a transaction is being relayed
    else if (strCommand == "tx")
    {
        //declare work queue vector. It seems like the work queue is supposed to represent all the newly accepted transactions that need to be checked as a dependency for orphan blocks
        vector<uint256> vWorkQueue;

        //Wrap the vRecv CDataStream in another CDataStream. Probably to make copy of data stream.
        CDataStream vMsg(vRecv);

        //Declare transaction
        CTransaction tx;

        //Read in transaction from vRecv data stream
        vRecv >> tx;

        //Create the inventory as a transaction message
        CInv inv(MSG_TX, tx.GetHash());

        //Add inventory to known in the receiving node
        pfrom->AddInventoryKnown(inv);

        //declare fMissingInputs to be populated by tx.AcceptTransaction
        bool fMissingInputs = false;

        //if the transaction is accepted
        if (tx.AcceptTransaction(true, &fMissingInputs))
        {
            //Adds the transaction to mapWallet and writes to db if ours
            AddToWalletIfMine(tx, NULL);

            //Send transaction to other nodes with CDataStream copy
            RelayMessage(inv, vMsg);

            //Erase the inventory from mapAlreadyAskedFor if it was in there (because we just go it?)
            mapAlreadyAskedFor.erase(inv);

            //Add the inventory hash to work queue
            vWorkQueue.push_back(inv.hash);


            //Satoshi comment pretty descriptive here
            //SATOSHI_START
            // Recursively process any orphan transactions that depended on this one
            //SATOSHI_END

            //Look through all elements of work queue
            for (int i = 0; i < vWorkQueue.size(); i++)
            {
                //Get inventory hash from iterating index of workQueue
                uint256 hashPrev = vWorkQueue[i];

                //for every multimap in mapOrphanTransactionsByPrev where the iterator isn't equal to the map at hashprev
                for (multimap<uint256, CDataStream *>::iterator mi = mapOrphanTransactionsByPrev.lower_bound(hashPrev);
                     mi != mapOrphanTransactionsByPrev.upper_bound(hashPrev);
                     ++mi)
                {
                    //Retrieve the data strea,
                    const CDataStream &vMsg = *((*mi).second);

                    //Declare a transaction
                    CTransaction tx;

                    //Read the data stream into transaction
                    CDataStream(vMsg) >> tx;

                    //Create a new inventory of the transaction
                    CInv inv(MSG_TX, tx.GetHash());

                    //Try to accept the transaction
                    if (tx.AcceptTransaction(true))
                    {
                        printf("   accepted orphan tx %s\n", inv.hash.ToString().substr(0, 6).c_str());
                        //Add to wallet if transaction is ours
                        AddToWalletIfMine(tx, NULL);

                        //Relay the message to other nodes
                        RelayMessage(inv, vMsg);

                        //Erase the inventory from the inventory we've queued to ask for
                        mapAlreadyAskedFor.erase(inv);

                        //push theis inventory into the work queue
                        vWorkQueue.push_back(inv.hash);
                    }
                }
            }

            //Delete every transaction from workQueue in orphan store (since workQueue represents all newly accepted transactions)
            foreach (uint256 hash, vWorkQueue)
                EraseOrphanTx(hash);
        }
        //If the transaction received pointed to some input transactions we don't have
        else if (fMissingInputs)
        {
            //log
            printf("storing orphan tx %s\n", inv.hash.ToString().substr(0, 6).c_str());
            //Store the transaction as an orphan
            AddOrphanTx(vMsg);
        }
    }

    else if (strCommand == "review")
    {
        CDataStream vMsg(vRecv);
        CReview review;
        vRecv >> review;

        CInv inv(MSG_REVIEW, review.GetHash());
        pfrom->AddInventoryKnown(inv);

        if (review.AcceptReview())
        {
            // Relay the original message as-is in case it's a higher version than we know how to parse
            RelayMessage(inv, vMsg);
            mapAlreadyAskedFor.erase(inv);
        }
    }

    else if (strCommand == "block")
    {
        auto_ptr<CBlock> pblock(new CBlock);
        vRecv >> *pblock;

        //// debug print
        printf("received block:\n");
        pblock->print();

        CInv inv(MSG_BLOCK, pblock->GetHash());
        pfrom->AddInventoryKnown(inv);

        if (ProcessBlock(pfrom, pblock.release()))
            mapAlreadyAskedFor.erase(inv);
    }

    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        //// need to expand the time range if not enough found
        int64 nSince = GetAdjustedTime() - 60 * 60; // in the last hour
        CRITICAL_BLOCK(cs_mapAddresses)
        {
            foreach (const PAIRTYPE(vector<unsigned char>, CAddress) & item, mapAddresses)
            {
                if (fShutdown)
                    return true;
                const CAddress &addr = item.second;
                if (addr.nTime > nSince)
                    pfrom->vAddrToSend.push_back(addr);
            }
        }
    }

    else if (strCommand == "checkorder")
    {
        uint256 hashReply;
        CWalletTx order;
        vRecv >> hashReply >> order;

        /// we have a chance to check the order here

        // Keep giving the same key to the same ip until they use it
        if (!mapReuseKey.count(pfrom->addr.ip))
            mapReuseKey[pfrom->addr.ip] = GenerateNewKey();

        // Send back approval of order and pubkey to use
        CScript scriptPubKey;
        scriptPubKey << mapReuseKey[pfrom->addr.ip] << OP_CHECKSIG;
        pfrom->PushMessage("reply", hashReply, (int)0, scriptPubKey);
    }

    else if (strCommand == "submitorder")
    {
        uint256 hashReply;
        CWalletTx wtxNew;
        vRecv >> hashReply >> wtxNew;

        // Broadcast
        if (!wtxNew.AcceptWalletTransaction())
        {
            pfrom->PushMessage("reply", hashReply, (int)1);
            return error("submitorder AcceptWalletTransaction() failed, returning error 1");
        }
        wtxNew.fTimeReceivedIsTxTime = true;
        AddToWallet(wtxNew);
        wtxNew.RelayWalletTransaction();
        mapReuseKey.erase(pfrom->addr.ip);

        // Send back confirmation
        pfrom->PushMessage("reply", hashReply, (int)0);
    }

    else if (strCommand == "reply")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        CRequestTracker tracker;
        CRITICAL_BLOCK(pfrom->cs_mapRequests)
        {
            map<uint256, CRequestTracker>::iterator mi = pfrom->mapRequests.find(hashReply);
            if (mi != pfrom->mapRequests.end())
            {
                tracker = (*mi).second;
                pfrom->mapRequests.erase(mi);
            }
        }
        if (!tracker.IsNull())
            tracker.fn(tracker.param1, vRecv);
    }

    else
    {
        // Ignore unknown commands for extensibility
        printf("ProcessMessage(%s) : Ignored unknown message\n", strCommand.c_str());
    }

    if (!vRecv.empty())
        printf("ProcessMessage(%s) : %d extra bytes\n", strCommand.c_str(), vRecv.size());

    return true;
}

//Look through what we have queued to send on other nodes and extract that into vInventoryToSend (populated in RelayWalletTransactions and similar functions). Then actually send the message to the other nodes. This method is run on node startup and runs within an endless loop.
bool SendMessages(CNode *pto)
{
    //Check for shutdown
    CheckForShutdown(2);
    CRITICAL_BLOCK(cs_main)
    {
        //SATOSHI_START
        // Don't send anything until we get their version message
        //SATOSHI_END

        //If version of recipient node is 0, bail out
        if (pto->nVersion == 0)
            return true;

        //SATOSHI_START
        //
        // Message: addr
        //
        //SATOSHI_END

        //Declare vector of addresses
        vector<CAddress> vAddrToSend;

        //Reverse the vector of addresses
        vAddrToSend.reserve(pto->vAddrToSend.size());

        //for each address in recipient node addresses
        foreach (const CAddress &addr, pto->vAddrToSend)
            //If the address is NOT in the recipient node's setAddrKnown
            if (!pto->setAddrKnown.count(addr))
                //Add the address to our own node's vAddrToSend
                vAddrToSend.push_back(addr);

        //Clear recipient node's vAddrToSend
        pto->vAddrToSend.clear();

        //If our addrToSend is not empty
        if (!vAddrToSend.empty())
            //Send a "addr" message using the vAddrToSend
            pto->PushMessage("addr", vAddrToSend);

        //SATOSHI_START
        //
        // Message: inventory
        //
        //SATOSHI_END

        //Declare inventory vector
        vector<CInv> vInventoryToSend;

        //lock recipient node inventory
        CRITICAL_BLOCK(pto->cs_inventory)
        {
            //Reserve inventory size of our own according to recipient node
            vInventoryToSend.reserve(pto->vInventoryToSend.size());

            //For each inventory on recipient
            foreach (const CInv &inv, pto->vInventoryToSend)
            {
                //SATOSHI_START
                // returns true if wasn't already contained in the set
                //SATOSHI_END

                //insert the inventory in the 'setInventoryKnown' for recipient 
                if (pto->setInventoryKnown.insert(inv).second)
                    //and push into our inventoryTOSend if newly inserted into set
                    vInventoryToSend.push_back(inv);
            }

            //Clear recipeint inventory
            pto->vInventoryToSend.clear();

            //clear recipient inventory set 2
            pto->setInventoryKnown2.clear();
        }

        //If the inventory to send for us is not empty
        if (!vInventoryToSend.empty())
            //Push inventory
            pto->PushMessage("inv", vInventoryToSend);

        //SATOSHI_START
        //
        // Message: getdata
        //
        //SATOSHI_END

        //Vector of inventoryy to ask for
        vector<CInv> vAskFor;

        //Get current time
        int64 nNow = GetTime() * 1000000;

        //initialize transaction db
        CTxDB txdb("r");

        //While thr recipient reference still has things in its mapAskFor, and the things in the map are timestamped in the past
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            //Get the inventory element
            const CInv &inv = (*pto->mapAskFor.begin()).second;

            //Log
            printf("sending getdata: %s\n", inv.ToString().c_str());

            //If the inventory does not exist in transaction db
            if (!AlreadyHave(txdb, inv))
                //Add inventory to askFor vector
                vAskFor.push_back(inv);

            //erase the element from recipient mapAskFor
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }

        //If what we have to ask for is not empty
        if (!vAskFor.empty())
            //send message asking for data
            pto->PushMessage("getdata", vAskFor);
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

int FormatHashBlocks(void *pbuffer, unsigned int len)
{
    unsigned char *pdata = (unsigned char *)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char *pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

using CryptoPP::ByteReverse;
static int detectlittleendian = 1;

///If detectlittleendian is dereferenced as a char and is not equal to 0, some compensation happnes
//Otherwise 2 byte chunks are taken out of the input and put into state that keeps getting input into hash
void BlockSHA256(const void *pin, unsigned int nBlocks, void *pout)
{
    unsigned int *pinput = (unsigned int *)pin;
    unsigned int *pstate = (unsigned int *)pout;

    CryptoPP::SHA256::InitState(pstate);

    if (*(char *)&detectlittleendian != 0)
    {
        for (int n = 0; n < nBlocks; n++)
        {
            unsigned int pbuf[16];
            for (int i = 0; i < 16; i++)
                pbuf[i] = ByteReverse(pinput[n * 16 + i]);
            CryptoPP::SHA256::Transform(pstate, pbuf);
        }
        for (int i = 0; i < 8; i++)
            pstate[i] = ByteReverse(pstate[i]);
    }
    else
    {
        for (int n = 0; n < nBlocks; n++)
            CryptoPP::SHA256::Transform(pstate, pinput + n * 16);
    }
}

//Creates a new coinbase transaction for block, looks through mapWallet for non-coinbase and non-finalized transactions, uses BlockSHA256 to do proof of work, asks the block for value to add to coinbase block, and temp block has its time set
bool BitcoinMiner()
{
    //Print the message and start the thread
    printf("BitcoinMiner started\n");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    //Declares a key and initializes it
    CKey key;
    key.MakeNewKey();

    //Creates a BigNum to hold the nonce
    CBigNum bnExtraNonce = 0;

    //While fGenerateBitcoins is true
    while (fGenerateBitcoins)
    {
        //Sleep every 50 ms?
        Sleep(50);

        //What is CheckForShutdown?
        CheckForShutdown(3);

        //What is vNodes?
        //If it's empty
        while (vNodes.empty())
        {
            //sleep for 1 second and check for shutdown
            Sleep(1000);
            CheckForShutdown(3);
        }

        //Get when the transaction was last updated
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;

        //Get the block pointer to pindexBest (newest block?)
        CBlockIndex *pindexPrev = pindexBest;

        //Get work required for the block retrieved
        unsigned int nBits = GetNextWorkRequired(pindexPrev);

        //SATOSHI_START
        //
        // Create coinbase tx
        //
        //SATOSHI_END

        //Create a new transaction
        CTransaction txNew;

        //Make the input transaction have 1 slot
        txNew.vin.resize(1);

        //Set the reference transaction to null (making coinbase)
        txNew.vin[0].prevout.SetNull();

        //put mix nonce, nBits, and script somehow?
        txNew.vin[0].scriptSig << nBits << ++bnExtraNonce;

        //Resize the output transaction to 1 (just payout, no refund)
        txNew.vout.resize(1);
        //Mix in some stuff again
        txNew.vout[0].scriptPubKey << key.GetPubKey() << OP_CHECKSIG;

        //SATOSHI_START
        //
        // Create new block
        //
        //SATOSHI_END

        //Make a pointer to a new block
        auto_ptr<CBlock> pblock(new CBlock());

        //Not sure what this does; make sure the new block that the pointer points at can be retrieved?
        if (!pblock.get())
            return false;

        //SATOSHI_START
        // Add our coinbase tx as first transaction
        //SATOSHI_END

        //Add new coinbase transaction to block
        pblock->vtx.push_back(txNew);

        //SATOSHI_START
        // Collect the latest transactions into the block
        //SATOSHI_END

        //Make no fees?
        int64 nFees = 0;

        //Lock cs_main and mapTransactions
        //What is 'locking cs_main'?
        CRITICAL_BLOCK(cs_main)
        CRITICAL_BLOCK(cs_mapTransactions)
        {
            //Create a transaction db in read mode
            CTxDB txdb("r");

            //Create a map of hash-to-transactions
            map<uint256, CTxIndex> mapTestPool;

            //make a vector to check if 'vf' already added? Same size as transactions
            vector<char> vfAlreadyAdded(mapTransactions.size());

            //Looks like 'fFoundSomething' is loop control that initializes to true. Wonder if there's disdain for do-while by the devs of this code base?
            bool fFoundSomething = true;

            //make block size 0
            unsigned int nBlockSize = 0;

            //While foundSomething is true and block size is less than half the max_size (of block?)
            //Could've used break to get out of loop but used continue instead...
            while (fFoundSomething && nBlockSize < MAX_SIZE / 2)
            {
                //found something always starts as false
                fFoundSomething = false;

                //n is 0
                unsigned int n = 0;

                //for every transaction in mapTransactions (with n as count)
                for (map<uint256, CTransaction>::iterator mi = mapTransactions.begin(); mi != mapTransactions.end(); ++mi, ++n)
                {

                    //If index already added
                    if (vfAlreadyAdded[n])
                        //then continue
                        continue;

                    //otherwise, get the transaction
                    CTransaction &tx = (*mi).second;

                    //If the transaction is coinbase or isFinal
                    //What is 'isFinal'?
                    //If the transaction has an 'nSequence' property that's equal to the unsigned integer max
                    if (tx.IsCoinBase() || !tx.IsFinal())
                        //skip
                        continue;

                    //SATOSHI_START
                    // Transaction fee requirements, mainly only needed for flood control
                    // Under 10K (about 80 inputs) is free for first 100 transactions
                    // Base rate is 0.01 per KB
                    //SATOSHI_END

                    //Get the minimum fee from current transaction
                    int64 nMinFee = tx.GetMinFee(pblock->vtx.size() < 100);

                    //create temporary mapTestPool
                    map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);

                    //Connect inputs of transaction to db, testPool, and disk (wih fees and whatnot)
                    if (!tx.ConnectInputs(txdb, mapTestPoolTmp, CDiskTxPos(1, 1, 1), 0, nFees, false, true, nMinFee))
                        //continue if not able to
                        continue;

                    //Idk why mapTestPool needs to get swapped with temp test pool
                    swap(mapTestPool, mapTestPoolTmp);

                    //Add the transaction to block
                    pblock->vtx.push_back(tx);
                    //Add the transaction to block size
                    nBlockSize += ::GetSerializeSize(tx, SER_NETWORK);

                    //Set the index to already added
                    vfAlreadyAdded[n] = true;
                    //and set foundSomething to true
                    fFoundSomething = true;
                }
            }
        }
        //This next part happens whether foundSOmething is true or false

        //what is nbits?
        //I think the difficulty
        pblock->nBits = nBits;

        //pblock has its output value as the method 'GetBlockValue' with fees
        pblock->vtx[0].vout[0].nValue = pblock->GetBlockValue(nFees);

        //log
        printf("\n\nRunning BitcoinMiner with %d transactions in block\n", pblock->vtx.size());

        //S
        //
        // Prebuild hash buffer
        //
        //S_E

        //making block structure
        struct unnamed1
        {
            struct unnamed2
            {
                int nVersion;
                uint256 hashPrevBlock;
                uint256 hashMerkleRoot;
                unsigned int nTime;
                unsigned int nBits;
                unsigned int nNonce;
            } block;
            unsigned char pchPadding0[64];
            uint256 hash1;
            unsigned char pchPadding1[64];
        } tmp;

        //Iniialize block version in tmp to nVersion on pblock where pblock is the new transaction
        tmp.block.nVersion = pblock->nVersion;

        //initialize hash from prev block to pIndexPrev's hash
        tmp.block.hashPrevBlock = pblock->hashPrevBlock = (pindexPrev ? pindexPrev->GetBlockHash() : 0);

        //have pblock build a merkle tree to set the root on its own member and the tmp block's
        tmp.block.hashMerkleRoot = pblock->hashMerkleRoot = pblock->BuildMerkleTree();

        //Have the nTime set to max between previous block's median past time or the adjusted current time
        tmp.block.nTime = pblock->nTime = max((pindexPrev ? pindexPrev->GetMedianTimePast() + 1 : 0), GetAdjustedTime());

        //nBits is set to whatever it was before
        tmp.block.nBits = pblock->nBits = nBits;

        //Nonce is set to 1
        tmp.block.nNonce = pblock->nNonce = 1;

        //Idk what these 2 are but I guess first one is in made of temp block data and the other one is nt made from tmp block's hash
        unsigned int nBlocks0 = FormatHashBlocks(&tmp.block, sizeof(tmp.block));
        unsigned int nBlocks1 = FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

        //S_STAR
        //
        // Search
        //
        //S_END

        //Get start time
        unsigned int nStart = GetTime();

        //Get difficulty
        //The value of nBits was ultimately set to GetNextWorkRequired which took the previous block as param
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        //declare hash
        uint256 hash;

        //For endless while loop (that's how macro is defined)
        loop
        {
            //Create 2 SHA256 blocks of blocks seen before
            BlockSHA256(&tmp.block, nBlocks0, &tmp.hash1);

            //Put nBlocks1 hash into &hash??
            BlockSHA256(&tmp.hash1, nBlocks1, &hash);

            //Idk what it means for hash to be less than hashTarget but it makes sense if it's equal
            //hashTarget is made from new block's nBits
            //I've been thinking hashTarget represents the number of 0s but this inequality makes sense if I think of it as the number of non-zeroes I guess..
            //So I could say it has to be less than : 00001111111 (the root hashTarget) which means I need more 0s
            if (hash <= hashTarget)
            {

                //if it is, make the new block equal the tmp block nonce
                pblock->nNonce = tmp.block.nNonce;

                //If proof of work is met?
                //The tmp block seems to be used as the working block needed to test the nonce
                assert(hash == pblock->GetHash());

                //S
                //// debug print
                //S_E
                printf("BitcoinMiner:\n");
                printf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex().c_str(), hashTarget.GetHex().c_str());
                pblock->print();

                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                CRITICAL_BLOCK(cs_main)
                {
                    // Save key
                    if (!AddKey(key))
                        return false;
                    key.MakeNewKey();

                    // Process this block the same as if we had received it from another node
                    if (!ProcessBlock(NULL, pblock.release()))
                        printf("ERROR in BitcoinMiner, ProcessBlock, block not accepted\n");
                }
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

                //Sleep is done to not make this go crazy
                Sleep(500);
                break;
            }

            //S_
            // Update nTime every few seconds
            //S_END

            //if tmp block nonce masked with all 1s and a 0 equals 0
            //This  part iterates the nonce
            if ((++tmp.block.nNonce & 0x3ffff) == 0)
            {
                //try to shutdown?
                CheckForShutdown(3);

                //tmp block nonce is checked for 0
                if (tmp.block.nNonce == 0)
                    break;

                //prev can't be best?
                if (pindexPrev != pindexBest)
                    break;

                //updates transactions isnt equal to nTransactionsUpdatedLast and the elapsed time (for 'proof of work'?) must be under 60 ms or seconds, idk
                if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    //if time is broken, break
                    break;

                //If no generate bitcoins global
                if (!fGenerateBitcoins)
                    //break
                    break;

                //The time of the block is equal to MediantTimePast or adjusted time
                tmp.block.nTime = pblock->nTime = max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());
            }
        }
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Actions
//

//Adds up all unspent wallet transaction values in mapWallet
int64 GetBalance()
{
    int64 nStart, nEnd;
    QueryPerformanceCounter((LARGE_INTEGER *)&nStart);

    int64 nTotal = 0;
    CRITICAL_BLOCK(cs_mapWallet)
    {
        for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            CWalletTx *pcoin = &(*it).second;
            if (!pcoin->IsFinal() || pcoin->fSpent)
                continue;
            nTotal += pcoin->GetCredit();
        }
    }

    QueryPerformanceCounter((LARGE_INTEGER *)&nEnd);
    ///printf(" GetBalance() time = %16I64d\n", nEnd - nStart);
    return nTotal;
}


//Find subset of transactions in mapWallet where the sum of their outputs most closely matches ntargetValue
bool SelectCoins(int64 nTargetValue, set<CWalletTx *> &setCoinsRet)
{
    //clear this setCoinRet set in the param
    setCoinsRet.clear();

    //SATOSHI_START
    // List of values less than target
    //SATOSHI_END

    //Declare nLowestLarger as the max 64 bit integer
    int64 nLowestLarger = _I64_MAX;

    //Initialize wallet transaction to null
    CWalletTx *pcoinLowestLarger = NULL;

    //Make a vector of int to transaction as vValue
    vector<pair<int64, CWalletTx *>> vValue;

    //Make nTotalLower = 0
    int64 nTotalLower = 0;

    //lock mapWallet vector
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //For every hash-transaction pair in mapWallet
        for (map<uint256, CWalletTx>::iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            //Get the transaction
            CWalletTx *pcoin = &(*it).second;

            //See if the transaction isFinal or it's spent because we want to continue in that case
            if (!pcoin->IsFinal() || pcoin->fSpent)
                continue;

            //Get the unspent transaction output for the transaction in our wallet
            int64 n = pcoin->GetCredit();

            //If the amount is negative somehow, continue
            if (n <= 0)
                continue;

            //If the amount is less than the target value in the parameter
            if (n < nTargetValue)
            {
                //Add the amount and hash to the vValue vector
                vValue.push_back(make_pair(n, pcoin));

                //Add the amount to nTotalLowe
                nTotalLower += n;
            }

            //If the amount of the coin is equal to the targetValue, then put the transaction into the output coin set and return
            else if (n == nTargetValue)
            {
                setCoinsRet.insert(pcoin);
                return true;
            }

            //If the amount is greater than the targetValue but less than the largest number of coins
            else if (n < nLowestLarger)
            {
                //Make the largest number of coins equal n and
                nLowestLarger = n;
                //make pcoinLowestLarger take the place of the lowest valued transaction that's larger than the target
                pcoinLowestLarger = pcoin;
            }
        }
    }

    //If all the coins that were lower than the target value were summed up and still didn't hit target
    if (nTotalLower < nTargetValue)
    {
        //return false if we have no other coins
        if (pcoinLowestLarger == NULL)
            return false;
        
        //Just put one coin that's larger than the target in the output transaction set
        setCoinsRet.insert(pcoinLowestLarger);
        //and return
        return true;
    }

    //SATOSHI_START
    // Solve subset sum by stochastic approximation
    //SATOSHI_END

    //Sort the value of coins smaller than target
    sort(vValue.rbegin(), vValue.rend());

    //A vector that will indicate whether the index in vVector with the same position i was chosen in that iteration of subset search below
    vector<char> vfIncluded;

    //A vector that is a copy of the best subset of transactions that most closely match the target value
    vector<char> vfBest(vValue.size(), true);

    //Set the best variable equal to the sum of utxos (unspent outputs) lower than target
    int64 nBest = nTotalLower;


    //this is subset finding to see which set of small coins can closest match our desired values
    //For 1000 iterations where the best variable isn't equal to the target
    for (int nRep = 0; nRep < 1000 && nBest != nTargetValue; nRep++)
    {
        //Set the included vector to equal the size of small transaction array and set ever index value to false
        vfIncluded.assign(vValue.size(), false);

        //initialize the value total for this iteration to 0
        int64 nTotal = 0;

        //Set reached target to false
        bool fReachedTarget = false;

        //For 2 passes (where the value doesn't reach the target)
        //So we're trying to add each transaction twice
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            //For each small transaction
            for (int i = 0; i < vValue.size(); i++)
            {
                //if this is the first pass then make this iteration have a .5 chance of happening, and if it's the second time then run if there's nothing inserted for the i'th place of vIncluded
                if (nPass == 0 ? rand() % 2 : !vfIncluded[i])
                {
                    //add the value to the total
                    nTotal += vValue[i].first;

                    //Set included to true for the index
                    vfIncluded[i] = true;

                    //if the total is greater than or equal to the target at this point
                    if (nTotal >= nTargetValue)
                    {
                        //set reachedTarget to true
                        fReachedTarget = true;

                        //if the the nTotal is less than the best (the subset sum of small transactions is less than the sum all small transactions)
                        if (nTotal < nBest)
                        {
                            //set the best equal to the total because it's closer to target
                            nBest = nTotal;

                            //Set the best vector pointer to the included vector
                            vfBest = vfIncluded;
                        }

                        //subtract the amount just added from total
                        nTotal -= vValue[i].first;

                        //and set included to false
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }

    //SATOSHI_START
    // If the next larger is still closer, return it
    //SATOSHI_END
    //Same as what Satoshi is saying above^
    if (pcoinLowestLarger && nLowestLarger - nTargetValue <= nBest - nTargetValue)
        setCoinsRet.insert(pcoinLowestLarger);
    else
    {
        //for every index of small transactions
        for (int i = 0; i < vValue.size(); i++)
            //if the small transaction is in the winning subset
            if (vfBest[i])
                //insert the transaction in the output coin set
                setCoinsRet.insert(vValue[i].second);

        //SATOSHI_START
        //// debug print
        //SATOSHI_END
        printf("SelectCoins() best subset: ");

        //for every transaction in vValue, literally just print some debug statements
        for (int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                printf("%s ", FormatMoney(vValue[i].first).c_str());
        printf("total %s\n", FormatMoney(nBest).c_str());
    }

    //terminate!
    return true;
}

//Creates a transaction with source transactions with sum greater or equal than nValue. Adds output to sender if change exists, add inputs to Vin, sign all source transaction outputs, and add source transactions to the wallet transaction's vtxprev
bool CreateTransaction(CScript scriptPubKey, int64 nValue, CWalletTx &wtxNew, int64 &nFeeRequiredRet)
{
    //Set the required fee to 0
    nFeeRequiredRet = 0;

    //lock main
    CRITICAL_BLOCK(cs_main)
    {
        //SATOSHI_START
        // txdb must be opened before the mapWallet lock
        //SATOSHI_END
        CTxDB txdb("r");

        //lock wallet
        CRITICAL_BLOCK(cs_mapWallet)
        {
            //I have no idea where nTransactionFee is ever modified aside from initialization
            int64 nFee = nTransactionFee;

            //endless loop 
            loop
            {
                //The wallet transaction in the param should not be initialized

                //Ensure param transaction clears its inputs and outputs just in case
                wtxNew.vin.clear();
                wtxNew.vout.clear();

                //If the target value of transaction is less than 0 then return
                if (nValue < 0)
                    return false;

                //nValueOut represents the value that will be output from transaction
                int64 nValueOut = nValue;

                //Add on the fee to nValue
                nValue += nFee;

                //SATOSHI_START
                // Choose coins to use
                //SATOSHI_END

                //Create a set of wallet transactoons
                set<CWalletTx *> setCoins;

                //Populate the set of transactions with a subset of transactions in mapWallet that most closely correspond to nValue
                if (!SelectCoins(nValue, setCoins))
                    return false;

                //make a placeholder the sum of the transaction outputs
                int64 nValueIn = 0;

                //For each transaction
                foreach (CWalletTx *pcoin, setCoins)
                    //Sum the value of all transaction into nValueIn
                    nValueIn += pcoin->GetCredit();

                //SATOSHI_START
                // Fill vout[0] to the payee
                //SATOSHI_END

                //Add an output payment to scriptPubKey (which I imagine is the recipient address)
                wtxNew.vout.push_back(CTxOut(nValueOut, scriptPubKey));

                //SATOSHI_START
                // Fill vout[1] back to self with any change
                //SATOSHI_END

                //If the value in is greater than nValue
                if (nValueIn > nValue)
                {
                    //SATOSHI_START
                    // Use the same key as one of the coins
                    //SAOSHI_END

                    //Make vector of chars
                    vector<unsigned char> vchPubKey;

                    //Get the first transaction in list used to source funds for this transaction
                    CTransaction &txFirst = *(*setCoins.begin());

                    //For every output in the first transaction
                    foreach (const CTxOut &txout, txFirst.vout)
                        //if the output is aimed at us
                        if (txout.IsMine())
                            //Idk why we should break on this, I'll revisit
                            if (ExtractPubKey(txout.scriptPubKey, true, vchPubKey))
                                break;

                    //if the pubKey vector is empty, return false (should have some pub keys in there)
                    if (vchPubKey.empty())
                        return false;

                    //SATOSHI_START
                    // Fill vout[1] to ourself
                    //SATOSHI_END

                    //Make a new script
                    CScript scriptPubKey;

                    //Put in whatever is in vchPubKey and the checksig operation
                    scriptPubKey << vchPubKey << OP_CHECKSIG;

                    //Add an output to ourselves of the difference between the value of the source transactions and the value we'd like to pay
                    wtxNew.vout.push_back(CTxOut(nValueIn - nValue, scriptPubKey));
                }

                //SATOSHI_START
                // Fill vin
                //SATOSHI_END
                foreach (CWalletTx *pcoin, setCoins)
                    for (int nOut = 0; nOut < pcoin->vout.size(); nOut++)
                        if (pcoin->vout[nOut].IsMine())
                            wtxNew.vin.push_back(CTxIn(pcoin->GetHash(), nOut));

                //SATOSHI_START
                // Sign
                //SATOSHI_END

                //Initialize nIn to 0. this will represent the count of input into our new (current) transaction
                int nIn = 0;

                //For each wallet transaction in the set of coins being used to fund this transaction
                foreach (CWalletTx *pcoin, setCoins)
                    //for every output
                    for (int nOut = 0; nOut < pcoin->vout.size(); nOut++)
                        //if the output is ours
                        if (pcoin->vout[nOut].IsMine())
                            //sign this new transaction?
                            //incrememnt nIn
                            SignSignature(*pcoin, wtxNew, nIn++);

                //SATOSHI_START
                // Check that enough fee is included
                //SATOSHI_END
                //Set nFee to the transaction's calculated fee if the fee is below that
                if (nFee < wtxNew.GetMinFee(true))
                {
                    nFee = nFeeRequiredRet = wtxNew.GetMinFee(true);
                    continue;
                }

                //SATOSHI_START
                // Fill vtxPrev by copying from previous transactions vtxPrev
                //SATOSHI_END
                //Adds transactions from vin from CTransaction interface to vtxPrev in the dervied CtxWalletTx class
                wtxNew.AddSupportingTransactions(txdb);
                //time received *should* be the same as transaction
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

//SATOSHI_START
// Call after CreateTransaction unless you want to abort
//SATOSHI_END

//Adds a wallet transaction to mapWallet and marks each of the input transactions as spent before updating the input transactions to disk
bool CommitTransactionSpent(const CWalletTx &wtxNew)
{
    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(cs_mapWallet)
    {
        //SATOSHI_START
        //// todo: make this transactional, never want to add a transaction
        ////  without marking spent transactions
        // Add tx to wallet, because if it has change it's also ours,
        // otherwise just for transaction history.
        //SATOSHI_END


        //Add the wallet transaction to mapWallet
        AddToWallet(wtxNew);

        //SATOSHI_START
        // Mark old coins as spent
        //SATOSHI_END

        //Created a set of wallet transaction pointers
        set<CWalletTx *> setCoins;

        //For each input transaction of the new wallet transaction
        foreach (const CTxIn &txin, wtxNew.vin)
            //Find the input transaction in mapWallet and insert it into setCoins
            setCoins.insert(&mapWallet[txin.prevout.hash]);

        //For each input wallet transaction in setCoins
        foreach (CWalletTx *pcoin, setCoins)
        {
            //Mark the wallet transaction as spent
            pcoin->fSpent = true;
            //Write change to disk
            pcoin->WriteToDisk();
            //Add wallet update
            vWalletUpdated.push_back(make_pair(pcoin->GetHash(), false));
        }
    }
    MainFrameRepaint();
    return true;
}

//Creates the transaction, commits it, accepts it, and relays it to other ndoes
bool SendMoney(CScript scriptPubKey, int64 nValue, CWalletTx &wtxNew)
{

    //lock on main
    CRITICAL_BLOCK(cs_main)
    {
        //Declare fee required
        int64 nFeeRequired;
        //Create the transaction. wtxNew is an uninitialized wallet transaction from ui.cpp. This call to CreateTransaction should populate it.
        if (!CreateTransaction(scriptPubKey, nValue, wtxNew, nFeeRequired))
        {
            //In the case where transaction isn't successful, declare an error string
            string strError;
            if (nValue + nFeeRequired > GetBalance())
                strError = strprintf("Error: This is an oversized transaction that requires a transaction fee of %s ", FormatMoney(nFeeRequired).c_str());
            else
                strError = "Error: Transaction creation failed ";
            wxMessageBox(strError, "Sending...");
            return error("SendMoney() : %s\n", strError.c_str());
        }

        //Try and commit the transaction as spent. This means adding the transaction to mapWallet and marking each input as spent before writing to disk.
        if (!CommitTransactionSpent(wtxNew))
        {
            wxMessageBox("Error finalizing transaction", "Sending...");
            return error("SendMoney() : Error finalizing transaction");
        }

        printf("SendMoney: %s\n", wtxNew.GetHash().ToString().substr(0, 6).c_str());

        //SATOSHI_START
        // Broadcast
        //SATOSHI_END

        //Try to get the transaction accepted. It gets upsert to memory pool here (mapTransactions as opposed to adding it to mapWallet above) and updates are even allowed in certain scenarios. Connect inputs etc.
        if (!wtxNew.AcceptTransaction())
        {
            //SATOSHI_START
            // This must not fail. The transaction has already been signed and recorded.
            //SATOSHI_END
            throw runtime_error("SendMoney() : wtxNew.AcceptTransaction() failed\n");
            wxMessageBox("Error: Transaction not valid", "Sending...");
            return error("SendMoney() : Error: Transaction not valid");
        }
        
        //Tell other nodes about this transaction
        wtxNew.RelayWalletTransaction();
    }
    MainFrameRepaint();
    return true;
}
