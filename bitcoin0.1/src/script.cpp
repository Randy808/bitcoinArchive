// Copyright (c) 2009 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "headers.h"

bool CheckSig(vector<unsigned char> vchSig, vector<unsigned char> vchPubKey, CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType);



typedef vector<unsigned char> valtype;
static const valtype vchFalse(0);
static const valtype vchZero(0);
static const valtype vchTrue(1, 1);
static const CBigNum bnZero(0);
static const CBigNum bnOne(1);
static const CBigNum bnFalse(0);
static const CBigNum bnTrue(1);


//Returns whether the vector array 'vch' can be converted into a valid BigNum
bool CastToBool(const valtype& vch)
{
    return (CBigNum(vch) != bnZero);
}

void MakeSameSize(valtype& vch1, valtype& vch2)
{
    // Lengthen the shorter one
    if (vch1.size() < vch2.size())
        vch1.resize(vch2.size(), 0);
    if (vch2.size() < vch1.size())
        vch2.resize(vch1.size(), 0);
}



//SATOSHI_START
//
// Script is a stack machine (like Forth) that evaluates a predicate
// returning a bool indicating valid or not.  There are no loops.
//
//SATOSHI_END
#define stacktop(i)  (stack.at(stack.size()+(i)))
#define altstacktop(i)  (altstack.at(altstack.size()+(i)))


//Run the script as a stack based programming language by reading in opcodes and pushin onto the stack
bool EvalScript(const CScript& script, const CTransaction& txTo, unsigned int nIn, int nHashType,
                vector<vector<unsigned char> >* pvStackRet)
{
    //Declare pctx as CAutoBN_CTX which allocates space for a BignNum and holds an internal reference to the pointer
    CAutoBN_CTX pctx;

    //A script inherits from a vector so iterators are initialized at the beginning and end of the script
    CScript::const_iterator pc = script.begin();

    //script with  iterator starting at end
    CScript::const_iterator pend = script.end();

    //Another iterator starting at the beginning of script vector. Judging by its name (and existence) I think it'll be used differently.
    CScript::const_iterator pbegincodehash = script.begin();

    //Vector of bools
    vector<bool> vfExec;

    //Vector of char vectors :hmmm
    vector<valtype> stack;

    //Another vector of char vectors.
    vector<valtype> altstack;

    //If a return stack is already defined, then clear it out
    if (pvStackRet)
        pvStackRet->clear();


    //while the script iterator beginning at the start hasn't exceeded the iterator at the end
    while (pc < pend)
    {
        //fExec is true if there's no 'false' in vfExec. It will be true if at least one of them is false.
        bool fExec = !count(vfExec.begin(), vfExec.end(), false);

        //SATOSHI_START
        //
        // Read instruction
        //
        //SATOSHI_END

        //Declare variable of enum type 'opcodetype'
        opcodetype opcode;

        //Declare a character array 
        valtype vchPushValue;

        //If opcode isn't able to be successfully read or if the value associated with the op code isn't able to be successfully read
        if (!script.GetOp(pc, opcode, vchPushValue))
            //return false
            return false;
        
        //If none of fExec is false and current opcode is less than or equal to push_data (meaning push data opcode)
        if (fExec && opcode <= OP_PUSHDATA4)
            //push the bytes read from ierator onto stack
            stack.push_back(vchPushValue);

        //if fExec or if the opCode is part of an 'if' construct
        else if (fExec || (OP_IF <= opcode && opcode <= OP_ENDIF))

        //switch-case the opcode
        switch (opcode)
        {
            //SATOSHI_START
            //
            // Push value
            //
            //SATOSHI_END
            case OP_1NEGATE:
            case OP_1:
            case OP_2:
            case OP_3:
            case OP_4:
            case OP_5:
            case OP_6:
            case OP_7:
            case OP_8:
            case OP_9:
            case OP_10:
            case OP_11:
            case OP_12:
            case OP_13:
            case OP_14:
            case OP_15:
            case OP_16:
            {
                //SATOSHI_START
                // ( -- value)
                //SATOSHI_END
                
                //The value of the second part is '80' so we're subracting 80 from the big num for some reason
                CBigNum bn((int)opcode - (int)(OP_1 - 1));

                //push the char vector representing the bignum onto the stack
                stack.push_back(bn.getvch());
            }
            break;


            //SATOSHI_START
            //
            // Control
            //
            //SATOSHI_END

            //In the case of a no-operation, break
            case OP_NOP:
            break;

            //In the case where the op code is OP_Ver
            case OP_VER:
            {
                //Make a big num out of the version 
                CBigNum bn(VERSION);
                //and put it onto the stack as char array
                stack.push_back(bn.getvch());
            }
            break;


            //If regular 'if'-related statement, it checks whether top of stack is true or false and adds result to stack (adding complement for 'NOT'). If ver if-statement, checks if version is greater than or equal to next value read off the stack.
            case OP_IF:
            case OP_NOTIF:
            case OP_VERIF:
            case OP_VERNOTIF:
            {
                //SATOSHI_START
                // <expression> if [statements] [else [statements]] endif
                //SATOSHI_END

                //value is false
                bool fValue = false;

                //if none of fExec evaluated to false
                if (fExec)
                {
                    //If the stack size is less than 1, bailout
                    if (stack.size() < 1)
                        return false;

                    //Get top of stack (last element in stack vector)
                    valtype& vch = stacktop(-1);

                    //if op_code is equal to verIF or the opposite
                    if (opcode == OP_VERIF || opcode == OP_VERNOTIF)
                        //Make fValue equal whether the version is bigger than or equal to the value read off the stack
                        fValue = (CBigNum(VERSION) >= CBigNum(vch));
                    else
                        //Otherwise (if the op code is IF or NOTIF) the value is cast to whatever bool is on the stack
                        fValue = CastToBool(vch);

                    //if the opcode is if notIf or verNotIf
                    if (opcode == OP_NOTIF || opcode == OP_VERNOTIF)
                        //get value complement
                        fValue = !fValue;

                    //remove from stack
                    stack.pop_back();
                }

                //Add the evaulation of this stack operation to vfExec
                vfExec.push_back(fValue);
            }
            break;

            //Changes last execution result to complement
            case OP_ELSE:
            {
                //return if there's nothing previously executed
                if (vfExec.empty())
                    return false;
                
                //Sets the last element to the complement of itself
                vfExec.back() = !vfExec.back();
            }
            break;

            //Removed element from top of execution result
            case OP_ENDIF:
            {
                //If vfExec has previous results
                if (vfExec.empty())
                    return false;
                
                //remove the last result
                vfExec.pop_back();
            }
            break;

            //Checks top of stack to see if it's true, ends script execution if not
            case OP_VERIFY:
            {
                //SATOSHI_START
                // (true -- ) or
                // (false -- false) and return
                //SATOSHI_END

                //Return if stack size less than 1
                if (stack.size() < 1)
                    return false;
                
                //Cast the top of the stack to bool
                bool fValue = CastToBool(stacktop(-1));

                //if top of stakc is true, remove from stack
                if (fValue)
                    stack.pop_back();
                else
                    pc = pend; //otherwise end execution
            }
            break;

            //Ends execution of script
            case OP_RETURN:
            {
                pc = pend;
            }
            break;


            //SATOSHI_START
            //
            // Stack ops
            //
            //SATOSHI_START

            //Move whatever is at the top of stack to altstack
            case OP_TOALTSTACK:
            {
                //if stack size is less than 1, return
                if (stack.size() < 1)
                    return false;

                //Push whatever is in the top of stack to alstack
                altstack.push_back(stacktop(-1));
                //remove from stack
                stack.pop_back();
            }
            break;

            //move whatever is at top of altstack to stack
            case OP_FROMALTSTACK:
            {
                if (altstack.size() < 1)
                    return false;
                
                //copy whatever is at top of altstack to stack
                stack.push_back(altstacktop(-1));
                //delete from altstack
                altstack.pop_back();
            }
            break;

            //Remove 2 elements from stack
            case OP_2DROP:
            {
                //SATOSHI_START
                // (x1 x2 -- )
                //SATOSHI_END
                stack.pop_back();
                stack.pop_back();
            }
            break;

            //Puts copies of the last 2 elements on the stack onto the stack
            case OP_2DUP:
            {
                //SATOSHI_START
                // (x1 x2 -- x1 x2 x1 x2)
                //SATOSHI_END

                
                if (stack.size() < 2)
                    return false;

                //Get 2 elements off the stack
                valtype vch1 = stacktop(-2);
                valtype vch2 = stacktop(-1);

                //Copy them in the same order onto stack
                stack.push_back(vch1);
                stack.push_back(vch2);
            }
            break;

             //Puts copies of the last 3 elements on the stack onto the stack
            case OP_3DUP:
            {
                //SATOSHI_START
                // (x1 x2 x3 -- x1 x2 x3 x1 x2 x3)
                //SATOSHI_ENF
                if (stack.size() < 3)
                    return false;
                valtype vch1 = stacktop(-3);
                valtype vch2 = stacktop(-2);
                valtype vch3 = stacktop(-1);
                stack.push_back(vch1);
                stack.push_back(vch2);
                stack.push_back(vch3);
            }
            break;

            //After adding an offset of 2, puts copies of the last 2 elements on the stack.
            case OP_2OVER:
            {
                //SATOSHI_START
                // (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
                //SATOSHI_END

                if (stack.size() < 4)
                    return false;
                valtype vch1 = stacktop(-4);
                valtype vch2 = stacktop(-3);
                stack.push_back(vch1);
                stack.push_back(vch2);
            }
            break;

            //CHECKPOINT : I get the point so I probably won't be annotating all commands

            case OP_2ROT:
            {
                // (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
                if (stack.size() < 6)
                    return false;
                valtype vch1 = stacktop(-6);
                valtype vch2 = stacktop(-5);
                stack.erase(stack.end()-6, stack.end()-4);
                stack.push_back(vch1);
                stack.push_back(vch2);
            }
            break;

            case OP_2SWAP:
            {
                // (x1 x2 x3 x4 -- x3 x4 x1 x2)
                if (stack.size() < 4)
                    return false;
                swap(stacktop(-4), stacktop(-2));
                swap(stacktop(-3), stacktop(-1));
            }
            break;

            case OP_IFDUP:
            {
                // (x - 0 | x x)
                if (stack.size() < 1)
                    return false;
                valtype vch = stacktop(-1);
                if (CastToBool(vch))
                    stack.push_back(vch);
            }
            break;

            case OP_DEPTH:
            {
                // -- stacksize
                CBigNum bn(stack.size());
                stack.push_back(bn.getvch());
            }
            break;

            case OP_DROP:
            {
                // (x -- )
                if (stack.size() < 1)
                    return false;
                stack.pop_back();
            }
            break;

            //Duplicates top of stack
            case OP_DUP:
            {
                // (x -- x x)
                if (stack.size() < 1)
                    return false;
                valtype vch = stacktop(-1);
                stack.push_back(vch);
            }
            break;

            case OP_NIP:
            {
                // (x1 x2 -- x2)
                if (stack.size() < 2)
                    return false;
                stack.erase(stack.end() - 2);
            }
            break;

            case OP_OVER:
            {
                // (x1 x2 -- x1 x2 x1)
                if (stack.size() < 2)
                    return false;
                valtype vch = stacktop(-2);
                stack.push_back(vch);
            }
            break;

            case OP_PICK:
            case OP_ROLL:
            {
                // (xn ... x2 x1 x0 n - xn ... x2 x1 x0 xn)
                // (xn ... x2 x1 x0 n - ... x2 x1 x0 xn)
                if (stack.size() < 2)
                    return false;
                int n = CBigNum(stacktop(-1)).getint();
                stack.pop_back();
                if (n < 0 || n >= stack.size())
                    return false;
                valtype vch = stacktop(-n-1);
                if (opcode == OP_ROLL)
                    stack.erase(stack.end()-n-1);
                stack.push_back(vch);
            }
            break;

            case OP_ROT:
            {
                // (x1 x2 x3 -- x2 x3 x1)
                //  x2 x1 x3  after first swap
                //  x2 x3 x1  after second swap
                if (stack.size() < 3)
                    return false;
                swap(stacktop(-3), stacktop(-2));
                swap(stacktop(-2), stacktop(-1));
            }
            break;

            case OP_SWAP:
            {
                // (x1 x2 -- x2 x1)
                if (stack.size() < 2)
                    return false;
                swap(stacktop(-2), stacktop(-1));
            }
            break;

            case OP_TUCK:
            {
                // (x1 x2 -- x2 x1 x2)
                if (stack.size() < 2)
                    return false;
                valtype vch = stacktop(-1);
                stack.insert(stack.end()-2, vch);
            }
            break;


            //
            // Splice ops
            //
            case OP_CAT:
            {
                // (x1 x2 -- out)
                if (stack.size() < 2)
                    return false;
                valtype& vch1 = stacktop(-2);
                valtype& vch2 = stacktop(-1);
                vch1.insert(vch1.end(), vch2.begin(), vch2.end());
                stack.pop_back();
            }
            break;

            case OP_SUBSTR:
            {
                // (in begin size -- out)
                if (stack.size() < 3)
                    return false;
                valtype& vch = stacktop(-3);
                int nBegin = CBigNum(stacktop(-2)).getint();
                int nEnd = nBegin + CBigNum(stacktop(-1)).getint();
                if (nBegin < 0 || nEnd < nBegin)
                    return false;
                if (nBegin > vch.size())
                    nBegin = vch.size();
                if (nEnd > vch.size())
                    nEnd = vch.size();
                vch.erase(vch.begin() + nEnd, vch.end());
                vch.erase(vch.begin(), vch.begin() + nBegin);
                stack.pop_back();
                stack.pop_back();
            }
            break;

            case OP_LEFT:
            case OP_RIGHT:
            {
                // (in size -- out)
                if (stack.size() < 2)
                    return false;
                valtype& vch = stacktop(-2);
                int nSize = CBigNum(stacktop(-1)).getint();
                if (nSize < 0)
                    return false;
                if (nSize > vch.size())
                    nSize = vch.size();
                if (opcode == OP_LEFT)
                    vch.erase(vch.begin() + nSize, vch.end());
                else
                    vch.erase(vch.begin(), vch.end() - nSize);
                stack.pop_back();
            }
            break;

            case OP_SIZE:
            {
                // (in -- in size)
                if (stack.size() < 1)
                    return false;
                CBigNum bn(stacktop(-1).size());
                stack.push_back(bn.getvch());
            }
            break;


            //
            // Bitwise logic
            //
            case OP_INVERT:
            {
                // (in - out)
                if (stack.size() < 1)
                    return false;
                valtype& vch = stacktop(-1);
                for (int i = 0; i < vch.size(); i++)
                    vch[i] = ~vch[i];
            }
            break;

            case OP_AND:
            case OP_OR:
            case OP_XOR:
            {
                // (x1 x2 - out)
                if (stack.size() < 2)
                    return false;
                valtype& vch1 = stacktop(-2);
                valtype& vch2 = stacktop(-1);
                MakeSameSize(vch1, vch2);
                if (opcode == OP_AND)
                {
                    for (int i = 0; i < vch1.size(); i++)
                        vch1[i] &= vch2[i];
                }
                else if (opcode == OP_OR)
                {
                    for (int i = 0; i < vch1.size(); i++)
                        vch1[i] |= vch2[i];
                }
                else if (opcode == OP_XOR)
                {
                    for (int i = 0; i < vch1.size(); i++)
                        vch1[i] ^= vch2[i];
                }
                stack.pop_back();
            }
            break;


            //RANDY ANNOTATED
            //Adds different valtypes (vector) on the stack depending on whether last 2 elements were equal. OP_EQUALVERIFY will remove the vector added.
            case OP_EQUAL:
            case OP_EQUALVERIFY:
            //SATOSHI_START
            //case OP_NOTEQUAL: // use OP_NUMNOTEQUAL
            //SATOSHI_END
            {
                //SATOSHI_START
                // (x1 x2 - bool)
                //SATOSHI_END
                if (stack.size() < 2)
                    return false;

                //Gets 2 elements off top of stack
                valtype& vch1 = stacktop(-2);
                valtype& vch2 = stacktop(-1);

                //set fEqqaul to whether 2 top elements are equal
                bool fEqual = (vch1 == vch2);

                //SATOSHI_START
                // OP_NOTEQUAL is disabled because it would be too easy to say
                // something like n != 1 and have some wiseguy pass in 1 with extra
                // zero bytes after it (numerically, 0x01 == 0x0001 == 0x000001)
                //if (opcode == OP_NOTEQUAL)
                //    fEqual = !fEqual;
                //SATOSHI_END

                //Remove 2 elements from stack
                stack.pop_back();
                stack.pop_back();

                //Adds vchTrue (a vector with 2 1s) if true and vchFalse (a vector with 1 0 if false)
                stack.push_back(fEqual ? vchTrue : vchFalse);

                //If EQUALVERIFY and not just EQUAL
                if (opcode == OP_EQUALVERIFY)
                {
                    //remove top of stack if fEqualTrue
                    if (fEqual)
                        stack.pop_back();
                    //Else, terminate program
                    else
                        pc = pend;
                }
            }
            break;


            //
            // Numeric
            //
            case OP_1ADD:
            case OP_1SUB:
            case OP_2MUL:
            case OP_2DIV:
            case OP_NEGATE:
            case OP_ABS:
            case OP_NOT:
            case OP_0NOTEQUAL:
            {
                // (in -- out)
                if (stack.size() < 1)
                    return false;
                CBigNum bn(stacktop(-1));
                switch (opcode)
                {
                case OP_1ADD:       bn += bnOne; break;
                case OP_1SUB:       bn -= bnOne; break;
                case OP_2MUL:       bn <<= 1; break;
                case OP_2DIV:       bn >>= 1; break;
                case OP_NEGATE:     bn = -bn; break;
                case OP_ABS:        if (bn < bnZero) bn = -bn; break;
                case OP_NOT:        bn = (bn == bnZero); break;
                case OP_0NOTEQUAL:  bn = (bn != bnZero); break;
                }
                stack.pop_back();
                stack.push_back(bn.getvch());
            }
            break;

            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD:
            case OP_LSHIFT:
            case OP_RSHIFT:
            case OP_BOOLAND:
            case OP_BOOLOR:
            case OP_NUMEQUAL:
            case OP_NUMEQUALVERIFY:
            case OP_NUMNOTEQUAL:
            case OP_LESSTHAN:
            case OP_GREATERTHAN:
            case OP_LESSTHANOREQUAL:
            case OP_GREATERTHANOREQUAL:
            case OP_MIN:
            case OP_MAX:
            {
                // (x1 x2 -- out)
                if (stack.size() < 2)
                    return false;
                CBigNum bn1(stacktop(-2));
                CBigNum bn2(stacktop(-1));
                CBigNum bn;
                switch (opcode)
                {
                case OP_ADD:
                    bn = bn1 + bn2;
                    break;

                case OP_SUB:
                    bn = bn1 - bn2;
                    break;

                case OP_MUL:
                    if (!BN_mul(&bn, &bn1, &bn2, pctx))
                        return false;
                    break;

                case OP_DIV:
                    if (!BN_div(&bn, NULL, &bn1, &bn2, pctx))
                        return false;
                    break;

                case OP_MOD:
                    if (!BN_mod(&bn, &bn1, &bn2, pctx))
                        return false;
                    break;

                case OP_LSHIFT:
                    if (bn2 < bnZero)
                        return false;
                    bn = bn1 << bn2.getulong();
                    break;

                case OP_RSHIFT:
                    if (bn2 < bnZero)
                        return false;
                    bn = bn1 >> bn2.getulong();
                    break;

                case OP_BOOLAND:             bn = (bn1 != bnZero && bn2 != bnZero); break;
                case OP_BOOLOR:              bn = (bn1 != bnZero || bn2 != bnZero); break;
                case OP_NUMEQUAL:            bn = (bn1 == bn2); break;
                case OP_NUMEQUALVERIFY:      bn = (bn1 == bn2); break;
                case OP_NUMNOTEQUAL:         bn = (bn1 != bn2); break;
                case OP_LESSTHAN:            bn = (bn1 < bn2); break;
                case OP_GREATERTHAN:         bn = (bn1 > bn2); break;
                case OP_LESSTHANOREQUAL:     bn = (bn1 <= bn2); break;
                case OP_GREATERTHANOREQUAL:  bn = (bn1 >= bn2); break;
                case OP_MIN:                 bn = (bn1 < bn2 ? bn1 : bn2); break;
                case OP_MAX:                 bn = (bn1 > bn2 ? bn1 : bn2); break;
                }
                stack.pop_back();
                stack.pop_back();
                stack.push_back(bn.getvch());

                if (opcode == OP_NUMEQUALVERIFY)
                {
                    if (CastToBool(stacktop(-1)))
                        stack.pop_back();
                    else
                        pc = pend;
                }
            }
            break;

            case OP_WITHIN:
            {
                // (x min max -- out)
                if (stack.size() < 3)
                    return false;
                CBigNum bn1(stacktop(-3));
                CBigNum bn2(stacktop(-2));
                CBigNum bn3(stacktop(-1));
                bool fValue = (bn2 <= bn1 && bn1 < bn3);
                stack.pop_back();
                stack.pop_back();
                stack.pop_back();
                stack.push_back(fValue ? vchTrue : vchFalse);
            }
            break;


            //
            // Crypto
            //
            case OP_RIPEMD160:
            case OP_SHA1:
            case OP_SHA256:
            case OP_HASH160:
            case OP_HASH256:
            {
                // (in -- hash)
                if (stack.size() < 1)
                    return false;
                valtype& vch = stacktop(-1);
                valtype vchHash(opcode == OP_RIPEMD160 || opcode == OP_SHA1 || opcode == OP_HASH160 ? 20 : 32);
                if (opcode == OP_RIPEMD160)
                    RIPEMD160(&vch[0], vch.size(), &vchHash[0]);
                else if (opcode == OP_SHA1)
                    SHA1(&vch[0], vch.size(), &vchHash[0]);
                else if (opcode == OP_SHA256)
                    SHA256(&vch[0], vch.size(), &vchHash[0]);
                else if (opcode == OP_HASH160)
                {
                    uint160 hash160 = Hash160(vch);
                    memcpy(&vchHash[0], &hash160, sizeof(hash160));
                }
                else if (opcode == OP_HASH256)
                {
                    uint256 hash = Hash(vch.begin(), vch.end());
                    memcpy(&vchHash[0], &hash, sizeof(hash));
                }
                stack.pop_back();
                stack.push_back(vchHash);
            }
            break;

            //OP_CODESEPARATOR is used to denote the start of a public key script
            case OP_CODESEPARATOR:
            {
                // public key script starts after the code separator
                pbegincodehash = pc;
            }
            break;

            case OP_CHECKSIG:
            case OP_CHECKSIGVERIFY:
            {
                //SATOSHI_START
                // (sig pubkey -- bool)
                //SATOSHI_END
                if (stack.size() < 2)
                    return false;

                //get sig which should be the second from the top
                valtype& vchSig    = stacktop(-2);
                //Get public key which should be on the top
                valtype& vchPubKey = stacktop(-1);

                //SATOSHI_START
                ////// debug print
                //PrintHex(vchSig.begin(), vchSig.end(), "sig: %s\n");
                //PrintHex(vchPubKey.begin(), vchPubKey.end(), "pubkey: %s\n");
                //S_E

                //SATOSHI_START
                // Subset of script starting at the most recent codeseparator
                //SATOSHI_END

                //Make a new script consisting of the start of the code hash to the end
                CScript scriptCode(pbegincodehash, pend);

                //SATOSHI_START
                // Drop the signature, since there's no way for a signature to sign itself
                //SATOSHI_END

                //Delete copies of the signatures from the script just initialized
                scriptCode.FindAndDelete(CScript(vchSig));

                //Checks the signature using
                bool fSuccess = CheckSig(vchSig, vchPubKey, scriptCode, txTo, nIn, nHashType);

                //Removes the signature and the public key from the stack
                stack.pop_back();
                stack.pop_back();

                //Adds a vector representinf result to stack
                stack.push_back(fSuccess ? vchTrue : vchFalse);

                //if it's also 'verify'ing
                if (opcode == OP_CHECKSIGVERIFY)
                {
                    //then remove the result from the stack if successful
                    if (fSuccess)
                        stack.pop_back();
                    //and end execution of the script otherwise
                    else
                        pc = pend;
                }
            }
            break;

            case OP_CHECKMULTISIG:
            case OP_CHECKMULTISIGVERIFY:
            {
                // ([sig ...] num_of_signatures [pubkey ...] num_of_pubkeys -- bool)

                int i = 1;
                if (stack.size() < i)
                    return false;

                int nKeysCount = CBigNum(stacktop(-i)).getint();
                if (nKeysCount < 0)
                    return false;
                int ikey = ++i;
                i += nKeysCount;
                if (stack.size() < i)
                    return false;

                int nSigsCount = CBigNum(stacktop(-i)).getint();
                if (nSigsCount < 0 || nSigsCount > nKeysCount)
                    return false;
                int isig = ++i;
                i += nSigsCount;
                if (stack.size() < i)
                    return false;

                // Subset of script starting at the most recent codeseparator
                CScript scriptCode(pbegincodehash, pend);

                // Drop the signatures, since there's no way for a signature to sign itself
                for (int i = 0; i < nSigsCount; i++)
                {
                    valtype& vchSig = stacktop(-isig-i);
                    scriptCode.FindAndDelete(CScript(vchSig));
                }

                bool fSuccess = true;
                while (fSuccess && nSigsCount > 0)
                {
                    valtype& vchSig    = stacktop(-isig);
                    valtype& vchPubKey = stacktop(-ikey);

                    // Check signature
                    if (CheckSig(vchSig, vchPubKey, scriptCode, txTo, nIn, nHashType))
                    {
                        isig++;
                        nSigsCount--;
                    }
                    ikey++;
                    nKeysCount--;

                    // If there are more signatures left than keys left,
                    // then too many signatures have failed
                    if (nSigsCount > nKeysCount)
                        fSuccess = false;
                }

                while (i-- > 0)
                    stack.pop_back();
                stack.push_back(fSuccess ? vchTrue : vchFalse);

                if (opcode == OP_CHECKMULTISIGVERIFY)
                {
                    if (fSuccess)
                        stack.pop_back();
                    else
                        pc = pend;
                }
            }
            break;

            default:
                return false;
        }
    }


    if (pvStackRet)
        *pvStackRet = stack;
    return (stack.empty() ? false : CastToBool(stack.back()));
}

#undef top









//Format the transaction inputs based on the hash type and returns the hash of the transaction
uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    if (nIn >= txTo.vin.size())
    {
        printf("ERROR: SignatureHash() : nIn=%d out of range\n", nIn);
        return 1;
    }

    //Make a new temporary transaction based on txTo
    CTransaction txTmp(txTo);

    //SATOSHI_START
    // In case concatenating two scripts ends up with two codeseparators,
    // or an extra one at the end, this prevents all those possible incompatibilities.
    //SATOSHI_END

    //Delete any leftover code separators
    scriptCode.FindAndDelete(CScript(OP_CODESEPARATOR));

    //SATOSHI_START
    // Blank out other inputs' signatures
    //SATOSHI_END

    //For all input transactions
    for (int i = 0; i < txTmp.vin.size(); i++)
        //Initialize their 'scriptSig' properties to a new script
        txTmp.vin[i].scriptSig = CScript();

    //For one of the inputs, set the scriptSig equal to the scriptCode in param
    txTmp.vin[nIn].scriptSig = scriptCode;

    //SATOSHI_START
    // Blank out some of the outputs
    //SATOSHI_END

    //If the hashtype is indicating there's no hash
    if ((nHashType & 0x1f) == SIGHASH_NONE)
    {
        //SATOSHI_START
        // Wildcard payee
        //SATPSHI_END

        //The output of the transaction is cleared
        txTmp.vout.clear();

        //S
        // Let the others update at will
        //S_E

        //For all input transaction metadata
        for (int i = 0; i < txTmp.vin.size(); i++)
            //if the index isn't equal to the index we're processing
            if (i != nIn)
                //Set the 'nSequence' to 0
                txTmp.vin[i].nSequence = 0;
    }
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        //SATOSHI_START
        // Only lockin the txout payee at same index as txin
        //SATOSHI_START

        //Make nOut equal nIn
        unsigned int nOut = nIn;

        //If nOut is out of bounds of the output size
        if (nOut >= txTmp.vout.size())
        {
            //log and return
            printf("ERROR: SignatureHash() : nOut=%d out of range\n", nOut);
            return 1;
        }

        //resize the output to equal input index (not sure why '+1')
        txTmp.vout.resize(nOut+1);

        //For all outputs indexes lower than nOut (which is same as 'nIn')
        for (int i = 0; i < nOut; i++)
            //Set the output to null
            txTmp.vout[i].SetNull();

        //SATOSHI_START
        // Let the others update at will
        //SATOSHI_START

        //For every input
        for (int i = 0; i < txTmp.vin.size(); i++)
            //If the input isn't rqual to 'nIn'
            if (i != nIn)
                //Set 'nSequence' to 0
                txTmp.vin[i].nSequence = 0;
    }

    //SATOSHI_START
    // Blank out other inputs completely, not recommended for open transactions
    //SATOSHI_START

    //If the hashType
    if (nHashType & SIGHASH_ANYONECANPAY)
    {
        //Make the first input of the transaction equal to the input with index passed in as parameter
        txTmp.vin[0] = txTmp.vin[nIn];

        //resize the input to make it the only input.
        txTmp.vin.resize(1);
    }

    //S
    // Serialize and hash
    //S_E 

    //Create data stream
    CDataStream ss(SER_GETHASH);

    //Reserve 10k bytes
    ss.reserve(10000);

    //Add the transaction with hashtype to buffer
    ss << txTmp << nHashType;

    //Hash and return the buffer
    return Hash(ss.begin(), ss.end());
}

//Called from EvalScript, this function verifies that the signature for an input transaction on txTo (vchSig) matches that of a recomputed signature using scriptCode, the transaction, and the public key
bool CheckSig(vector<unsigned char> vchSig, vector<unsigned char> vchPubKey, CScript scriptCode,
              const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    //Initialize the key to the vchPubKey
    CKey key;
    if (!key.SetPubKey(vchPubKey))
        return false;

    //SATOSHI_START
    // Hash type is one byte tacked on to the end of the signature
    //SATOSHI_END

    //Make sure sig isn't empty because we're checking sig here
    if (vchSig.empty())
        return false;
    
    //If hashType is 0 (unitialized)
    if (nHashType == 0)
        //Get the hash type from the last byte of the signature 
        nHashType = vchSig.back();
        
    //If the hashType entered in does not equal the hashType in the signature, return false
    //This means the hash type parameter is just a validation mechanisms
    else if (nHashType != vchSig.back())
        return false;
    
    //remove the last byte of the sig (the byte indicating the hash type)
    vchSig.pop_back();

    //verify the signature on the transaction with the supplied public key by decrypting the  signature using the public key, and recomputing the transaction hash to make sure it matches with decrypted sig.
    //scriptCode is the original public key on the input transaction
    if (key.Verify(SignatureHash(scriptCode, txTo, nIn, nHashType), vchSig))
        return true;

    return false;
}










//Tries to match the template used for the scriptPubKey and tries to parse script values into vSolutionRet
//Returns false if none of the templates were able to be read
bool Solver(const CScript& scriptPubKey, vector<pair<opcodetype, valtype> >& vSolutionRet)
{
    // Initialize static vector of scripts called templates
    static vector<CScript> vTemplates;

    //If the templates are empty (since vTemplates is static, that means it's the first time entering into the method)
    //Script 2 has placeholders for things like OP_PUBKEY that should hold a different value in the actual transaction
    if (vTemplates.empty())
    {
        //S
        // Standard tx, sender provides pubkey, receiver adds signature
        //

        //https://stackoverflow.com/questions/2722879/calling-constructors-in-c-without-new
        //Adds a scroipt containing something denoting a public key and a check sig operation
        //This template is used when sending money to oneself or when creating a coinbase transaction in BitcoinMiner
        vTemplates.push_back(CScript() << OP_PUBKEY << OP_CHECKSIG);

        //S
        // Short account number tx, sender provides hash of pubkey, receiver provides signature and pubkey
        //S_E

        //Adds a duplicate command to the template and hashes and compares values.
        //This template follows the one that's added to a transaction on transfer in ui.cpp
        //This trmplate is used for the public key when the transaction output is going to oneself (so when making a second cashback payment to oneself and when generating coinbase transaction)
        vTemplates.push_back(CScript() << OP_DUP << OP_HASH160 << OP_PUBKEYHASH << OP_EQUALVERIFY << OP_CHECKSIG);
    }

    //S
    // Scan templates
    //S_E

    //Save the public key into script1
    const CScript& script1 = scriptPubKey;

    //For each script in vTemplates
    foreach(const CScript& script2, vTemplates)
    {
        //clear vSolutionRet
        vSolutionRet.clear();

        //Declare 2 opcodes
        opcodetype opcode1, opcode2;

        //Declare 2 char vectors
        vector<unsigned char> vch1, vch2;

        //S
        // Compare
        //S_END

        //Make a script iterator for the public key
        CScript::const_iterator pc1 = script1.begin();

        //Make a script iterator for the template script being processed
        CScript::const_iterator pc2 = script2.begin();

        //endlessly loop
        loop
        {
            //Get the operation at the public key script
            bool f1 = script1.GetOp(pc1, opcode1, vch1);

            //Get the operation at the template
            bool f2 = script2.GetOp(pc2, opcode2, vch2);

            //If there was an operation read failure for both scripts
            if (!f1 && !f2)
            {
                //SATOSHI_START
                // Success
                //SATOSHI_END

                //Revers the solution vector to be returned
                reverse(vSolutionRet.begin(), vSolutionRet.end());

                //And return true
                return true;
            }

            //If one could be read and not the other, break
            else if (f1 != f2)
            {
                break;
            }

            //If both could be read and the operation code read off the template script is OP_PUBKEY
            else if (opcode2 == OP_PUBKEY)
            {
                //Make sure that whatever was read of the pubkey script is less than or equal to an unisgned 256 bit int
                if (vch1.size() <= sizeof(uint256))
                    break;

                //Add the (OP_PUBKEY operation code, pub key script) pair to the solution vector
                //push opcode2 and not 1
                vSolutionRet.push_back(make_pair(opcode2, vch1));
            }

            //If the operation from the template is OP_PUBKEYHAS
            else if (opcode2 == OP_PUBKEYHASH)
            {
                //The size read in from pubkey vector shouldn't exceed a 160 bit unsigned int
                if (vch1.size() != sizeof(uint160))
                    break;

                //Add the opcode and pub script key val
                vSolutionRet.push_back(make_pair(opcode2, vch1));
            }

            //If the op code of the template wasn't any of the above and it differed from the one read from pubkey script, then break
            else if (opcode1 != opcode2)
            {
                break;
            }
        }
    }

    //Clear the solution return vector
    vSolutionRet.clear();
    return false;
}


//Informal Name; Signer
//This function will sign the message digest hash and place the signature in scriptSigRet. The pubkey used to sign may be appended to the 'scriptSigRet' signature vector based on the operations read from vSolution (populated by the other 'Solver' function.)
//When creating a transaction the hash and nHashType are both 0
//Returns false if public key passed in is not found in our in-memory mapKeys
bool Solver(const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet)
{
    //clear the last parameter's CScript
    scriptSigRet.clear();

    //create a vector of (opcodetype, valtype) pairs called 'vSolution'
    vector<pair<opcodetype, valtype> > vSolution;

    //if the scriptPubKeyParameter and the empty solution vector returns false
    if (!Solver(scriptPubKey, vSolution))
        //bailout
        return false;

    //SATOSHI_START
    // Compile solution
    //SATOSHI_END

    //lock mapKeys
    CRITICAL_BLOCK(cs_mapKeys)
    {
        //for each pair in the computed solution
        foreach(PAIRTYPE(opcodetype, valtype)& item, vSolution)
        {
            //if the opcodetype is equal to op_pubkey enum value
            if (item.first == OP_PUBKEY)
            {
                //SATOSHI_START
                // Sign
                //SATOSHI_END
                //take value of valtype in solution pair (where valtype is a typedef for char vector)
                const valtype& vchPubKey = item.second;

                //See if the key in the vchPubKey vector is in mapKeys
                if (!mapKeys.count(vchPubKey))
                    //error out if the key isn't present
                    return false;

                //if the hash in the parameter is non-zero
                if (hash != 0)
                {
                    //Initialize new char vector (don't know why didn't use valtype)
                    vector<unsigned char> vchSig;

                    //Call the static Sign method on CKey with the key as the first parameter (mapKeys[pubkey] == privateKey), the hash as the seconds, and the presumed output as the just initialized vchSig
                    if (!CKey::Sign(mapKeys[vchPubKey], hash, vchSig))
                        //return false if errored out
                        return false;
                    //Add nhashType to the signature vector
                    vchSig.push_back((unsigned char)nHashType);

                    //Add the signature vector to scriptSigRet, the last parameter passed in that's probably intended to hold the return value
                    scriptSigRet << vchSig;
                }
            }

            //if the optype is a public key hash
            else if (item.first == OP_PUBKEYHASH)
            {
                //SATOSHI_START
                // Sign and give pubkey
                //SATOSHI_END

                //Use the public key hash from valtype of solution pair to find real key
                map<uint160, valtype>::iterator mi = mapPubKeys.find(uint160(item.second));

                //if not found
                if (mi == mapPubKeys.end())
                    //return false
                    return false;
                
                //Extract the public key from the item located using pubkey hash
                const vector<unsigned char>& vchPubKey = (*mi).second;

                //if mapKeys doesn't have he public key
                if (!mapKeys.count(vchPubKey))
                    //bailout
                    return false;

                //If hash is non-zero
                if (hash != 0)
                {
                    //Make signature container
                    vector<unsigned char> vchSig;

                    //Sign hash using public key
                    if (!CKey::Sign(mapKeys[vchPubKey], hash, vchSig))
                        //if not, bail
                        return false;

                    //push the hash type into vchSig
                    vchSig.push_back((unsigned char)nHashType);
                    
                    //Build presumed return script by assemling the signature and the public key used
                    scriptSigRet << vchSig << vchPubKey;
                }
            }
        }
    }

    return true;
}


//Returns true if the public key used in the transaction is in mapKeys
bool IsMine(const CScript& scriptPubKey)
{
    //Declares a new scriptSig that will be discarded
    //Created just to match Solver interface, the scriptPubKey value is all that's important here
    CScript scriptSig;
    return Solver(scriptPubKey, 0, 0, scriptSig);
}

//Tries to extract the public key from scriptPubKey using the 'Solver' function
bool ExtractPubKey(const CScript& scriptPubKey, bool fMineOnly, vector<unsigned char>& vchPubKeyRet)
{

    //Clear the pub key vector slated for return
    vchPubKeyRet.clear();

    //Create vSolution to hold the public key (op-code, val) pair
    vector<pair<opcodetype, valtype> > vSolution;
    //Reads the public key as an (opcode, val) pair into vSolution from the vector of chars in scriptPubKey
    if (!Solver(scriptPubKey, vSolution))
        return false;

    //Lock the in-memory key store
    CRITICAL_BLOCK(cs_mapKeys)
    {
        //For each parsed instruction-val pair (only 2 types of public key formats exist)
        foreach(PAIRTYPE(opcodetype, valtype)& item, vSolution)
        {
            //Make a char array for the pubKey
            valtype vchPubKey;

            //If the first instruction is the pub key
            if (item.first == OP_PUBKEY)
            {
                //Set the pub key array val
                vchPubKey = item.second;
            }

            //If the first instruction is op_pubkeyhash
            else if (item.first == OP_PUBKEYHASH)
            {
                //Find the pubKey using the hash
                map<uint160, valtype>::iterator mi = mapPubKeys.find(uint160(item.second));

                //continue if not found
                if (mi == mapPubKeys.end())
                    continue;

                //And put it in vchPubKey if found
                vchPubKey = (*mi).second;
            }

            //If our goal isn't to mine only or we have the private key for this public key
            if (!fMineOnly || mapKeys.count(vchPubKey))
            {
                //return the public key
                vchPubKeyRet = vchPubKey;
                return true;
            }
        }
    }
    return false;
}


bool ExtractHash160(const CScript& scriptPubKey, uint160& hash160Ret)
{
    hash160Ret = 0;

    vector<pair<opcodetype, valtype> > vSolution;
    if (!Solver(scriptPubKey, vSolution))
        return false;

    foreach(PAIRTYPE(opcodetype, valtype)& item, vSolution)
    {
        if (item.first == OP_PUBKEYHASH)
        {
            hash160Ret = uint160(item.second);
            return true;
        }
    }
    return false;
}


bool SignSignature(const CTransaction& txFrom, CTransaction& txTo, unsigned int nIn, int nHashType, CScript scriptPrereq)
{
    //assert that nIn is less than input transactions
    assert(nIn < txTo.vin.size());

    //The input transaction reference is set to the current transactions input
    CTxIn& txin = txTo.vin[nIn];

    //Asser that the incoming transaction reference on our new transaction leads to a valid index
    assert(txin.prevout.n < txFrom.vout.size());

    //The transaction output of the incoming transaction is extracted
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    //SATOSHI_START
    // Leave out the signature from the hash, since a signature can't sign itself.
    // The checksig op will also drop the signatures from its hash.
    //SATOSHI_END

    //Make a hash using an empty scriptPreReq and our default script pubKey (also passing in our newly made transaction)
    //Does scriptPubKey change here?
    //txout here is the output of a previous transaction. So scriptPubKey should have been processed and is not the default
    //hash is the hash of txTo with sig on the vin from txout.scriptPubKey
    uint256 hash = SignatureHash(scriptPrereq + txout.scriptPubKey, txTo, nIn, nHashType);

    //The input transaction's public key (taken from it's txout) is placed in the solver along with a reference to the signature we want to add to txin (the metadata we have for the input transaction on our current transaction)
    //This populates txin.scriptSig by using the scriptPubKey (which should be our public key in the input transaction's output) to get our private key and make the signature to say we're using that transaction in our new input
    if (!Solver(txout.scriptPubKey, hash, nHashType, txin.scriptSig))
        return false;

    txin.scriptSig = scriptPrereq + txin.scriptSig;

    //SATOSHI_START
    // Test solution
    //SATOSHI_END
    //scriptPubKey is usally in format: HASH OP_CHECKSIG
    if (scriptPrereq.empty())
        if (!EvalScript(txin.scriptSig + CScript(OP_CODESEPARATOR) + txout.scriptPubKey, txTo, nIn))
            return false;

    return true;
}

//Performs a basic validation and calls 'EvalScript' to verify the sig
//This is called on a per-input basis
bool VerifySignature(const CTransaction& txFrom, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    //Assert the input transaction is not bigger than size of all input transactions
    assert(nIn < txTo.vin.size());

    //Retrieve a transaction that's reflectant to the current input being processed
    const CTxIn& txin = txTo.vin[nIn];

    //Validate that input metadata lines up with current way things are in transactions
    if (txin.prevout.n >= txFrom.vout.size())
        return false;

    //make an output from the input transaction
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    //Validate hash from input transaction is equal to the hash of the source transacrtion
    if (txin.prevout.hash != txFrom.GetHash())
        return false;

    return EvalScript(txin.scriptSig + CScript(OP_CODESEPARATOR) + txout.scriptPubKey, txTo, nIn, nHashType);
}
