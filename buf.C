#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <climits>
#include <iostream>

// clang-format off
#include "page.h"
#include "buf.h"
// clang-format on

#define ASSERT(c)                                                                                                      \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            cerr << "At line " << __LINE__ << ":" << endl << "  ";                                                     \
            cerr << "This condition should hold: " #c << endl;                                                         \
            exit(1);                                                                                                   \
        }                                                                                                              \
    }

#define FILECASTHACK(f) (File*)((long)(f) % INT_MAX)

//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(const int bufs)
{
    numBufs = bufs;

    bufTable = new BufDesc[bufs];
    memset(bufTable, 0, bufs * sizeof(BufDesc));
    for (int i = 0; i < bufs; i++)
    {
        bufTable[i].frameNo = i;
        bufTable[i].valid = false;
    }

    bufPool = new Page[bufs];
    memset(bufPool, 0, bufs * sizeof(Page));

    int htsize = ((((int)(bufs * 1.2)) * 2) / 2) + 1;
    hashTable = new BufHashTbl(htsize); // allocate the buffer hash table

    clockHand = bufs - 1;
}

BufMgr::~BufMgr()
{
    // flush out all unwritten pages
    for (int i = 0; i < numBufs; i++)
    {
        BufDesc* tmpbuf = &bufTable[i];
        if (tmpbuf->valid == true && tmpbuf->dirty == true)
        {
#ifdef DEBUGBUF
            cout << "flushing page " << tmpbuf->pageNo << " from frame " << i << endl;
#endif

            tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]));
        }
    }

    delete[] bufTable;
    delete[] bufPool;
}

const Status BufMgr::allocBuf(int& frame)
{
    int steps = 0;
    // prerequisite: clockHand points to the most recently allocated frameNo
    do
    {
        advanceClock();
        ++steps;
        BufDesc* bufDesc = &bufTable[clockHand];
        if (!bufDesc->valid) // The frame is not valid (empty) break and return
        {
            break;
        }
        if (bufDesc->refbit) // The frame is recently used, continue and check the next one
        {
            bufDesc->refbit = false;
            continue;
        }
        if (bufDesc->pinCnt) // The frame is pin, continue and check the next one
        {
            continue;      
        }
        // When the control flow flows here, this frame is not pin, not recently used, and not empty, evict this page
        ASSERT(hashTable->remove(FILECASTHACK(bufDesc->file), bufDesc->pageNo) == OK);
        if (bufDesc->dirty) // Write to disk if it's dirty
        {
            ASSERT(bufDesc->file->writePage(bufDesc->pageNo, &bufPool[clockHand]) == OK);
        }
        break;
    } while (steps != numBufs * 2); // loop until we circle the clock twice

    // If we circle the clock twice, all the frames are PIN and we have to return BUFFEREXCEEDED
    if (steps == numBufs * 2){
        return BUFFEREXCEEDED;
    }

    frame = clockHand;
    // Reset the frame before returning
    bufTable[frame].Clear();
    return OK;
}

const Status BufMgr::readPage(File* file, const int PageNo, Page*& page)
{
    Status status = OK;
    int frameNo = 0;

    status = hashTable->lookup(FILECASTHACK(file), PageNo, frameNo);
    if (status == HASHNOTFOUND) // check if page is in buffer pool
    {
        if (allocBuf(frameNo) == BUFFEREXCEEDED) // allocate buffer frame
        {
            return BUFFEREXCEEDED;
        }
        if (file->readPage(PageNo, &bufPool[frameNo]) == UNIXERR) // read page from disk into buffer pool frame
        {
            return UNIXERR;
        }
        if (hashTable->insert(FILECASTHACK(file), PageNo, frameNo) == HASHTBLERROR) // insert page into hashtable
        {
            return HASHTBLERROR;
        }
        bufTable[frameNo].Set(file,
                              PageNo); // invoke Set() to set up frame properly
    }
    if (status == OK) // page is in buffer pool
    {
        // set refbit, increment pinCnt
        bufTable[frameNo].refbit = true;
        bufTable[frameNo].pinCnt++;
    }
    // return a pointer to frame containing page via page parameter
    page = &bufPool[frameNo];
    return OK;
}

const Status BufMgr::unPinPage(File* file, const int PageNo, const bool dirty)
{
    int frameNo = 0;

    // use hashtable to look for the frame number
    if (hashTable->lookup(FILECASTHACK(file), PageNo, frameNo) != OK)
    {
        return HASHNOTFOUND;
    }

    // check the pinCount of the frame
    if (bufTable[frameNo].pinCnt == 0)
    {
        return PAGENOTPINNED;
    }

    bufTable[frameNo].pinCnt -= 1;

    // set dirty bit
    if (dirty)
    {
        bufTable[frameNo].dirty = dirty;
    }
    return OK;
}

const Status BufMgr::allocPage(File* file, int& pageNo, Page*& page)
{
    int frameNo = 0;

    // create new page
    if (file->allocatePage(pageNo) != OK)
    {
        return UNIXERR;
    }

    // allocate buffer frame
    if (allocBuf(frameNo) == BUFFEREXCEEDED)
    {
        return BUFFEREXCEEDED;
    }

    // insert file information into hashtable
    if (hashTable->insert(FILECASTHACK(file), pageNo, frameNo) == HASHTBLERROR)
    {
        return HASHTBLERROR;
    }

    // update information into the buftable
    bufTable[frameNo].Set(file, pageNo);

    // return a pointer to the buffer frame
    page = &bufPool[frameNo];

    return OK;
}

const Status BufMgr::disposePage(File* file, const int pageNo)
{
    // see if it is in the buffer pool
    Status status = OK;
    int frameNo = 0;
    status = hashTable->lookup(FILECASTHACK(file), pageNo, frameNo);
    if (status == OK)
    {
        // clear the page
        bufTable[frameNo].Clear();
    }
    status = hashTable->remove(FILECASTHACK(file), pageNo);

    // deallocate it in the file
    return file->disposePage(pageNo);
}

const Status BufMgr::flushFile(const File* file)
{
    Status status;

    for (int i = 0; i < numBufs; i++)
    {
        BufDesc* tmpbuf = &(bufTable[i]);
        if (tmpbuf->valid == true && tmpbuf->file == file)
        {
            if (tmpbuf->pinCnt > 0)
                return PAGEPINNED;

            if (tmpbuf->dirty == true)
            {
#ifdef DEBUGBUF
                cout << "flushing page " << tmpbuf->pageNo << " from frame " << i << endl;
#endif
                if ((status = tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]))) != OK)
                    return status;

                tmpbuf->dirty = false;
            }

            hashTable->remove(FILECASTHACK(file), tmpbuf->pageNo);

            tmpbuf->file = NULL;
            tmpbuf->pageNo = -1;
            tmpbuf->valid = false;
        }

        else if (tmpbuf->valid == false && tmpbuf->file == file)
            return BADBUFFER;
    }

    return OK;
}

void BufMgr::printSelf(void)
{
    BufDesc* tmpbuf;

    cout << endl << "Print buffer...\n";
    for (int i = 0; i < numBufs; i++)
    {
        tmpbuf = &(bufTable[i]);
        cout << i << "\t" << (char*)(&bufPool[i]) << "\tpinCnt: " << tmpbuf->pinCnt;

        if (tmpbuf->valid == true)
            cout << "\tvalid\n";
        cout << endl;
    };
}
