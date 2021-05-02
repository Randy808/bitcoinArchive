// Copyright (c) 2009 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"




#pragma pack(1)
struct ircaddr
{
    int ip;
    short port;
};

string EncodeAddress(const CAddress& addr)
{
    struct ircaddr tmp;
    tmp.ip    = addr.ip;
    tmp.port  = addr.port;

    vector<unsigned char> vch(UBEGIN(tmp), UEND(tmp));
    return string("u") + EncodeBase58Check(vch);
}

bool DecodeAddress(string str, CAddress& addr)
{
    vector<unsigned char> vch;
    if (!DecodeBase58Check(str.substr(1), vch))
        return false;

    struct ircaddr tmp;
    if (vch.size() != sizeof(tmp))
        return false;
    memcpy(&tmp, &vch[0], sizeof(tmp));

    addr  = CAddress(tmp.ip, tmp.port);
    return true;
}






static bool Send(SOCKET hSocket, const char* pszSend)
{
    if (strstr(pszSend, "PONG") != pszSend)
        printf("SENDING: %s\n", pszSend);
    const char* psz = pszSend;
    const char* pszEnd = psz + strlen(psz);
    while (psz < pszEnd)
    {
        int ret = send(hSocket, psz, pszEnd - psz, 0);
        if (ret < 0)
            return false;
        psz += ret;
    }
    return true;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    loop
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
        }
        else if (nBytes <= 0)
        {
            if (!strLine.empty())
                return true;
            // socket closed
            printf("IRC socket closed\n");
            return false;
        }
        else
        {
            // socket error
            int nErr = WSAGetLastError();
            if (nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
            {
                printf("IRC recv failed: %d\n", nErr);
                return false;
            }
        }
    }
}

bool RecvLineIRC(SOCKET hSocket, string& strLine)
{
    loop
    {
        bool fRet = RecvLine(hSocket, strLine);
        if (fRet)
        {
            if (fShutdown)
                return false;
            vector<string> vWords;
            ParseString(strLine, ' ', vWords);
            if (vWords[0] == "PING")
            {
                strLine[1] = 'O';
                strLine += '\r';
                Send(hSocket, strLine.c_str());
                continue;
            }
        }
        return fRet;
    }
}

bool RecvUntil(SOCKET hSocket, const char* psz1, const char* psz2=NULL, const char* psz3=NULL)
{
    loop
    {
        string strLine;
        if (!RecvLineIRC(hSocket, strLine))
            return false;
        printf("IRC %s\n", strLine.c_str());
        if (psz1 && strLine.find(psz1) != -1)
            return true;
        if (psz2 && strLine.find(psz2) != -1)
            return true;
        if (psz3 && strLine.find(psz3) != -1)
            return true;
    }
}




bool fRestartIRCSeed = false;


//Joins irc channel with socket and looks for other join messages to find other nodes.
void ThreadIRCSeed(void* parg)
{
    loop
    {
        //Performs a dns lookup over udp on the parameter to get hostent struct: http://www.on-time.com/rtos-32-docs/rtip-32/reference-manual/socket-api/gethostbyname.htm
        struct hostent* phostent = gethostbyname("chat.freenode.net");


        //tries to get the address and manually assigns port 6667 to address data structure
        CAddress addrConnect(*(u_long*)phostent->h_addr_list[0], htons(6667));

        //Alias to int (probably to denote a socket id)
        SOCKET hSocket;

        //Connect socket using Address and allowing population of hSocket as id
        if (!ConnectSocket(addrConnect, hSocket))
        {
            printf("IRC connect failed\n");
            return;
        }

        //loops until one of the following strings are found coming from irc
        if (!RecvUntil(hSocket, "Found your hostname", "using your IP address instead", "Couldn't look up your hostname"))
        {
            closesocket(hSocket);
            return;
        }

        //Gets address to use as nickname in IRC
        string strMyName = EncodeAddress(addrLocalHost);

        //A random name is chosen if this node can't be routed to I guess
        if (!addrLocalHost.IsRoutable())
            strMyName = strprintf("x%u", GetRand(1000000000));

        //uses nickname of ip
        Send(hSocket, strprintf("NICK %s\r", strMyName.c_str()).c_str());

        //Send the irc server a command starting with 'User', idk what command does
        Send(hSocket, strprintf("USER %s 8 * : %s\r", strMyName.c_str(), strMyName.c_str()).c_str());

        //looking for 004
        if (!RecvUntil(hSocket, " 004 "))
        {
            closesocket(hSocket);
            return;
        }
        Sleep(500);

        //Join bitcoin chat
        Send(hSocket, "JOIN #bitcoin\r");
        Send(hSocket, "WHO #bitcoin\r");

        //keep looking for new irc lines until RestartIRCSeed is closed for some reason
        while (!fRestartIRCSeed)
        {
            string strLine;
            if (fShutdown || !RecvLineIRC(hSocket, strLine))
            {
                closesocket(hSocket);
                return;
            }
            if (strLine.empty() || strLine[0] != ':')
                continue;
            printf("IRC %s\n", strLine.c_str());

            vector<string> vWords;
            ParseString(strLine, ' ', vWords);
            if (vWords.size() < 2)
                continue;

            char pszName[10000];
            pszName[0] = '\0';

            if (vWords[1] == "352" && vWords.size() >= 8)
            {
                //S
                // index 7 is limited to 16 characters
                // could get full length name at index 10, but would be different from join messages
                //S_E
                strcpy(pszName, vWords[7].c_str());
                printf("GOT WHO: [%s]  ", pszName);
            }

            //Gets pszName from join message on irc
            if (vWords[1] == "JOIN")
            {
                //S
                // :username!username@50000007.F000000B.90000002.IP JOIN :#channelname
                //S_E
                strcpy(pszName, vWords[0].c_str() + 1);
                if (strchr(pszName, '!'))
                    *strchr(pszName, '!') = '\0';
                printf("GOT JOIN: [%s]  ", pszName);
            }

            //If the username starts with 'u', parse the username as an address and add it to address db
            if (pszName[0] == 'u')
            {
                CAddress addr;
                if (DecodeAddress(pszName, addr))
                {
                    CAddrDB addrdb;
                    if (AddAddress(addrdb, addr))
                        printf("new  ");
                    addr.print();
                }
                else
                {
                    printf("decode failed\n");
                }
            }
        }

        fRestartIRCSeed = false;
        closesocket(hSocket);
    }
}










#ifdef TEST
int main(int argc, char *argv[])
{
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2,2), &wsadata) != NO_ERROR)
    {
        printf("Error at WSAStartup()\n");
        return false;
    }

    ThreadIRCSeed(NULL);

    WSACleanup();
    return 0;
}
#endif
