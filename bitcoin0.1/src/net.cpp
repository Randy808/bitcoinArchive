// Copyright (c) 2009 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"
#include <winsock2.h>

void ThreadMessageHandler2(void *parg);
void ThreadSocketHandler2(void *parg);
void ThreadOpenConnections2(void *parg);

//
// Global state variables
//
bool fClient = false;
uint64 nLocalServices = (fClient ? 0 : NODE_NETWORK);
CAddress addrLocalHost(0, DEFAULT_PORT, nLocalServices);
CNode nodeLocalHost(INVALID_SOCKET, CAddress("127.0.0.1", nLocalServices));
CNode *pnodeLocalHost = &nodeLocalHost;
bool fShutdown = false;
array<bool, 10> vfThreadRunning;
vector<CNode *> vNodes;
CCriticalSection cs_vNodes;
map<vector<unsigned char>, CAddress> mapAddresses;
CCriticalSection cs_mapAddresses;
map<CInv, CDataStream> mapRelay;
deque<pair<int64, CInv>> vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64> mapAlreadyAskedFor;

CAddress addrProxy;

bool ConnectSocket(const CAddress &addrConnect, SOCKET &hSocketRet)
{
    hSocketRet = INVALID_SOCKET;

    SOCKET hSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (hSocket == INVALID_SOCKET)
        return false;

    bool fRoutable = !(addrConnect.GetByte(3) == 10 || (addrConnect.GetByte(3) == 192 && addrConnect.GetByte(2) == 168));
    bool fProxy = (addrProxy.ip && fRoutable);
    struct sockaddr_in sockaddr = (fProxy ? addrProxy.GetSockAddr() : addrConnect.GetSockAddr());

    if (connect(hSocket, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == SOCKET_ERROR)
    {
        closesocket(hSocket);
        return false;
    }

    if (fProxy)
    {
        printf("Proxy connecting to %s\n", addrConnect.ToString().c_str());
        char pszSocks4IP[] = "\4\1\0\0\0\0\0\0user";
        memcpy(pszSocks4IP + 2, &addrConnect.port, 2);
        memcpy(pszSocks4IP + 4, &addrConnect.ip, 4);
        char *pszSocks4 = pszSocks4IP;
        int nSize = sizeof(pszSocks4IP);

        int ret = send(hSocket, pszSocks4, nSize, 0);
        if (ret != nSize)
        {
            closesocket(hSocket);
            return error("Error sending to proxy\n");
        }
        char pchRet[8];
        if (recv(hSocket, pchRet, 8, 0) != 8)
        {
            closesocket(hSocket);
            return error("Error reading proxy response\n");
        }
        if (pchRet[1] != 0x5a)
        {
            closesocket(hSocket);
            return error("Proxy returned error %d\n", pchRet[1]);
        }
        printf("Proxy connection established %s\n", addrConnect.ToString().c_str());
    }

    hSocketRet = hSocket;
    return true;
}

bool GetMyExternalIP(unsigned int &ipRet)
{
    CAddress addrConnect("72.233.89.199:80"); // whatismyip.com 198-200

    SOCKET hSocket;
    if (!ConnectSocket(addrConnect, hSocket))
        return error("GetMyExternalIP() : connection to %s failed\n", addrConnect.ToString().c_str());

    char *pszGet =
        "GET /automation/n09230945.asp HTTP/1.1\r\n"
        "Host: www.whatismyip.com\r\n"
        "User-Agent: Bitcoin/0.1\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(hSocket, pszGet, strlen(pszGet), 0);

    string strLine;
    while (RecvLine(hSocket, strLine))
    {
        if (strLine.empty())
        {
            if (!RecvLine(hSocket, strLine))
            {
                closesocket(hSocket);
                return false;
            }
            closesocket(hSocket);
            CAddress addr(strLine.c_str());
            printf("GetMyExternalIP() received [%s] %s\n", strLine.c_str(), addr.ToString().c_str());
            if (addr.ip == 0)
                return false;
            ipRet = addr.ip;
            return true;
        }
    }
    closesocket(hSocket);
    return error("GetMyExternalIP() : connection closed\n");
}

bool AddAddress(CAddrDB &addrdb, const CAddress &addr)
{
    if (!addr.IsRoutable())
        return false;
    if (addr.ip == addrLocalHost.ip)
        return false;
    CRITICAL_BLOCK(cs_mapAddresses)
    {
        map<vector<unsigned char>, CAddress>::iterator it = mapAddresses.find(addr.GetKey());
        if (it == mapAddresses.end())
        {
            // New address
            mapAddresses.insert(make_pair(addr.GetKey(), addr));
            addrdb.WriteAddress(addr);
            return true;
        }
        else
        {
            CAddress &addrFound = (*it).second;
            if ((addrFound.nServices | addr.nServices) != addrFound.nServices)
            {
                // Services have been added
                addrFound.nServices |= addr.nServices;
                addrdb.WriteAddress(addrFound);
                return true;
            }
        }
    }
    return false;
}

void AbandonRequests(void (*fn)(void *, CDataStream &), void *param1)
{
    // If the dialog might get closed before the reply comes back,
    // call this in the destructor so it doesn't get called after it's deleted.
    CRITICAL_BLOCK(cs_vNodes)
    {
        foreach (CNode *pnode, vNodes)
        {
            CRITICAL_BLOCK(pnode->cs_mapRequests)
            {
                for (map<uint256, CRequestTracker>::iterator mi = pnode->mapRequests.begin(); mi != pnode->mapRequests.end();)
                {
                    CRequestTracker &tracker = (*mi).second;
                    if (tracker.fn == fn && tracker.param1 == param1)
                        pnode->mapRequests.erase(mi++);
                    else
                        mi++;
                }
            }
        }
    }
}

//
// Subscription methods for the broadcast and subscription system.
// Channel numbers are message numbers, i.e. MSG_TABLE and MSG_PRODUCT.
//
// The subscription system uses a meet-in-the-middle strategy.
// With 100,000 nodes, if senders broadcast to 1000 random nodes and receivers
// subscribe to 1000 random nodes, 99.995% (1 - 0.99^1000) of messages will get through.
//

bool AnySubscribed(unsigned int nChannel)
{
    if (pnodeLocalHost->IsSubscribed(nChannel))
        return true;
    CRITICAL_BLOCK(cs_vNodes)
    foreach (CNode *pnode, vNodes)
        if (pnode->IsSubscribed(nChannel))
            return true;
    return false;
}

bool CNode::IsSubscribed(unsigned int nChannel)
{
    if (nChannel >= vfSubscribe.size())
        return false;
    return vfSubscribe[nChannel];
}

void CNode::Subscribe(unsigned int nChannel, unsigned int nHops)
{
    if (nChannel >= vfSubscribe.size())
        return;

    if (!AnySubscribed(nChannel))
    {
        // Relay subscribe
        CRITICAL_BLOCK(cs_vNodes)
        foreach (CNode *pnode, vNodes)
            if (pnode != this)
                pnode->PushMessage("subscribe", nChannel, nHops);
    }

    vfSubscribe[nChannel] = true;
}

void CNode::CancelSubscribe(unsigned int nChannel)
{
    if (nChannel >= vfSubscribe.size())
        return;

    // Prevent from relaying cancel if wasn't subscribed
    if (!vfSubscribe[nChannel])
        return;
    vfSubscribe[nChannel] = false;

    if (!AnySubscribed(nChannel))
    {
        // Relay subscription cancel
        CRITICAL_BLOCK(cs_vNodes)
        foreach (CNode *pnode, vNodes)
            if (pnode != this)
                pnode->PushMessage("sub-cancel", nChannel);

        // Clear memory, no longer subscribed
        if (nChannel == MSG_PRODUCT)
            CRITICAL_BLOCK(cs_mapProducts)
        mapProducts.clear();
    }
}

CNode *FindNode(unsigned int ip)
{
    CRITICAL_BLOCK(cs_vNodes)
    {
        foreach (CNode *pnode, vNodes)
            if (pnode->addr.ip == ip)
                return (pnode);
    }
    return NULL;
}

CNode *FindNode(CAddress addr)
{
    CRITICAL_BLOCK(cs_vNodes)
    {
        foreach (CNode *pnode, vNodes)
            if (pnode->addr == addr)
                return (pnode);
    }
    return NULL;
}

CNode *ConnectNode(CAddress addrConnect, int64 nTimeout)
{
    if (addrConnect.ip == addrLocalHost.ip)
        return NULL;

    // Look for an existing connection
    CNode *pnode = FindNode(addrConnect.ip);
    if (pnode)
    {
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();
        return pnode;
    }

    /// debug print
    printf("trying %s\n", addrConnect.ToString().c_str());

    // Connect
    SOCKET hSocket;
    if (ConnectSocket(addrConnect, hSocket))
    {
        /// debug print
        printf("connected %s\n", addrConnect.ToString().c_str());

        // Add node
        CNode *pnode = new CNode(hSocket, addrConnect, false);
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();
        CRITICAL_BLOCK(cs_vNodes)
        vNodes.push_back(pnode);

        CRITICAL_BLOCK(cs_mapAddresses)
        mapAddresses[addrConnect.GetKey()].nLastFailed = 0;
        return pnode;
    }
    else
    {
        CRITICAL_BLOCK(cs_mapAddresses)
        mapAddresses[addrConnect.GetKey()].nLastFailed = GetTime();
        return NULL;
    }
}

void CNode::Disconnect()
{
    printf("disconnecting node %s\n", addr.ToString().c_str());

    closesocket(hSocket);

    // All of a nodes broadcasts and subscriptions are automatically torn down
    // when it goes down, so a node has to stay up to keep its broadcast going.

    CRITICAL_BLOCK(cs_mapProducts)
    for (map<uint256, CProduct>::iterator mi = mapProducts.begin(); mi != mapProducts.end();)
        AdvertRemoveSource(this, MSG_PRODUCT, 0, (*(mi++)).second);

    // Cancel subscriptions
    for (unsigned int nChannel = 0; nChannel < vfSubscribe.size(); nChannel++)
        if (vfSubscribe[nChannel])
            CancelSubscribe(nChannel);
}

//Seems like it checks for shutdowns and runs ThreadSocketHandler2 periodically
void ThreadSocketHandler(void *parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadSocketHandler(parg));

    loop
    {
        vfThreadRunning[0] = true;
        CheckForShutdown(0);
        try
        {
            ThreadSocketHandler2(parg);
        }
        CATCH_PRINT_EXCEPTION("ThreadSocketHandler()")
        vfThreadRunning[0] = false;
        Sleep(5000);
    }
}

void ThreadSocketHandler2(void *parg)
{
    printf("ThreadSocketHandler started\n");
    SOCKET hListenSocket = *(SOCKET *)parg;
    list<CNode *> vNodesDisconnected;
    int nPrevNodeCount = 0;

    loop
    {
        //
        // Disconnect nodes
        //
        CRITICAL_BLOCK(cs_vNodes)
        {
            // Disconnect duplicate connections
            map<unsigned int, CNode *> mapFirst;
            foreach (CNode *pnode, vNodes)
            {
                if (pnode->fDisconnect)
                    continue;
                unsigned int ip = pnode->addr.ip;
                if (mapFirst.count(ip) && addrLocalHost.ip < ip)
                {
                    // In case two nodes connect to each other at once,
                    // the lower ip disconnects its outbound connection
                    CNode *pnodeExtra = mapFirst[ip];

                    if (pnodeExtra->GetRefCount() > (pnodeExtra->fNetworkNode ? 1 : 0))
                        swap(pnodeExtra, pnode);

                    if (pnodeExtra->GetRefCount() <= (pnodeExtra->fNetworkNode ? 1 : 0))
                    {
                        printf("(%d nodes) disconnecting duplicate: %s\n", vNodes.size(), pnodeExtra->addr.ToString().c_str());
                        if (pnodeExtra->fNetworkNode && !pnode->fNetworkNode)
                        {
                            pnode->AddRef();
                            swap(pnodeExtra->fNetworkNode, pnode->fNetworkNode);
                            pnodeExtra->Release();
                        }
                        pnodeExtra->fDisconnect = true;
                    }
                }
                mapFirst[ip] = pnode;
            }

            // Disconnect unused nodes
            vector<CNode *> vNodesCopy = vNodes;
            foreach (CNode *pnode, vNodesCopy)
            {
                if (pnode->ReadyToDisconnect() && pnode->vRecv.empty() && pnode->vSend.empty())
                {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());
                    pnode->Disconnect();

                    // hold in disconnected pool until all refs are released
                    pnode->nReleaseTime = max(pnode->nReleaseTime, GetTime() + 5 * 60);
                    if (pnode->fNetworkNode)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode *> vNodesDisconnectedCopy = vNodesDisconnected;
            foreach (CNode *pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                    TRY_CRITICAL_BLOCK(pnode->cs_vRecv)
                    TRY_CRITICAL_BLOCK(pnode->cs_mapRequests)
                    TRY_CRITICAL_BLOCK(pnode->cs_inventory)
                    fDelete = true;
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if (vNodes.size() != nPrevNodeCount)
        {
            nPrevNodeCount = vNodes.size();
            MainFrameRepaint();
        }

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        struct fd_set fdsetRecv;
        struct fd_set fdsetSend;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        SOCKET hSocketMax = 0;
        FD_SET(hListenSocket, &fdsetRecv);
        hSocketMax = max(hSocketMax, hListenSocket);
        CRITICAL_BLOCK(cs_vNodes)
        {
            foreach (CNode *pnode, vNodes)
            {
                FD_SET(pnode->hSocket, &fdsetRecv);
                hSocketMax = max(hSocketMax, pnode->hSocket);
                TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                if (!pnode->vSend.empty())
                    FD_SET(pnode->hSocket, &fdsetSend);
            }
        }

        vfThreadRunning[0] = false;
        int nSelect = select(hSocketMax + 1, &fdsetRecv, &fdsetSend, NULL, &timeout);
        vfThreadRunning[0] = true;
        CheckForShutdown(0);
        if (nSelect == SOCKET_ERROR)
        {
            int nErr = WSAGetLastError();
            printf("select failed: %d\n", nErr);
            for (int i = 0; i <= hSocketMax; i++)
            {
                FD_SET(i, &fdsetRecv);
                FD_SET(i, &fdsetSend);
            }
            Sleep(timeout.tv_usec / 1000);
        }
        RandAddSeed();

        //// debug print
        //foreach(CNode* pnode, vNodes)
        //{
        //    printf("vRecv = %-5d ", pnode->vRecv.size());
        //    printf("vSend = %-5d    ", pnode->vSend.size());
        //}
        //printf("\n");

        //
        // Accept new connections
        //
        if (FD_ISSET(hListenSocket, &fdsetRecv))
        {
            struct sockaddr_in sockaddr;
            int len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr *)&sockaddr, &len);
            CAddress addr(sockaddr);
            if (hSocket == INVALID_SOCKET)
            {
                if (WSAGetLastError() != WSAEWOULDBLOCK)
                    printf("ERROR ThreadSocketHandler accept failed: %d\n", WSAGetLastError());
            }
            else
            {
                printf("accepted connection from %s\n", addr.ToString().c_str());
                CNode *pnode = new CNode(hSocket, addr, true);
                pnode->AddRef();
                CRITICAL_BLOCK(cs_vNodes)
                vNodes.push_back(pnode);
            }
        }

        //
        // Service each socket
        //
        vector<CNode *> vNodesCopy;
        CRITICAL_BLOCK(cs_vNodes)
        vNodesCopy = vNodes;
        foreach (CNode *pnode, vNodesCopy)
        {
            CheckForShutdown(0);
            SOCKET hSocket = pnode->hSocket;

            //
            // Receive
            //
            if (FD_ISSET(hSocket, &fdsetRecv))
            {
                TRY_CRITICAL_BLOCK(pnode->cs_vRecv)
                {
                    CDataStream &vRecv = pnode->vRecv;
                    unsigned int nPos = vRecv.size();

                    // typical socket buffer is 8K-64K
                    const unsigned int nBufSize = 0x10000;
                    vRecv.resize(nPos + nBufSize);
                    int nBytes = recv(hSocket, &vRecv[nPos], nBufSize, 0);
                    vRecv.resize(nPos + max(nBytes, 0));
                    if (nBytes == 0)
                    {
                        // socket closed gracefully
                        if (!pnode->fDisconnect)
                            printf("recv: socket closed\n");
                        pnode->fDisconnect = true;
                    }
                    else if (nBytes < 0)
                    {
                        // socket error
                        int nErr = WSAGetLastError();
                        if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                        {
                            if (!pnode->fDisconnect)
                                printf("recv failed: %d\n", nErr);
                            pnode->fDisconnect = true;
                        }
                    }
                }
            }

            //
            // Send
            //
            if (FD_ISSET(hSocket, &fdsetSend))
            {
                TRY_CRITICAL_BLOCK(pnode->cs_vSend)
                {
                    CDataStream &vSend = pnode->vSend;
                    if (!vSend.empty())
                    {
                        int nBytes = send(hSocket, &vSend[0], vSend.size(), 0);
                        if (nBytes > 0)
                        {
                            vSend.erase(vSend.begin(), vSend.begin() + nBytes);
                        }
                        else if (nBytes == 0)
                        {
                            if (pnode->ReadyToDisconnect())
                                pnode->vSend.clear();
                        }
                        else
                        {
                            printf("send error %d\n", nBytes);
                            if (pnode->ReadyToDisconnect())
                                pnode->vSend.clear();
                        }
                    }
                }
            }
        }

        Sleep(10);
    }
}

void ThreadOpenConnections(void *parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadOpenConnections(parg));

    loop
    {
        vfThreadRunning[1] = true;
        CheckForShutdown(1);
        try
        {
            ThreadOpenConnections2(parg);
        }
        CATCH_PRINT_EXCEPTION("ThreadOpenConnections()")
        vfThreadRunning[1] = false;
        Sleep(5000);
    }
}

//Look through addresses we collected on other thread and try to add non-sequential ip addresses. Iteratively pick one random ip address from collected ips and add that to our vNodes until all remaining ips (nLimit) are added.
void ThreadOpenConnections2(void *parg)
{
    printf("ThreadOpenConnections started\n");

    //SATOSHI_START
    // Initiate network connections
    //SATOSHI_END
    const int nMaxConnections = 15;
    loop
    {
        //SATOSHI_START
        // Wait
        //SATOSHI_END

        //If the thread isn't running, set the 2nd index of vfThreadRunning to false
        //I think vfThreadRunning has an index for all threads spawned from 'StartNode'
        vfThreadRunning[1] = false;

        //Sleep since we said the thread wasn't running right now
        Sleep(500);

        //While the vnode size is more than we want to connect to or they're bigger than our number of addresses
        while (vNodes.size() >= nMaxConnections || vNodes.size() >= mapAddresses.size())
        {
            //keep sleeping
            CheckForShutdown(1);
            Sleep(2000);
        }

        //thread starts running if we can make more connections
        vfThreadRunning[1] = true;
        //Always check for a shutdown call
        CheckForShutdown(1);

        //SATOSHI_START
        // Make a list of unique class C's
        //SATOSHI_END

        //Make an ipc mask 
        //What is ipc?
        //From satoshi comment below, it looks like this is an ip class c mask that will retain the first 3 bytes but 0 out the last one to prevent class C addresses that are allocated sequentially from being added
        // bytes equal {255, 255, 255, 0}
        unsigned char pchIPCMask[4] = {0xff, 0xff, 0xff, 0x00};

        //Make the array a pointer to a number, then dereference that?
        //ipc mask is a 4 byte array so he's converting the number into an int
        unsigned int nIPCMask = *(unsigned int *)pchIPCMask;

        //Make an ipc vector
        vector<unsigned int> vIPC;

        //Lock the 'mappAddresses' mutex 
        CRITICAL_BLOCK(cs_mapAddresses)
        {
            //Pre-allocate space for the addresses
            vIPC.reserve(mapAddresses.size());

            //Iniializes Prev to 0
            unsigned int nPrev = 0;

            //For each address in mapAddresses
            foreach (const PAIRTYPE(vector<unsigned char>, CAddress) & item, mapAddresses)
            {
                ///Get the adddress
                const CAddress &addr = item.second;

                //If the address is not an ipv4 address, skip it
                if (!addr.IsIPv4())
                    continue;

                //SATOSHI_START
                // Taking advantage of mapAddresses being in sorted order,
                // with IPs of the same class C grouped together.
                //SATOSHI_END

                //Mask the ip with the nIPCMask
                unsigned int ipC = addr.ip & nIPCMask;

                //If ipc (the masked ip) is not equal to nprev
                if (ipC != nPrev)
                    //Set nPrev to the maskedIp and add it to vIPC
                    vIPC.push_back(nPrev = ipC);
            }
        }

        //SATOSHI_START
        //
        // The IP selection process is designed to limit vulnerability to address flooding.
        // Any class C (a.b.c.?) has an equal chance of being chosen, then an IP is
        // chosen within the class C.  An attacker may be able to allocate many IPs, but
        // they would normally be concentrated in blocks of class C's.  They can hog the
        // attention within their class C, but not the whole IP address space overall.
        // A lone node in a class C will get as much attention as someone holding all 255
        // IPs in another class C.
        //
        //SATOSHI_END

        //Initialize success to false
        bool fSuccess = false;

        //Check how many ips we collected
        int nLimit = vIPC.size();

        //If not sucessful and the nLimit (ips we have) is greater than 0, enter loop and decrement limit
        while (!fSuccess && nLimit-- > 0)
        {
            //SATOSHI_START
            // Choose a random class C
            //SATOSHI_END
            unsigned int ipC = vIPC[GetRand(vIPC.size())];

            //SATOSHI_START
            // Organize all addresses in the class C by IP
            //SATOSHI_END
            map<unsigned int, vector<CAddress>> mapIP;

            //lock mapAddresses
            CRITICAL_BLOCK(cs_mapAddresses)
            {
                //180 shifted by the number of nodes is the delay (bigger delay for bigger nodes)
                unsigned int nDelay = ((30 * 60) << vNodes.size());

                //If delay is bigger than 8*60*60 (idk significance of expression; 80 minutes in seconds?)
                if (nDelay > 8 * 60 * 60)
                    //The nDelay is limited
                    nDelay = 8 * 60 * 60;

                //For every address of map addresses starting with the random ip we chose and going to a subsequence with all 1s for last byte?
                //Research  '.lower_bound' and 'upper_bound'
                //All Ipc should have last byte 0'd out from masking right before entering vIPC array
                for (map<vector<unsigned char>, CAddress>::iterator mi = mapAddresses.lower_bound(CAddress(ipC, 0).GetKey());
                     mi != mapAddresses.upper_bound(CAddress(ipC | ~nIPCMask, 0xffff).GetKey());
                     ++mi)
                {
                    //Get address
                    const CAddress &addr = (*mi).second;

                    //Exponential backoff? Idk wtf this is
                    unsigned int nRandomizer = (addr.nLastFailed * addr.ip * 7777U) % 20000;

                    //If the time from when the address last failed is bigger than the product of delay and randomizer (divided by 1000)
                    if (GetTime() - addr.nLastFailed > nDelay * nRandomizer / 10000)
                        //Add the ip address data structure to map using ip as key
                        //Not sure why vector is used in ip index, can there be more than one address for an ip index? Ahh, satoshi comment later on says there are different addresses for different ports
                        mapIP[addr.ip].push_back(addr);
                }
            }

            //If no ips were added, break cycle
            if (mapIP.empty())
                break;

            //SATOSHI_START
            // Choose a random IP in the class C
            //SATOSHI_END

            //Make mapIp iterator
            map<unsigned int, vector<CAddress>>::iterator mi = mapIP.begin();

            //Move to a random ip
            advance(mi, GetRand(mapIP.size()));

            //SATOSHI_START
            // Once we've chosen an IP, we'll try every given port before moving on
            //SATOSHI_END
            foreach (const CAddress &addrConnect, (*mi).second)
            {
                //make sure ip is not outselves, it's ipv4, and there is a node there
                if (addrConnect.ip == addrLocalHost.ip || !addrConnect.IsIPv4() || FindNode(addrConnect.ip))
                    continue;

                //Call 'ConnectNode' with validated ip
                CNode *pnode = ConnectNode(addrConnect);

                //If node reference wasn't created
                if (!pnode)
                    continue;

                //Let the node know it's a network node (set prop to true)
                pnode->fNetworkNode = true;

                //Check if we can be routed to
                if (addrLocalHost.IsRoutable())
                {
                    //SATOSHI_START
                    // Advertise our address
                    //SATOSHI_END

                    //Send out our ip using 'PushMessage'
                    vector<CAddress> vAddrToSend;
                    vAddrToSend.push_back(addrLocalHost);
                    //What does PushMessage do?
                    pnode->PushMessage("addr", vAddrToSend);
                }

                //SATOSHI_START
                // Get as many addresses as we can
                //SAOSHI_END
                pnode->PushMessage("getaddr");

                //SATOSHI_START
                ////// should the one on the receiving end do this too?
                // Subscribe our local subscription list
                //SATOSHI_END
                const unsigned int nHops = 0;

                //Go through local subscribe list
                for (unsigned int nChannel = 0; nChannel < pnodeLocalHost->vfSubscribe.size(); nChannel++)
                    //For certain channel indexes
                    if (pnodeLocalHost->vfSubscribe[nChannel])
                        //Let the connecting node know we're subscribed if we are
                        pnode->PushMessage("subscribe", nChannel, nHops);

                //Let success be true when an address with the proper port is added
                fSuccess = true;
                break;
            }
        }
    }
}

void ThreadMessageHandler(void *parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadMessageHandler(parg));

    loop
    {
        vfThreadRunning[2] = true;
        CheckForShutdown(2);
        try
        {
            ThreadMessageHandler2(parg);
        }
        CATCH_PRINT_EXCEPTION("ThreadMessageHandler()")
        vfThreadRunning[2] = false;
        Sleep(5000);
    }
}


//This is where nodes communicate information
void ThreadMessageHandler2(void *parg)
{
    printf("ThreadMessageHandler started\n");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    loop
    {
        //SATOSHI_START
        // Poll the connected nodes for messages
        //SATOSHI_END

        //Create a vNodes copy rref?
        vector<CNode *> vNodesCopy;
        CRITICAL_BLOCK(cs_vNodes)
        vNodesCopy = vNodes;

        //For each node in VnNodes
        foreach (CNode *pnode, vNodesCopy)
        {
            pnode->AddRef();

            //SATOSHI_START
            // Receive messages
            //SATOSHI_END
            TRY_CRITICAL_BLOCK(pnode->cs_vRecv)

            //process messages from node
            //How does node ref get populated with new info?
            ProcessMessages(pnode);

            //SATOSHI_START
            // Send messages
            //SATOSHI_END
            TRY_CRITICAL_BLOCK(pnode->cs_vSend)

            //Send messages to node
            SendMessages(pnode);

            pnode->Release();
        }

        // Wait and allow messages to bunch up
        vfThreadRunning[2] = false;
        Sleep(100);
        vfThreadRunning[2] = true;
        CheckForShutdown(2);
    }
}

//// todo: start one thread per processor, use getenv("NUMBER_OF_PROCESSORS")
void ThreadBitcoinMiner(void *parg)
{
    vfThreadRunning[3] = true;
    CheckForShutdown(3);
    try
    {
        bool fRet = BitcoinMiner();
        printf("BitcoinMiner returned %s\n\n\n", fRet ? "true" : "false");
    }
    CATCH_PRINT_EXCEPTION("BitcoinMiner()")
    vfThreadRunning[3] = false;
}

//Called from Ui::OnInit2
bool StartNode(string &strError)
{
    strError = "";

    //s
    // Sockets startup
    //S_E

    //contains info about a windows socket
    WSADATA wsadata;

    //create socket with socket data
    int ret = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        printf("%s\n", strError.c_str());
        return false;
    }

    //s
    // Get local host ip
    //s_e

    //gets current current host name
    char pszHostName[255];
    if (gethostname(pszHostName, 255) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Unable to get IP address of this computer (gethostname returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    //gets info of host by name
    struct hostent *pHostEnt = gethostbyname(pszHostName);
    if (!pHostEnt)
    {
        strError = strprintf("Error: Unable to get IP address of this computer (gethostbyname returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    //retrieves ip address of this host with port 8333
    addrLocalHost = CAddress(*(long *)(pHostEnt->h_addr_list[0]),
                             DEFAULT_PORT,
                             nLocalServices);
    printf("addrLocalHost = %s\n", addrLocalHost.ToString().c_str());

    //s
    // Create socket for listening for incoming connections
    SOCKET hListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    //s
    // Set to nonblocking, incoming connections will also inherit this
    //s_e

    u_long nOne = 1;
    if (ioctlsocket(hListenSocket, FIONBIO, &nOne) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (ioctlsocket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    //s
    // The sockaddr_in structure specifies the address family,
    // IP address, and port for the socket that is being bound
    //s_e

    int nRetryLimit = 15;

    //explicitly connects socket to local host
    struct sockaddr_in sockaddr = addrLocalHost.GetSockAddr();
    if (bind(hListenSocket, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf("Error: Unable to bind to port %s on this computer. The program is probably already running.", addrLocalHost.ToString().c_str());
        else
            strError = strprintf("Error: Unable to bind to port %s on this computer (bind returned error %d)", addrLocalHost.ToString().c_str(), nErr);
        printf("%s\n", strError.c_str());
        return false;
    }
    printf("bound to addrLocalHost = %s\n\n", addrLocalHost.ToString().c_str());

    //s
    // Listen for incoming connections
    //s_end
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    //S
    // Get our external IP address for incoming connections
    //S_END

    //What is addrIncoming?
    if (addrIncoming.ip)
        addrLocalHost.ip = addrIncoming.ip;

    //Get external ip from addrIncoming if defined or from local host ip
    if (GetMyExternalIP(addrLocalHost.ip))
    {
        addrIncoming = addrLocalHost;
        CWalletDB().WriteSetting("addrIncoming", addrIncoming);
    }

    //S
    // Get addresses from IRC and advertise ours
    //S_E

    //Start IRCSeed thread (to look for nodes?)
    if (_beginthread(ThreadIRCSeed, 0, NULL) == -1)
        printf("Error: _beginthread(ThreadIRCSeed) failed\n");

    //S
    //
    // Start threads
    //
    //S_E
    if (_beginthread(ThreadSocketHandler, 0, new SOCKET(hListenSocket)) == -1)
    {
        strError = "Error: _beginthread(ThreadSocketHandler) failed";
        printf("%s\n", strError.c_str());
        return false;
    }

    //Connecting nodes and adding them to vNodes
    if (_beginthread(ThreadOpenConnections, 0, NULL) == -1)
    {
        strError = "Error: _beginthread(ThreadOpenConnections) failed";
        printf("%s\n", strError.c_str());
        return false;
    }

    if (_beginthread(ThreadMessageHandler, 0, NULL) == -1)
    {
        strError = "Error: _beginthread(ThreadMessageHandler) failed";
        printf("%s\n", strError.c_str());
        return false;
    }

    return true;
}

bool StopNode()
{
    printf("StopNode()\n");
    fShutdown = true;
    nTransactionsUpdated++;
    while (count(vfThreadRunning.begin(), vfThreadRunning.end(), true))
        Sleep(10);
    Sleep(50);

    // Sockets shutdown
    WSACleanup();
    return true;
}

void CheckForShutdown(int n)
{
    if (fShutdown)
    {
        if (n != -1)
            vfThreadRunning[n] = false;
        _endthread();
    }
}
