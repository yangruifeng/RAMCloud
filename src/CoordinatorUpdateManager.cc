/* Copyright (c) 2013 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "CoordinatorUpdateInfo.pb.h"
#include "CoordinatorUpdateManager.h"

namespace RAMCloud {

/**
 * Construct a CoordinatorUpdateManager object
 *
 * \param storage
 *      Used to read and write the configuration information from/to an
 *      external system.
 */
CoordinatorUpdateManager::CoordinatorUpdateManager(ExternalStorage* storage)
    : mutex()
    , storage(storage)
    , activeUpdates()
    , smallestUnfinished(0)
    , lastAssigned(0)
    , externalLastFinished(0)
    , externalFirstAvailable(0)
{}

/**
 * Destructor for MockExternalStorage objects.
 */
CoordinatorUpdateManager::~CoordinatorUpdateManager()
{}

/**
 * This method is called once, typically right after a new coordinator
 * has assumed leadership. It reads the CoordinatorUpdateInfo record stored
 * externally and uses it to initialize the information in this object.
 * If the record doesn't exist, the object is initialize assuming we are
 * starting a new cluster from scratch. this method must be called before
 * either nextSequenceNumber or updateFinished is called.
 * 
 * \return
 *      The return value is the sequence number of the last update made
 *      by previous coordinators that is known to have completed successfully.
 *      Any updates more recent than this may still be in complete, and
 *      hence must be recovered.
 */
uint64_t
CoordinatorUpdateManager::init()
{
    Lock lock(mutex);
    Buffer externalData;
    if (!storage->get("/coordinatorUpdateInfo", &externalData)) {
        RAMCLOUD_LOG(WARNING, "couldn't find \"coordinatorUpdateInfo\" object "
                "in external storage; starting new cluster from scratch");
        externalLastFinished = 0;
        externalFirstAvailable = 1;
    } else {
        ProtoBuf::CoordinatorUpdateInfo info;
        uint32_t length = externalData.getTotalLength();
        string str(static_cast<const char*>(externalData.getRange(0, length)),
                length);
        if (!info.ParseFromString(str)) {
            throw FatalError(HERE, "format error in \"coordinatorUpdateInfo\" "
                "object in external storage");
        }
        externalLastFinished = info.lastfinished();
        externalFirstAvailable = info.firstavailable();
        RAMCLOUD_LOG(NOTICE, "initializing CoordinatorUpdateManager: "
                "lastFinished = %lu, firstAvailable = %lu",
                externalLastFinished, externalFirstAvailable);
    }
    smallestUnfinished = externalFirstAvailable;
    lastAssigned = smallestUnfinished - 1;
    return externalLastFinished;
}

/**
 * Returns a unique sequence number that the caller can associate with
 * an update. Sequence numbers are assigned using consecutive integer
 * values.
 */
uint64_t
CoordinatorUpdateManager::nextSequenceNumber()
{
    Lock lock(mutex);
    lastAssigned++;
    if (lastAssigned >= externalFirstAvailable) {
        // We have used up all of the sequence numbers that we have
        // "reserved"; update reservation, so that a future coordinator
        // won't reuse anything that we have already used.
        sync(lock);
    }
    activeUpdates.emplace_back(false);
    return lastAssigned;
}

/**
 * This method is invoked to indicated that a particular update has
 * completed successfully.
 *
 * \param sequenceNumber
 *      Identifies the operation that completed; must have been the return
 *      value from a previous call to nextSequenceNumber.
 */
void
CoordinatorUpdateManager::updateFinished(uint64_t sequenceNumber)
{
    Lock lock(mutex);
    assert(sequenceNumber >= smallestUnfinished);
    assert(sequenceNumber <= lastAssigned);

    activeUpdates[sequenceNumber - smallestUnfinished] = true;

    // If the oldest update has now completed, flush it and the ones
    // after it, until we reach an update that hasn't yet finished.
    while (!activeUpdates.empty() && activeUpdates.front()) {
        activeUpdates.pop_front();
        smallestUnfinished++;
    }

    // If the "last finished" information on external storage is way
    // out of date, update it (it doesn't matter if it's a bit out of date,
    // but if it gets too old, it will lengthen coordinator recovery time
    // by increasing the number of updates that a new coordinator has to
    // retry).
    if (smallestUnfinished > (externalLastFinished + 100)) {
        sync(lock);
    }
}

/**
 * Update the CoordinatorUpdateInfo record stored externally to reflect
 * the current state.
 *
 * \param lock
 *      Ensures that caller has acquired mutex; not actually used here.
 */
void
CoordinatorUpdateManager::sync(Lock& lock)
{
    ProtoBuf::CoordinatorUpdateInfo info;
    externalLastFinished = smallestUnfinished-1;
    info.set_lastfinished(externalLastFinished);

    // Reserve the next 1000 sequence numbers (i.e. if we crash, the next
    // coordinator will not use any of these).
    externalFirstAvailable = lastAssigned + 1000;
    info.set_firstavailable(externalFirstAvailable);

    string str;
    info.SerializeToString(&str);
    storage->set(ExternalStorage::UPDATE, "/coordinatorUpdateInfo", str.c_str(),
            downCast<int>(str.length()));
}

} // namespace RAMCloud
