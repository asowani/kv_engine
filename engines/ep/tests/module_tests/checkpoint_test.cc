/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include <algorithm>
#include <set>
#include <thread>
#include <vector>

#include "checkpoint.h"
#include "configuration.h"
#include "failover-table.h"
#include "item_pager.h"
#include "stats.h"
#include "tests/module_tests/test_helpers.h"
#include "thread_gate.h"
#include "ep_vb.h"

#include <engines/ep/src/ep_types.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <valgrind/valgrind.h>

#define NUM_DCP_THREADS 3
#define NUM_DCP_THREADS_VG 2
#define NUM_SET_THREADS 4
#define NUM_SET_THREADS_VG 2

#define NUM_ITEMS 500
#define NUM_ITEMS_VG 10

#define DCP_CURSOR_PREFIX "dcp-client-"

/**
 * Dummy callback to replace the flusher callback.
 */
class DummyCB: public Callback<uint16_t> {
public:
    DummyCB() {}

    void callback(uint16_t &dummy) {
        (void) dummy;
    }
};

/**
 * Test fixture for Checkpoint tests. Once constructed provides a checkpoint
 * manager and single vBucket (VBID 0).
 *
 *@tparam V The VBucket class to use for the vbucket object.
 */
template <typename V>
class CheckpointTest : public ::testing::Test {
protected:
    CheckpointTest()
        : callback(new DummyCB()),
          vbucket(new V(0,
                        vbucket_state_active,
                        global_stats,
                        checkpoint_config,
                        /*kvshard*/ NULL,
                        /*lastSeqno*/ 1000,
                        /*lastSnapStart*/ 0,
                        /*lastSnapEnd*/ 0,
                        /*table*/ NULL,
                        callback,
                        /*newSeqnoCb*/ nullptr,
                        config,
                        item_eviction_policy_t::VALUE_ONLY)) {
        createManager();
    }

    void createManager(int64_t last_seqno = 1000) {
        manager.reset(new CheckpointManager(global_stats,
                                            this->vbucket->getId(),
                                            checkpoint_config,
                                            last_seqno,
                                            /*lastSnapStart*/ 0,
                                            /*lastSnapEnd*/ 0,
                                            callback));

        // Sanity check initial state.
        ASSERT_EQ(1, this->manager->getNumOfCursors());
        ASSERT_EQ(0, this->manager->getNumOpenChkItems());
        ASSERT_EQ(1, this->manager->getNumCheckpoints());
    }

    // Creates a new item with the given key and queues it into the checkpoint
    // manager.
    bool queueNewItem(const std::string& key) {
        queued_item qi{new Item(makeStoredDocKey(key),
                                this->vbucket->getId(),
                                queue_op::mutation,
                                /*revSeq*/ 0,
                                /*bySeq*/ 0)};
        return this->manager->queueDirty(*this->vbucket,
                                         qi,
                                         GenerateBySeqno::Yes,
                                         GenerateCas::Yes,
                                         /*preLinkDocCtx*/ nullptr);
    }


    EPStats global_stats;
    CheckpointConfig checkpoint_config;
    Configuration config;
    std::shared_ptr<Callback<uint16_t> > callback;
    std::unique_ptr<V> vbucket;
    std::unique_ptr<CheckpointManager> manager;
};


struct thread_args {
    VBucket* vbucket;
    CheckpointManager *checkpoint_manager;
    std::string name;
    ThreadGate& gate;
};

static void launch_persistence_thread(void *arg) {
    struct thread_args *args = static_cast<struct thread_args *>(arg);
    args->gate.threadUp();

    bool flush = false;
    while(true) {
        size_t itemPos;
        std::vector<queued_item> items;
        const std::string cursor(CheckpointManager::pCursorName);
        args->checkpoint_manager->getAllItemsForCursor(cursor, items);
        for(itemPos = 0; itemPos < items.size(); ++itemPos) {
            queued_item qi = items.at(itemPos);
            if (qi->getOperation() == queue_op::flush) {
                flush = true;
                break;
            }
        }
        if (flush) {
            // Checkpoint start and end operations may have been introduced in
            // the items queue after the "flush" operation was added. Ignore
            // these. Anything else will be considered an error.
            for(size_t i = itemPos + 1; i < items.size(); ++i) {
                queued_item qi = items.at(i);
                EXPECT_TRUE(queue_op::checkpoint_start == qi->getOperation() ||
                            queue_op::checkpoint_end == qi->getOperation())
                    << "Unexpected operation:" << to_string(qi->getOperation());
            }
            break;
        }
        // yield to allow set thread to actually do some useful work.
        std::this_thread::yield();
    }
    EXPECT_TRUE(flush);
}

static void launch_dcp_client_thread(void* arg) {
    struct thread_args *args = static_cast<struct thread_args *>(arg);
    args->gate.threadUp();

    bool flush = false;
    bool isLastItem = false;
    while(true) {
        queued_item qi = args->checkpoint_manager->nextItem(args->name,
                                                            isLastItem);
        if (qi->getOperation() == queue_op::flush) {
            flush = true;
            break;
        }
        // yield to allow set thread to actually do some useful work.
        std::this_thread::yield();
    }
    EXPECT_TRUE(flush);
}

static void launch_checkpoint_cleanup_thread(void *arg) {
    struct thread_args *args = static_cast<struct thread_args *>(arg);
    args->gate.threadUp();

    while (args->checkpoint_manager->getNumOfCursors() > 1) {
        bool newCheckpointCreated;
        args->checkpoint_manager->removeClosedUnrefCheckpoints(
                *args->vbucket, newCheckpointCreated);
        // yield to allow set thread to actually do some useful work.
        std::this_thread::yield();
    }
}

static void launch_set_thread(void *arg) {
    struct thread_args *args = static_cast<struct thread_args *>(arg);
    args->gate.threadUp();

    int i(0);
    for (i = 0; i < NUM_ITEMS; ++i) {
        std::string key = "key-" + std::to_string(i);
        queued_item qi(new Item(makeStoredDocKey(key),
                                args->vbucket->getId(),
                                queue_op::mutation,
                                0,
                                0));
        args->checkpoint_manager->queueDirty(*args->vbucket,
                                             qi,
                                             GenerateBySeqno::Yes,
                                             GenerateCas::Yes,
                                             /*preLinkDocCtx*/ nullptr);
    }
}

typedef ::testing::Types<EPVBucket> VBucketTypes;
TYPED_TEST_CASE(CheckpointTest, VBucketTypes);

TYPED_TEST(CheckpointTest, basic_chk_test) {
    std::shared_ptr<Callback<uint16_t> > cb(new DummyCB());
    this->vbucket.reset(new TypeParam(0,
                                      vbucket_state_active,
                                      this->global_stats,
                                      this->checkpoint_config,
                                      NULL,
                                      0,
                                      0,
                                      0,
                                      NULL,
                                      cb,
                                      /*newSeqnoCb*/ nullptr,
                                      this->config,
                                      item_eviction_policy_t::VALUE_ONLY));

    this->manager.reset(new CheckpointManager(
            this->global_stats, 0, this->checkpoint_config, 1, 0, 0, cb));

    const size_t n_set_threads = RUNNING_ON_VALGRIND ? NUM_SET_THREADS_VG :
                                                       NUM_SET_THREADS;

    const size_t n_dcp_threads =
            RUNNING_ON_VALGRIND ? NUM_DCP_THREADS_VG : NUM_DCP_THREADS;

    std::vector<cb_thread_t> dcp_threads(n_dcp_threads);
    std::vector<cb_thread_t> set_threads(n_set_threads);
    cb_thread_t persistence_thread;
    cb_thread_t checkpoint_cleanup_thread;
    int rc(0);

    const size_t n_threads{n_set_threads + n_dcp_threads + 2};
    ThreadGate gate{n_threads};
    thread_args t_args{this->vbucket.get(), this->manager.get(), {}, gate};

    std::vector<thread_args> dcp_t_args;
    for (size_t i = 0; i < n_dcp_threads; ++i) {
        std::string name(DCP_CURSOR_PREFIX + std::to_string(i));
        dcp_t_args.emplace_back(thread_args{
                this->vbucket.get(), this->manager.get(), name, gate});
        this->manager->registerCursor(
                name, 1, false, MustSendCheckpointEnd::YES);
    }

    rc = cb_create_thread(&persistence_thread, launch_persistence_thread, &t_args, 0);
    EXPECT_EQ(0, rc);

    rc = cb_create_thread(&checkpoint_cleanup_thread,
                        launch_checkpoint_cleanup_thread, &t_args, 0);
    EXPECT_EQ(0, rc);

    for (size_t i = 0; i < n_dcp_threads; ++i) {
        rc = cb_create_thread(
                &dcp_threads[i], launch_dcp_client_thread, &dcp_t_args[i], 0);
        EXPECT_EQ(0, rc);
    }

    for (size_t i = 0; i < n_set_threads; ++i) {
        rc = cb_create_thread(&set_threads[i], launch_set_thread, &t_args, 0);
        EXPECT_EQ(0, rc);
    }

    for (size_t i = 0; i < n_set_threads; ++i) {
        rc = cb_join_thread(set_threads[i]);
        EXPECT_EQ(0, rc);
    }

    // Push the flush command into the queue so that all other threads can be terminated.
    queued_item qi(new Item(makeStoredDocKey("flush"),
                            this->vbucket->getId(),
                            queue_op::flush,
                            0xffff,
                            0));
    this->manager->queueDirty(*this->vbucket,
                              qi,
                              GenerateBySeqno::Yes,
                              GenerateCas::Yes,
                              /*preLinkDocCtx*/ nullptr);

    rc = cb_join_thread(persistence_thread);
    EXPECT_EQ(0, rc);

    for (size_t i = 0; i < n_dcp_threads; ++i) {
        rc = cb_join_thread(dcp_threads[i]);
        EXPECT_EQ(0, rc);
        std::stringstream name;
        name << "dcp-client-" << i;
        this->manager->removeCursor(name.str());
    }

    rc = cb_join_thread(checkpoint_cleanup_thread);
    EXPECT_EQ(0, rc);
}

TYPED_TEST(CheckpointTest, reset_checkpoint_id) {
    int i;
    for (i = 0; i < 10; ++i) {
        EXPECT_TRUE(this->queueNewItem("key-" + std::to_string(i)));
    }
    EXPECT_EQ(10, this->manager->getNumOpenChkItems());
    EXPECT_EQ(10,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    EXPECT_EQ(2, this->manager->createNewCheckpoint());

    size_t itemPos;
    size_t lastMutationId = 0;
    std::vector<queued_item> items;
    const std::string cursor(CheckpointManager::pCursorName);
    auto range = this->manager->getAllItemsForCursor(cursor, items);
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1010, range.end);
    EXPECT_EQ(13, items.size());
    EXPECT_EQ(queue_op::checkpoint_start, items.at(0)->getOperation());
    // Check that the next 10 items are all SET operations.
    for(itemPos = 1; itemPos < 11; ++itemPos) {
        queued_item qi = items.at(itemPos);
        EXPECT_EQ(queue_op::mutation, qi->getOperation());
        size_t mid = qi->getBySeqno();
        EXPECT_GT(mid, lastMutationId);
        lastMutationId = qi->getBySeqno();
    }

    // Check that the following items are checkpoint end, followed by a
    // checkpoint start.
    EXPECT_EQ(queue_op::checkpoint_end, items.at(11)->getOperation());
    EXPECT_EQ(queue_op::checkpoint_start, items.at(12)->getOperation());

    items.clear();

    this->manager->checkAndAddNewCheckpoint(1, *this->vbucket);
    range = this->manager->getAllItemsForCursor(cursor, items);
    EXPECT_EQ(1001, range.start);
    EXPECT_EQ(1010, range.end);
    EXPECT_EQ(0, items.size());
}

// Sanity check test fixture
TYPED_TEST(CheckpointTest, CheckFixture) {
    // Should intially have a single cursor (persistence).
    EXPECT_EQ(1, this->manager->getNumOfCursors());
    EXPECT_EQ(0, this->manager->getNumOpenChkItems());
    for (auto& cursor : this->manager->getAllCursors()) {
        EXPECT_EQ(CheckpointManager::pCursorName, cursor.first);
    }
    // Should initially be zero items to persist.
    EXPECT_EQ(0,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Check that the items fetched matches the number we were told to expect.
    std::vector<queued_item> items;
    auto result = this->manager->getAllItemsForCursor(
            CheckpointManager::pCursorName, items);
    EXPECT_EQ(0, result.start);
    EXPECT_EQ(0, result.end);
    EXPECT_EQ(1, items.size());
    EXPECT_EQ(queue_op::checkpoint_start, items.at(0)->getOperation());
}

MATCHER_P(HasOperation, op, "") { return arg->getOperation() == op; }

// Basic test of a single, open checkpoint.
TYPED_TEST(CheckpointTest, OneOpenCkpt) {
    // Queue a set operation.
    queued_item qi(new Item(makeStoredDocKey("key1"),
                            this->vbucket->getId(),
                            queue_op::mutation,
                            /*revSeq*/ 20,
                            /*bySeq*/ 0));

    // No set_ops in queue, expect queueDirty to return true (increase
    // persistence queue size).
    EXPECT_TRUE(this->manager->queueDirty(*this->vbucket,
                                          qi,
                                          GenerateBySeqno::Yes,
                                          GenerateCas::Yes,
                                          /*preLinkDocCtx*/ nullptr));
    EXPECT_EQ(1, this->manager->getNumCheckpoints()); // Single open checkpoint.
    // 1x op_set
    EXPECT_EQ(1, this->manager->getNumOpenChkItems());
    EXPECT_EQ(1001, qi->getBySeqno());
    EXPECT_EQ(20, qi->getRevSeqno());
    EXPECT_EQ(1,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Adding the same key again shouldn't increase the size.
    queued_item qi2(new Item(makeStoredDocKey("key1"),
                             this->vbucket->getId(),
                             queue_op::mutation,
                             /*revSeq*/ 21,
                             /*bySeq*/ 0));
    EXPECT_FALSE(this->manager->queueDirty(*this->vbucket,
                                           qi2,
                                           GenerateBySeqno::Yes,
                                           GenerateCas::Yes,
                                           /*preLinkDocCtx*/ nullptr));
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    EXPECT_EQ(1, this->manager->getNumOpenChkItems());
    EXPECT_EQ(1002, qi2->getBySeqno());
    EXPECT_EQ(21, qi2->getRevSeqno());
    EXPECT_EQ(1,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Adding a different key should increase size.
    queued_item qi3(new Item(makeStoredDocKey("key2"),
                             this->vbucket->getId(),
                             queue_op::mutation,
                             /*revSeq*/ 0,
                             /*bySeq*/ 0));
    EXPECT_TRUE(this->manager->queueDirty(*this->vbucket,
                                          qi3,
                                          GenerateBySeqno::Yes,
                                          GenerateCas::Yes,
                                          /*preLinkDocCtx*/ nullptr));
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    EXPECT_EQ(2, this->manager->getNumOpenChkItems());
    EXPECT_EQ(1003, qi3->getBySeqno());
    EXPECT_EQ(0, qi3->getRevSeqno());
    EXPECT_EQ(2,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Check that the items fetched matches the number we were told to expect.
    std::vector<queued_item> items;
    auto result = this->manager->getAllItemsForCursor(
            CheckpointManager::pCursorName, items);
    EXPECT_EQ(0, result.start);
    EXPECT_EQ(1003, result.end);
    EXPECT_EQ(3, items.size());
    EXPECT_THAT(items,
                testing::ElementsAre(HasOperation(queue_op::checkpoint_start),
                                     HasOperation(queue_op::mutation),
                                     HasOperation(queue_op::mutation)));
}

// Test that enqueuing a single delete works.
TYPED_TEST(CheckpointTest, Delete) {
    // Enqueue a single delete.
    queued_item qi{new Item{makeStoredDocKey("key1"),
                            this->vbucket->getId(),
                            queue_op::mutation,
                            /*revSeq*/ 10,
                            /*byseq*/ 0}};
    qi->setDeleted();
    EXPECT_TRUE(this->manager->queueDirty(*this->vbucket,
                                          qi,
                                          GenerateBySeqno::Yes,
                                          GenerateCas::Yes,
                                          /*preLinkDocCtx*/ nullptr));

    EXPECT_EQ(1, this->manager->getNumCheckpoints());  // Single open checkpoint.
    EXPECT_EQ(1, this->manager->getNumOpenChkItems()); // 1x op_del
    EXPECT_EQ(1001, qi->getBySeqno());
    EXPECT_EQ(10, qi->getRevSeqno());

    // Check that the items fetched matches what was enqueued.
    std::vector<queued_item> items;
    auto result = this->manager->getAllItemsForCursor
            (CheckpointManager::pCursorName, items);

    EXPECT_EQ(0, result.start);
    EXPECT_EQ(1001, result.end);
    ASSERT_EQ(2, items.size());
    EXPECT_THAT(items,
                testing::ElementsAre(HasOperation(queue_op::checkpoint_start),
                                     HasOperation(queue_op::mutation)));
    EXPECT_TRUE(items[1]->isDeleted());
}


// Test with one open and one closed checkpoint.
TYPED_TEST(CheckpointTest, OneOpenOneClosed) {
    // Add some items to the initial (open) checkpoint.
    for (auto i : {1,2}) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(i)));
    }
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    // 2x op_set
    EXPECT_EQ(2, this->manager->getNumOpenChkItems());
    const uint64_t ckpt_id1 = this->manager->getOpenCheckpointId();

    // Create a new checkpoint (closing the current open one).
    const uint64_t ckpt_id2 = this->manager->createNewCheckpoint();
    EXPECT_NE(ckpt_id1, ckpt_id2) << "New checkpoint ID should differ from old";
    EXPECT_EQ(ckpt_id1, this->manager->getLastClosedCheckpointId());
    EXPECT_EQ(0, this->manager->getNumOpenChkItems()); // no items yet

    // Add some items to the newly-opened checkpoint (note same keys as 1st
    // ckpt).
    for (auto ii : {1,2}) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    // 2x op_set
    EXPECT_EQ(2, this->manager->getNumOpenChkItems());

    // Examine the items - should be 2 lots of two keys.
    EXPECT_EQ(4,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Check that the items fetched matches the number we were told to expect.
    std::vector<queued_item> items;
    auto result = this->manager->getAllItemsForCursor(
            CheckpointManager::pCursorName, items);
    EXPECT_EQ(0, result.start);
    EXPECT_EQ(1004, result.end);
    EXPECT_EQ(7, items.size());
    EXPECT_THAT(items,
                testing::ElementsAre(HasOperation(queue_op::checkpoint_start),
                                     HasOperation(queue_op::mutation),
                                     HasOperation(queue_op::mutation),
                                     HasOperation(queue_op::checkpoint_end),
                                     HasOperation(queue_op::checkpoint_start),
                                     HasOperation(queue_op::mutation),
                                     HasOperation(queue_op::mutation)));
}

// Test the automatic creation of checkpoints based on the number of items.
TYPED_TEST(CheckpointTest, ItemBasedCheckpointCreation) {
    // Size down the default number of items to create a new checkpoint and
    // recreate the manager
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               MIN_CHECKPOINT_ITEMS,
                                               /*numCheckpoints*/ 2,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ false,
                                               /*persistenceEnabled*/ true);
    // TODO: ^^ Consider a variant for Ephemeral testing -
    // persistenceEnabled:false

    this->createManager();

    // Create one less than the number required to create a new checkpoint.
    queued_item qi;
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_EQ(ii, this->manager->getNumOpenChkItems());

        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
        EXPECT_EQ(1, this->manager->getNumCheckpoints());
    }

    // Add one more - should create a new checkpoint.
    EXPECT_TRUE(this->queueNewItem("key_epoch"));
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    EXPECT_EQ(1, this->manager->getNumOpenChkItems()); // 1x op_set

    // Fill up this checkpoint also - note loop for MIN_CHECKPOINT_ITEMS - 1
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS - 1; ii++) {
        EXPECT_EQ(ii + 1,
                  this->manager->getNumOpenChkItems()); /* +1 initial set */

        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));

        EXPECT_EQ(2, this->manager->getNumCheckpoints());
    }

    // Add one more - as we have hit maximum checkpoints should *not* create a
    // new one.
    EXPECT_TRUE(this->queueNewItem("key_epoch2"));
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    EXPECT_EQ(11, // 1x key_epoch, 9x key_X, 1x key_epoch2
              this->manager->getNumOpenChkItems());

    // Fetch the items associated with the persistence cursor. This
    // moves the single cursor registered outside of the initial checkpoint,
    // allowing a new open checkpoint to be created.
    EXPECT_EQ(1, this->manager->getNumOfCursors());
    snapshot_range_t range;
    std::vector<queued_item> items;
    range = this->manager->getAllItemsForCursor(CheckpointManager::pCursorName,
                                                items);

    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1021, range.end);
    EXPECT_EQ(24, items.size());

    // Should still have the same number of checkpoints and open items.
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    EXPECT_EQ(11, this->manager->getNumOpenChkItems());

    // But adding a new item will create a new one.
    EXPECT_TRUE(this->queueNewItem("key_epoch3"));
    EXPECT_EQ(3, this->manager->getNumCheckpoints());
    EXPECT_EQ(1, this->manager->getNumOpenChkItems()); // 1x op_set
}

// Test checkpoint and cursor accounting - when checkpoints are closed the
// offset of cursors is updated as appropriate.
TYPED_TEST(CheckpointTest, CursorOffsetOnCheckpointClose) {
    // Add two items to the initial (open) checkpoint.
    for (auto i : {1,2}) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(i)));
    }
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    // 2x op_set
    EXPECT_EQ(2, this->manager->getNumOpenChkItems());

    // Use the existing persistence cursor for this test:
    EXPECT_EQ(
            2,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Cursor should initially have two items pending";

    // Check de-dupe counting - after adding another item with the same key,
    // should still see two items.
    EXPECT_FALSE(this->queueNewItem("key1")) << "Adding a duplicate key to "
                                                "open checkpoint should not "
                                                "increase queue size";

    EXPECT_EQ(
            2,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected 2 items for cursor (2x op_set) after adding a "
               "duplicate.";

    // Create a new checkpoint (closing the current open one).
    this->manager->createNewCheckpoint();
    EXPECT_EQ(0, this->manager->getNumOpenChkItems());
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    EXPECT_EQ(
            2,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected 2 items for cursor after creating new checkpoint";

    // Advance cursor - first to get the 'checkpoint_start' meta item,
    // and a second time to get the a 'proper' mutation.
    bool isLastMutationItem;
    auto item = this->manager->nextItem(CheckpointManager::pCursorName,
                                        isLastMutationItem);
    EXPECT_TRUE(item->isCheckPointMetaItem());
    EXPECT_FALSE(isLastMutationItem);
    EXPECT_EQ(
            2,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected 2 items for cursor after advancing one item";

    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_FALSE(item->isCheckPointMetaItem());
    EXPECT_FALSE(isLastMutationItem);
    EXPECT_EQ(
            1,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected 1 item for cursor after advancing by 1";

    // Add two items to the newly-opened checkpoint. Same keys as 1st ckpt,
    // but cannot de-dupe across checkpoints.
    for (auto ii : {1,2}) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    EXPECT_EQ(
            3,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected 3 items for cursor after adding 2 more to new "
               "checkpoint";

    // Advance the cursor 'out' of the first checkpoint.
    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_FALSE(item->isCheckPointMetaItem());
    EXPECT_TRUE(isLastMutationItem);

    // Now at the end of the first checkpoint, move into the next checkpoint.
    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_TRUE(item->isCheckPointMetaItem());
    EXPECT_TRUE(isLastMutationItem);
    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_TRUE(item->isCheckPointMetaItem());
    EXPECT_FALSE(isLastMutationItem);

    // Tell Checkpoint manager the items have been persisted, so it advances
    // pCursorPreCheckpointId, which will allow us to remove the closed
    // unreferenced checkpoints.
    this->manager->itemsPersisted();

    // Both previous checkpoints are unreferenced. Close them. This will
    // cause the offset of this cursor to be recalculated.
    bool new_open_ckpt_created;
    EXPECT_EQ(2,
              this->manager->removeClosedUnrefCheckpoints(
                      *this->vbucket, new_open_ckpt_created));

    EXPECT_EQ(1, this->manager->getNumCheckpoints());

    EXPECT_EQ(2,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Drain the remaining items.
    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_FALSE(item->isCheckPointMetaItem());
    EXPECT_FALSE(isLastMutationItem);
    item = this->manager->nextItem(CheckpointManager::pCursorName,
                                   isLastMutationItem);
    EXPECT_FALSE(item->isCheckPointMetaItem());
    EXPECT_TRUE(isLastMutationItem);

    EXPECT_EQ(0,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));
}

// Test the getAllItemsForCursor()
TYPED_TEST(CheckpointTest, ItemsForCheckpointCursor) {
    /* We want to have items across 2 checkpoints. Size down the default number
       of items to create a new checkpoint and recreate the manager */
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               MIN_CHECKPOINT_ITEMS,
                                               /*numCheckpoints*/ 2,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ false,
                                               /*persistenceEnabled*/ true);
    // TODO: ^^ Consider a variant for Ephemeral testing -
    // persistenceEnabled:false

    this->createManager();

    /* Add items such that we have 2 checkpoints */
    queued_item qi;
    for (unsigned int ii = 0; ii < 2 * MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    /* Check if we have desired number of checkpoints and desired number of
       items */
    EXPECT_EQ(2, this->manager->getNumCheckpoints());
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS, this->manager->getNumOpenChkItems());

    /* Register DCP replication cursor */
    std::string dcp_cursor(DCP_CURSOR_PREFIX + std::to_string(1));
    this->manager->registerCursorBySeqno(
            dcp_cursor.c_str(), 0, MustSendCheckpointEnd::NO);

    /* Get items for persistence*/
    std::vector<queued_item> items;
    auto range = this->manager->getAllItemsForCursor(
            CheckpointManager::pCursorName, items);

    /* We should have got (2 * MIN_CHECKPOINT_ITEMS + 3) items. 3 additional are
       op_ckpt_start, op_ckpt_end and op_ckpt_start */
    EXPECT_EQ(2 * MIN_CHECKPOINT_ITEMS + 3, items.size());
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1000 + 2 * MIN_CHECKPOINT_ITEMS, range.end);

    /* Get items for DCP replication cursor */
    items.clear();
    range = this->manager->getAllItemsForCursor(dcp_cursor.c_str(), items);
    EXPECT_EQ(2 * MIN_CHECKPOINT_ITEMS + 3, items.size());
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1000 + 2 * MIN_CHECKPOINT_ITEMS, range.end);
}

// Test getAllItemsForCursor() when it is limited to fewer items than exist
// in total. Cursor should only advanced to the start of the 2nd checkpoint.
TYPED_TEST(CheckpointTest, ItemsForCheckpointCursorLimited) {
    /* We want to have items across 2 checkpoints. Size down the default number
       of items to create a new checkpoint and recreate the manager */
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               MIN_CHECKPOINT_ITEMS,
                                               /*numCheckpoints*/ 2,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ false,
                                               /*persistenceEnabled*/ true);

    this->createManager();

    /* Add items such that we have 2 checkpoints */
    queued_item qi;
    for (unsigned int ii = 0; ii < 2 * MIN_CHECKPOINT_ITEMS; ii++) {
        ASSERT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    /* Verify we have desired number of checkpoints and desired number of
       items */
    ASSERT_EQ(2, this->manager->getNumCheckpoints());
    ASSERT_EQ(MIN_CHECKPOINT_ITEMS, this->manager->getNumOpenChkItems());

    /* Get items for persistence. Specify a limit of 1 so we should only
     * fetch the first checkpoints' worth.
     */
    std::vector<queued_item> items;
    auto range = this->manager->getItemsForCursor(
            CheckpointManager::pCursorName, items, 1);
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1000 + MIN_CHECKPOINT_ITEMS, range.end);
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS + 2, items.size())
            << "Should have MIN_CHECKPOINT_ITEMS + 2 (ckpt start & end) items";
    EXPECT_EQ(2,
              this->manager->getCheckpointIdForCursor(
                      CheckpointManager::pCursorName))
            << "Cursor should have moved into second checkpoint.";

}

// Test the checkpoint cursor movement
TYPED_TEST(CheckpointTest, CursorMovement) {
    /* We want to have items across 2 checkpoints. Size down the default number
     of items to create a new checkpoint and recreate the manager */
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               MIN_CHECKPOINT_ITEMS,
                                               /*numCheckpoints*/ 2,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ false,
                                               /*persistenceEnabled*/true);
    // TODO: ^^ Consider a variant for Ephemeral testing -
    // persistenceEnabled:false

    this->createManager();

    /* Add items such that we have 1 full (max items as per config) checkpoint.
       Adding another would open new checkpoint */
    queued_item qi;
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    /* Check if we have desired number of checkpoints and desired number of
       items */
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS, this->manager->getNumOpenChkItems());

    /* Register DCP replication cursor */
    std::string dcp_cursor(DCP_CURSOR_PREFIX + std::to_string(1));
    this->manager->registerCursorBySeqno(
            dcp_cursor.c_str(), 0, MustSendCheckpointEnd::NO);

    /* Get items for persistence cursor */
    std::vector<queued_item> items;
    auto range = this->manager->getAllItemsForCursor(
            CheckpointManager::pCursorName, items);

    /* We should have got (MIN_CHECKPOINT_ITEMS + op_ckpt_start) items. */
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS + 1, items.size());
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1000 + MIN_CHECKPOINT_ITEMS, range.end);

    /* Get items for DCP replication cursor */
    items.clear();
    range = this->manager->getAllItemsForCursor(dcp_cursor.c_str(), items);
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS + 1, items.size());
    EXPECT_EQ(0, range.start);
    EXPECT_EQ(1000 + MIN_CHECKPOINT_ITEMS, range.end);

    uint64_t curr_open_chkpt_id = this->manager->getOpenCheckpointId_UNLOCKED();

    /* Run the checkpoint remover so that new open checkpoint is created */
    bool newCheckpointCreated;
    this->manager->removeClosedUnrefCheckpoints(*this->vbucket,
                                                newCheckpointCreated);
    EXPECT_EQ(curr_open_chkpt_id + 1,
              this->manager->getOpenCheckpointId_UNLOCKED());

    /* Get items for persistence cursor */
    EXPECT_EQ(
            0,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected to have no normal (only meta) items";
    items.clear();
    range = this->manager->getAllItemsForCursor(CheckpointManager::pCursorName,
                                                items);

    /* We should have got op_ckpt_start item */
    EXPECT_EQ(1, items.size());
    EXPECT_EQ(1000 + MIN_CHECKPOINT_ITEMS, range.start);
    EXPECT_EQ(1000 + MIN_CHECKPOINT_ITEMS, range.end);

    EXPECT_EQ(queue_op::checkpoint_start, items.at(0)->getOperation());

    /* Get items for DCP replication cursor */
    EXPECT_EQ(
            0,
            this->manager->getNumItemsForCursor(CheckpointManager::pCursorName))
            << "Expected to have no normal (only meta) items";
    items.clear();
    this->manager->getAllItemsForCursor(dcp_cursor.c_str(), items);
    /* Expecting only 1 op_ckpt_start item */
    EXPECT_EQ(1, items.size());
    EXPECT_EQ(queue_op::checkpoint_start, items.at(0)->getOperation());
}

// Test the checkpoint cursor movement for replica vBuckets (where we can
// perform more checkpoint collapsing)
TYPED_TEST(CheckpointTest, CursorMovementReplicaMerge) {
    this->vbucket->setState(vbucket_state_replica);

    /* We want to have items across 2 checkpoints. Size down the default number
     of items to create a new checkpoint and recreate the manager */
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               MIN_CHECKPOINT_ITEMS,
                                               /*numCheckpoints*/ 2,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ true,
                                               /*persistenceEnabled*/true);
    // TODO: ^^ Consider a variant for Ephemeral testing -
    // persistenceEnabled:false

    // Add items such that we have a checkpoint at half-capacity.
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS / 2; ii++) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    /* Check if we have desired number of checkpoints and desired number of
        items */
    EXPECT_EQ(1, this->manager->getNumCheckpoints());
    EXPECT_EQ((MIN_CHECKPOINT_ITEMS / 2), this->manager->getNumOpenChkItems());

    // Register DCP replication cursor, which will be moved into the middle of
    // first checkpoint and then left there.
    std::string dcp_cursor{DCP_CURSOR_PREFIX + std::to_string(1)};
    this->manager->registerCursorBySeqno(
            dcp_cursor.c_str(), 0, MustSendCheckpointEnd::NO);

    std::vector<queued_item> items;
    this->manager->getAllItemsForCursor(dcp_cursor.c_str(), items);
    EXPECT_EQ((MIN_CHECKPOINT_ITEMS / 2) + 1, items.size());

    // Add more items so this checkpoint is now full.
    for (unsigned int ii = MIN_CHECKPOINT_ITEMS / 2; ii < MIN_CHECKPOINT_ITEMS;
            ii++) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }
    EXPECT_EQ(1, this->manager->getNumCheckpoints())
            << "Should still only have 1 checkpoint after adding "
               "MIN_CHECKPOINT_ITEMS total";
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS, this->manager->getNumOpenChkItems());

    /* Get items for persistence cursor - this will move the persistence cursor
     * out of the initial checkpoint.
     */
    items.clear();
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);

    /* We should have got (MIN_CHECKPOINT_ITEMS + op_ckpt_start) items. */
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS + 1, items.size());

    EXPECT_EQ(1, this->manager->getOpenCheckpointId_UNLOCKED());

    // Create a new checkpoint.
    EXPECT_EQ(2, this->manager->createNewCheckpoint());

    // Add another MIN_CHECKPOINT_ITEMS. This should fill up the second
    // checkpoint.
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_TRUE(this->queueNewItem("keyB_" + std::to_string(ii)));
    }

    // Move the persistence cursor through these new items.
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));
    items.clear();
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS + 1, items.size());

    // Create a third checkpoint.
    EXPECT_EQ(3, this->manager->createNewCheckpoint());

    // Move persistence cursor into third checkpoint.
    items.clear();
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    EXPECT_EQ(1, items.size())
        << "Expected to get a single meta item";
    EXPECT_EQ(queue_op::checkpoint_start, items.at(0)->getOperation());

    // We now have an unoccupied second checkpoint. We should be able to
    // collapse this, and move the dcp_cursor into the merged checkpoint.
    bool newCheckpointCreated;
    this->manager->removeClosedUnrefCheckpoints(*this->vbucket,
                                                newCheckpointCreated);

    /* Get items for DCP cursor */
    EXPECT_EQ(MIN_CHECKPOINT_ITEMS / 2 + MIN_CHECKPOINT_ITEMS,
              this->manager->getNumItemsForCursor(dcp_cursor))
            << "DCP cursor remaining items should have been recalculated after "
               "close of unref checkpoints.";

    items.clear();
    auto range = this->manager->getAllItemsForCursor(dcp_cursor.c_str(), items);
    EXPECT_EQ(1001, range.start);
    EXPECT_EQ(1020, range.end);

    // Check we have received correct items (done in chunks because
    // EXPECT_THAT maxes out at 10 elements).
    std::vector<queued_item> items_a(items.begin(), items.begin() + 5);
    // Remainder of first checkpoint.
    EXPECT_THAT(items_a, testing::Each(HasOperation(queue_op::mutation)));

    // Second checkpoint's data- 10x set.
    std::vector<queued_item> items_b(items.begin() + 5, items.begin() + 15);
    EXPECT_THAT(items_b, testing::Each(HasOperation(queue_op::mutation)));

    // end of second checkpoint and start of third.
    std::vector<queued_item> items_c(items.begin() + 15, items.end());
    EXPECT_THAT(items_c,
                testing::ElementsAre(HasOperation(queue_op::checkpoint_end),
                                     HasOperation(queue_op::checkpoint_start)));
}

// MB-25056 - Regression test replicating situation where the seqno returned by
// registerCursorBySeqno minus one is greater than the input parameter
// startBySeqno but a backfill is not required.
TYPED_TEST(CheckpointTest, MB25056_backfill_not_required) {
    std::vector<queued_item> items;
    this->vbucket->setState(vbucket_state_replica);

    ASSERT_TRUE(this->queueNewItem("key0"));
    // Add duplicate items, which should cause de-duplication to occur.
    for (unsigned int ii = 0; ii < MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_FALSE(this->queueNewItem("key0"));
    }
    // Add a number of non duplicate items to the same checkpoint
    for (unsigned int ii = 1; ii < MIN_CHECKPOINT_ITEMS; ii++) {
        EXPECT_TRUE(this->queueNewItem("key" + std::to_string(ii)));
    }

    // Register DCP replication cursor
    std::string dcp_cursor(DCP_CURSOR_PREFIX);
    // Request to register the cursor with a seqno that has been de-duped away
    CursorRegResult result = this->manager->registerCursorBySeqno(
            dcp_cursor.c_str(), 1005, MustSendCheckpointEnd::NO);
    EXPECT_EQ(1011, result.first) << "Returned seqno is not expected value.";
    EXPECT_FALSE(result.second) << "Backfill is unexpectedly required.";
}

//
// It's critical that the HLC (CAS) is ordered with seqno generation
// otherwise XDCR may drop a newer bySeqno mutation because the CAS is not
// higher.
//
TYPED_TEST(CheckpointTest, SeqnoAndHLCOrdering) {
    const int n_threads = 8;
    const int n_items = 1000;

    // configure so we can store a large number of items
    // configure with 1 checkpoint to ensure the time-based closing
    // does not split the items over many checkpoints and muddy the final
    // data checks.
    this->checkpoint_config = CheckpointConfig(DEFAULT_CHECKPOINT_PERIOD,
                                               n_threads * n_items,
                                               /*numCheckpoints*/ 1,
                                               /*itemBased*/ true,
                                               /*keepClosed*/ false,
                                               /*enableMerge*/ false,
                                               /*persistenceEnabled*/true);
    // TODO: ^^ Consider a variant for Ephemeral testing -
    // persistenceEnabled:false

    this->createManager();

    std::vector<std::thread> threads;

    // vector of pairs, first is seqno, second is CAS
    // just do a scatter gather over n_threads
    std::vector<std::vector<std::pair<uint64_t, uint64_t> > > threadData(n_threads);
    for (int ii = 0; ii < n_threads; ii++) {
        auto& threadsData = threadData[ii];
        threads.push_back(std::thread([this, ii, n_items, &threadsData](){
            std::string key = "key" + std::to_string(ii);
            for (int item  = 0; item < n_items; item++) {
                queued_item qi(
                        new Item(makeStoredDocKey(key + std::to_string(item)),
                                 this->vbucket->getId(),
                                 queue_op::mutation,
                                 /*revSeq*/ 0,
                                 /*bySeq*/ 0));
                EXPECT_TRUE(
                        this->manager->queueDirty(*this->vbucket,
                                                  qi,
                                                  GenerateBySeqno::Yes,
                                                  GenerateCas::Yes,
                                                  /*preLinkDocCtx*/ nullptr));

                // Save seqno/cas
                threadsData.push_back(std::make_pair(qi->getBySeqno(), qi->getCas()));
            }
        }));
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Now combine the data and check HLC is increasing with seqno
    std::map<uint64_t, uint64_t> finalData;
    for (auto t : threadData) {
        for (auto pair : t) {
            EXPECT_EQ(finalData.end(), finalData.find(pair.first));
            finalData[pair.first] = pair.second;
        }
    }

    auto itr = finalData.begin();
    EXPECT_NE(itr, finalData.end());
    uint64_t previousCas = (itr++)->second;
    EXPECT_NE(itr, finalData.end());
    for (; itr != finalData.end(); itr++) {
        EXPECT_LT(previousCas, itr->second);
        previousCas = itr->second;
    }

    // Now a final check, iterate the checkpoint and also check for increasing
    // HLC.
    std::vector<queued_item> items;
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);

    /* We should have got (n_threads*n_items + op_ckpt_start) items. */
    EXPECT_EQ(n_threads*n_items + 1, items.size());

    previousCas = items[1]->getCas();
    for (size_t ii = 2; ii < items.size(); ii++) {
        EXPECT_LT(previousCas, items[ii]->getCas());
        previousCas = items[ii]->getCas();
    }
}

// Test cursor is correctly updated when enqueuing a key which already exists
// in the checkpoint (and needs de-duping), where the cursor points at a
// meta-item at the head of the checkpoint:
//
//  Before:
//      Checkpoint [ 0:EMPTY(), 1:CKPT_START(), 1:SET(key), 2:SET_VBSTATE() ]
//                                                               ^
//                                                            Cursor
//
//  After:
//      Checkpoint [ 0:EMPTY(), 1:CKPT_START(), 2:SET_VBSTATE(), 2:SET(key) ]
//                                                     ^
//                                                   Cursor
//
TYPED_TEST(CheckpointTest, CursorUpdateForExistingItemWithMetaItemAtHead) {
    // Setup the checkpoint and cursor.
    ASSERT_EQ(1, this->manager->getNumItems());
    ASSERT_TRUE(this->queueNewItem("key"));
    ASSERT_EQ(2, this->manager->getNumItems());
    this->manager->queueSetVBState(*this->vbucket);

    ASSERT_EQ(3, this->manager->getNumItems());

    // Advance persistence cursor so all items have been consumed.
    std::vector<queued_item> items;
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    ASSERT_EQ(3, items.size());
    ASSERT_EQ(0,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Queue an item with a duplicate key.
    this->queueNewItem("key");

    // Test: Should have one item for cursor (the one we just added).
    EXPECT_EQ(1,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Should have another item to read (new version of 'key')
    items.clear();
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    EXPECT_EQ(1, items.size());
}

// Test cursor is correctly updated when enqueuing a key which already exists
// in the checkpoint (and needs de-duping), where the cursor points at a
// meta-item *not* at the head of the checkpoint:
//
//  Before:
//      Checkpoint [ 0:EMPTY(), 1:CKPT_START(), 1:SET_VBSTATE(key), 1:SET() ]
//                                                     ^
//                                                    Cursor
//
//  After:
//      Checkpoint [ 0:EMPTY(), 1:CKPT_START(), 1:SET_VBSTATE(key), 2:SET() ]
//                                                     ^
//                                                   Cursor
//
TYPED_TEST(CheckpointTest, CursorUpdateForExistingItemWithNonMetaItemAtHead) {
    // Setup the checkpoint and cursor.
    ASSERT_EQ(1, this->manager->getNumItems());
    this->manager->queueSetVBState(*this->vbucket);
    ASSERT_EQ(2, this->manager->getNumItems());

    // Advance persistence cursor so all items have been consumed.
    std::vector<queued_item> items;
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    ASSERT_EQ(2, items.size());
    ASSERT_EQ(0,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Queue a set (cursor will now be one behind).
    ASSERT_TRUE(this->queueNewItem("key"));
    ASSERT_EQ(1,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Test: queue an item with a duplicate key.
    this->queueNewItem("key");

    // Test: Should have one item for cursor (the one we just added).
    EXPECT_EQ(1,
              this->manager->getNumItemsForCursor(
                      CheckpointManager::pCursorName));

    // Should an item to read (new version of 'key')
    items.clear();
    this->manager->getAllItemsForCursor(CheckpointManager::pCursorName, items);
    EXPECT_EQ(1, items.size());
    EXPECT_EQ(1002, items.at(0)->getBySeqno());
    EXPECT_EQ(makeStoredDocKey("key"), items.at(0)->getKey());
}

// Regression test for MB-21925 - when a duplicate key is queued and the
// persistence cursor is still positioned on the initial dummy key,
// should return EXISTING_ITEM.
TYPED_TEST(CheckpointTest,
           MB21925_QueueDuplicateWithPersistenceCursorOnInitialMetaItem) {
    // Need a manager starting from seqno zero.
    this->createManager(0);
    ASSERT_EQ(0, this->manager->getHighSeqno());
    ASSERT_EQ(1, this->manager->getNumItems())
            << "Should start with queue_op::empty on checkpoint.";

    // Add an item with some new key.
    ASSERT_TRUE(this->queueNewItem("key"));

    // Test - second item (duplicate key) should return false.
    EXPECT_FALSE(this->queueNewItem("key"));
}
