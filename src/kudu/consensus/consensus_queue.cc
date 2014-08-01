// Copyright (c) 2013, Cloudera, inc.

#include <algorithm>
#include <boost/foreach.hpp>
#include <boost/thread/locks.hpp>
#include <gflags/gflags.h>
#include <iostream>
#include <string>
#include <tr1/memory>
#include <utility>

#include "kudu/consensus/consensus_queue.h"
#include "kudu/consensus/log_util.h"

#include "kudu/common/wire_protocol.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/util/auto_release_pool.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/url-coding.h"

DEFINE_int32(consensus_entry_cache_size_soft_limit_mb, 128,
             "The total per-tablet size of consensus entries to keep in memory."
             " This is a soft limit, i.e. messages in the queue are discarded"
             " down to this limit only if no peer needs to replicate them.");
DEFINE_int32(consensus_entry_cache_size_hard_limit_mb, 256,
             "The total per-tablet size of consensus entries to keep in memory."
             " This is a hard limit, i.e. messages in the queue are always discarded"
             " down to this limit. If a peer has not yet replicated the messages"
             " selected to be discarded the peer will be evicted from the quorum.");

DEFINE_int32(global_consensus_entry_cache_size_soft_limit_mb, 1024,
             "Server-wide version of 'consensus_entry_cache_size_soft_limit_mb'");
DEFINE_int32(global_consensus_entry_cache_size_hard_limit_mb, 1024,
             "Server-wide version of 'consensus_entry_cache_size_hard_limit_mb'");

DEFINE_int32(consensus_max_batch_size_bytes, 1024 * 1024,
             "The maximum per-tablet RPC batch size when updating peers.");
DEFINE_bool(consensus_dump_queue_on_full, false,
            "Whether to dump the full contents of the consensus queue to the log"
            " when it gets full. Mostly useful for debugging.");

namespace kudu {
namespace consensus {

using log::Log;
using log::OpIdCompare;
using metadata::QuorumPeerPB;
using std::tr1::shared_ptr;
using std::tr1::unordered_map;
using strings::Substitute;

METRIC_DEFINE_gauge_int64(total_num_ops, MetricUnit::kCount,
                          "Total number of queued operations in the leader queue.");
METRIC_DEFINE_gauge_int64(num_all_done_ops, MetricUnit::kCount,
                          "Number of operations in the leader queue ack'd by all peers.");
METRIC_DEFINE_gauge_int64(num_majority_done_ops, MetricUnit::kCount,
                          "Number of operations in the leader queue ack'd by a majority but "
                          "not all peers.");
METRIC_DEFINE_gauge_int64(num_in_progress_ops, MetricUnit::kCount,
                          "Number of operations in the leader queue ack'd by a minority of "
                          "peers.");

// TODO expose and register metics via the MemTracker itself, so that
// we don't have to do the accounting in two places.
METRIC_DEFINE_gauge_int64(queue_size_bytes, MetricUnit::kBytes,
                          "Size of the leader queue, in bytes.");

const char kConsensusQueueParentTrackerId[] = "consensus_queue_parent";

OperationStatusTracker::OperationStatusTracker(gscoped_ptr<OperationPB> operation)
  : operation_(operation.Pass()) {
}

#define INSTANTIATE_METRIC(x) \
  AtomicGauge<int64_t>::Instantiate(x, metric_ctx)
PeerMessageQueue::Metrics::Metrics(const MetricContext& metric_ctx)
  : total_num_ops(INSTANTIATE_METRIC(METRIC_total_num_ops)),
    num_all_done_ops(INSTANTIATE_METRIC(METRIC_num_all_done_ops)),
    num_majority_done_ops(INSTANTIATE_METRIC(METRIC_num_majority_done_ops)),
    num_in_progress_ops(INSTANTIATE_METRIC(METRIC_num_in_progress_ops)),
    queue_size_bytes(INSTANTIATE_METRIC(METRIC_queue_size_bytes)) {
}
#undef INSTANTIATE_METRIC

PeerMessageQueue::PeerMessageQueue(const MetricContext& metric_ctx,
                                   const std::string& parent_tracker_id)
    : max_ops_size_bytes_hard_(FLAGS_consensus_entry_cache_size_hard_limit_mb * 1024 * 1024),
      global_max_ops_size_bytes_hard_(
          FLAGS_global_consensus_entry_cache_size_hard_limit_mb * 1024 * 1024),
      metrics_(metric_ctx),
      state_(kQueueOpen) {
  uint64_t max_ops_size_bytes_soft = FLAGS_consensus_entry_cache_size_soft_limit_mb * 1024 * 1024;
  uint64_t global_max_ops_size_bytes_soft =
      FLAGS_global_consensus_entry_cache_size_soft_limit_mb * 1024 * 1024;

  // If no tracker is registered for kConsensusQueueMemTrackerId,
  // create one using the global soft limit.
  parent_tracker_ = MemTracker::FindOrCreateTracker(global_max_ops_size_bytes_soft,
                                                    parent_tracker_id,
                                                    NULL);

  tracker_ = MemTracker::CreateTracker(max_ops_size_bytes_soft,
                                       Substitute("$0-$1", parent_tracker_id, metric_ctx.prefix()),
                                       parent_tracker_.get());
}

Status PeerMessageQueue::TrackPeer(const string& uuid, const OpId& initial_watermark) {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  DCHECK_EQ(state_, kQueueOpen);
  // TODO allow the queue to go and fetch requests from the log
  // up to a point.
  DCHECK(initial_watermark.IsInitialized());
  ConsensusStatusPB* status = new ConsensusStatusPB();
  status->mutable_safe_commit_watermark()->CopyFrom(initial_watermark);
  status->mutable_replicated_watermark()->CopyFrom(initial_watermark);
  status->mutable_received_watermark()->CopyFrom(initial_watermark);
  InsertOrDie(&watermarks_, uuid, status);
  return Status::OK();
}

void PeerMessageQueue::UntrackPeer(const string& uuid) {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  ConsensusStatusPB* status = EraseKeyReturnValuePtr(&watermarks_, uuid);
  if (status != NULL) {
    delete status;
  }
}

Status PeerMessageQueue::AppendOperation(scoped_refptr<OperationStatusTracker> status) {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  DCHECK_EQ(state_, kQueueOpen);
  const OperationPB* operation = status->operation();

  DCHECK(operation->has_commit()
         || operation->has_replicate()) << "Operation must be a commit or a replicate: "
             << operation->DebugString();

  // Once either the local or global soft limit is exceeded...
  if (tracker_->AnyLimitExceeded()) {
    // .. try to trim the queue.
    Status s  = TrimBufferForMessage(operation);
    if (PREDICT_FALSE(!s.ok() && (VLOG_IS_ON(2) || FLAGS_consensus_dump_queue_on_full))) {
      queue_lock_.unlock();
      LOG(INFO) << "Queue Full: Dumping State:";
      vector<string>  queue_dump;
      DumpToStringsUnlocked(&queue_dump);
      BOOST_FOREACH(const string& line, queue_dump) {
        LOG(INFO) << line;
      }
    }
    RETURN_NOT_OK(s);
  }

  // If we get here, then either:
  //
  // 1) We were able to trim the queue such that no local or global
  // soft limit was exceeded
  // 2) We were unable to trim the queue to below any soft limits, but
  // hard limits were not violated.
  // 3) 'operation' is a COMMIT instead of a REPLICATE.
  //
  // See also: TrimBufferForMessage() in this class.
  metrics_.queue_size_bytes->IncrementBy(operation->SpaceUsed());
  tracker_->Consume(operation->SpaceUsed());

  if (PREDICT_FALSE(VLOG_IS_ON(2))) {
    VLOG(2) << "Appended operation to queue: " << operation->ShortDebugString() <<
        " Operation Status: " << status->ToString();
  }
  InsertOrDieNoPrint(&messages_, status->op_id(), status);
  metrics_.total_num_ops->Increment();

  // In tests some operations might already be IsAllDone().
  if (PREDICT_FALSE(status->IsAllDone())) {
    metrics_.num_all_done_ops->Increment();
  // If we're just replicating to learners, some operations might already
  // be IsDone().
  } else if (PREDICT_FALSE(status->IsDone())) {
    metrics_.num_majority_done_ops->Increment();
  } else {
    metrics_.num_in_progress_ops->Increment();
  }

  return Status::OK();
}

void PeerMessageQueue::RequestForPeer(const string& uuid,
                                      ConsensusRequestPB* request) {
  // Clear the requests without deleting the entries, as they may be in use by other peers.
  request->mutable_ops()->ExtractSubrange(0, request->ops_size(), NULL);
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  DCHECK_EQ(state_, kQueueOpen);
  const ConsensusStatusPB* current_status = FindOrDie(watermarks_, uuid);

  MessagesBuffer::iterator iter = messages_.upper_bound(current_status->received_watermark());

  // Add as many operations as we can to a request.
  OperationStatusTracker* ost = NULL;
  for (; iter != messages_.end(); iter++) {
    ost = (*iter).second.get();
    request->mutable_ops()->AddAllocated(ost->operation());
    if (request->ByteSize() > FLAGS_consensus_max_batch_size_bytes) {

      // Allow overflowing the max batch size in the case that we are sending
      // exactly onle op. Otherwise we would never send the batch!
      if (request->ops_size() > 1) {
        request->mutable_ops()->ReleaseLast();
      }
      if (PREDICT_FALSE(VLOG_IS_ON(2))) {
        VLOG(2) << "Request reached max size for peer: "
            << uuid << " trimmed to: " << request->ops_size()
            << " ops and " << request->ByteSize() << " bytes."
            << " max is: " << FLAGS_consensus_max_batch_size_bytes;
      }
      break;
    }
  }
  if (PREDICT_FALSE(VLOG_IS_ON(2))) {
    if (request->ops_size() > 0) {
      VLOG(2) << "Sending request with operations to Peer: " << uuid
              << ". Size: " << request->ops_size()
              << ". From: " << request->ops(0).id().ShortDebugString() << ". To: "
              << request->ops(request->ops_size() - 1).id().ShortDebugString();
    } else {
      VLOG(2) << "Sending status only request to Peer: " << uuid;
    }
  }
}

void PeerMessageQueue::ResponseFromPeer(const string& uuid,
                                        const ConsensusStatusPB& new_status,
                                        bool* more_pending) {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  ConsensusStatusPB* current_status = FindPtrOrNull(watermarks_, uuid);
  if (PREDICT_FALSE(state_ == kQueueClosed || current_status == NULL)) {
    LOG(WARNING) << "Queue is closed or peer was untracked, disregarding peer response.";
    *more_pending = false;
    return;
  }
  MessagesBuffer::iterator iter;
  // We always start processing messages from the lowest watermark
  // (which might be the replicated or the committed one)
  const OpId* lowest_watermark = &std::min(current_status->replicated_watermark(),
                                           current_status->safe_commit_watermark(),
                                           OpIdCompare);
  iter = messages_.upper_bound(*lowest_watermark);

  MessagesBuffer::iterator end_iter = messages_.upper_bound(new_status.received_watermark());

  if (PREDICT_FALSE(VLOG_IS_ON(2))) {
    VLOG(2) << "Received Response from Peer: " << uuid << ". Current Status: "
        << current_status->ShortDebugString() << ". New Status: " << new_status.ShortDebugString();
  }

  // We need to ack replicates and commits separately (commits are executed asynchonously).
  // So for instance in the case of commits:
  // - Check that the op is a commit
  // - Check that it falls between the last ack'd commit watermark and the
  //   incoming commit watermark
  // If both checks pass ack it. The case for replicates is similar.
  OperationStatusTracker* ost = NULL;
  for (;iter != end_iter; iter++) {
    ost = (*iter).second.get();
    bool was_done = ost->IsDone();
    bool was_all_done = ost->IsAllDone();
    OperationPB* operation = ost->operation();
    const OpId& id = operation->id();

    if (operation->has_commit() &&
        OpIdCompare(id, current_status->safe_commit_watermark()) > 0 &&
        OpIdCompare(id, new_status.safe_commit_watermark()) <= 0) {
      ost->AckPeer(uuid);
    } else if (operation->has_replicate() &&
        OpIdCompare(id, current_status->replicated_watermark()) > 0 &&
        OpIdCompare(id, new_status.replicated_watermark()) <= 0) {
      ost->AckPeer(uuid);
    }

    if (ost->IsAllDone() && !was_all_done) {
      metrics_.num_all_done_ops->Increment();
      metrics_.num_majority_done_ops->Decrement();
    }
    if (ost->IsDone() && !was_done) {
      metrics_.num_majority_done_ops->Increment();
      metrics_.num_in_progress_ops->Decrement();
    }
  }

  if (current_status == NULL) {
    InsertOrUpdate(&watermarks_, uuid, new ConsensusStatusPB(new_status));
  } else {
    current_status->CopyFrom(new_status);
  }

  // check if there are more messages pending.
  *more_pending = (iter != messages_.end());
}

Status PeerMessageQueue::GetOperationStatus(const OpId& op_id,
                                            scoped_refptr<OperationStatusTracker>* status) {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  MessagesBuffer::iterator iter = messages_.find(op_id);
  if (iter == messages_.end()) {
    return Status::NotFound("Operation is not in the queue.");
  }
  *status = (*iter).second;
  return Status::OK();
}

Status PeerMessageQueue::TrimBufferForMessage(const OperationPB* operation) {
  // TODO for now we're just trimming the buffer, but we need to handle when
  // the buffer is full but there is a peer hanging on to the queue (very delayed)
  int32_t bytes = operation->SpaceUsed();

  MessagesBuffer::iterator iter = messages_.begin();

  // If adding 'operation' to the queue would violate either a local
  // or a global soft limit, try to trim any finished operations from
  // the queue and release the memory used to the mem tracker.
  while (bytes > tracker_->SpareCapacity()) {
    OperationStatusTracker* ost = NULL;
    // Handle the situation where this tablet's consensus queue is
    // empty, but the global limits may have already been violated due
    // to the other queues' memory consumption.
    if (iter != messages_.end()) {
      ost = (*iter).second.get();
    }
    if (ost == NULL || !ost->IsAllDone()) {
      // return OK if we could trim the queue or if the operation was a commit
      // in which case we always accept it.
      if (CheckHardLimitsNotViolated(bytes) || operation->has_commit()) {
        // parent_tracker_->consumption() in this case returns total
        // consumption by _ALL_ all consensus queues, i.e., the
        // server-wide consensus queue memory consumption.
        return Status::OK();
      } else {
        return Status::ServiceUnavailable("Cannot append replicate message. Queue is full.");
      }
    }
    uint64_t bytes_to_decrement = ost->operation()->SpaceUsed();
    metrics_.total_num_ops->Decrement();
    metrics_.num_all_done_ops->Decrement();
    metrics_.queue_size_bytes->DecrementBy(bytes_to_decrement);

    tracker_->Release(bytes_to_decrement);
    messages_.erase(iter++);
  }
  return Status::OK();
}


bool PeerMessageQueue::CheckHardLimitsNotViolated(size_t bytes) const {
  bool local_limit_violated = (bytes + tracker_->consumption()) > max_ops_size_bytes_hard_;
  bool global_limit_violated = (bytes + parent_tracker_->consumption())
      > global_max_ops_size_bytes_hard_;
#ifndef NDEBUG
  if (VLOG_IS_ON(1)) {
    DVLOG(1) << "global consumption: "
             << HumanReadableNumBytes::ToString(parent_tracker_->consumption());
    string human_readable_bytes = HumanReadableNumBytes::ToString(bytes);
    if (local_limit_violated) {
      DVLOG(1) << "adding " << human_readable_bytes
               << " would violate local hard limit ("
               << HumanReadableNumBytes::ToString(max_ops_size_bytes_hard_) << ").";
    }
    if (global_limit_violated) {
      DVLOG(1) << "adding " << human_readable_bytes
               << " would violate global hard limit ("
               << HumanReadableNumBytes::ToString(global_max_ops_size_bytes_hard_) << ").";
    }
  }
#endif
  return !local_limit_violated && !global_limit_violated;
}

void PeerMessageQueue::DumpToStrings(vector<string>* lines) const {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  DumpToStringsUnlocked(lines);
}

void PeerMessageQueue::DumpToStringsUnlocked(vector<string>* lines) const {
  lines->push_back("Watermarks:");
  BOOST_FOREACH(const WatermarksMap::value_type& entry, watermarks_) {
    lines->push_back(
        Substitute("Peer: $0 Watermark: $1",
                   entry.first,
                   (entry.second != NULL ? entry.second->ShortDebugString() : "NULL")));
  }
  int counter = 0;
  lines->push_back("Messages:");
  BOOST_FOREACH(const MessagesBuffer::value_type entry, messages_) {
    const OpId& id = entry.second->op_id();
    OperationPB* operation = entry.second->operation();
    if (operation->has_replicate()) {
      lines->push_back(
          Substitute("Message[$0] $1.$2 : REPLICATE. Type: $3, Size: $4, Status: $5",
                     counter++, id.term(), id.index(),
                     OperationType_Name(operation->replicate().op_type()),
                     operation->ByteSize(), entry.second->ToString()));
    } else {
      const OpId& committed_op_id = operation->commit().commited_op_id();
      lines->push_back(
          Substitute("Message[$0] $1.$2 : COMMIT. Committed OpId: $3.$4 "
              "Type: $5, Size: $6, Status: $7",
                     counter++, id.term(), id.index(), committed_op_id.index(),
                     committed_op_id.term(),
                     OperationType_Name(operation->commit().op_type()),
                     operation->ByteSize(), entry.second->ToString()));
    }
  }
}

void PeerMessageQueue::DumpToHtml(std::ostream& out) const {
  using std::endl;

  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  out << "<h3>Watermarks</h3>" << endl;
  out << "<table>" << endl;;
  out << "  <tr><th>Peer</th><th>Watermark</th></tr>" << endl;
  BOOST_FOREACH(const WatermarksMap::value_type& entry, watermarks_) {
    string watermark_str = (entry.second != NULL ? entry.second->ShortDebugString() : "NULL");
    out << Substitute("  <tr><td>$0</td><td>$1</td></tr>",
                      EscapeForHtmlToString(entry.first),
                      EscapeForHtmlToString(watermark_str)) << endl;
  }
  out << "</table>" << endl;

  out << "<h3>Messages:</h3>" << endl;
  out << "<table>" << endl;
  out << "<tr><th>Entry</th><th>OpId</th><th>Type</th><th>Size</th><th>Status</th></tr>" << endl;

  int counter = 0;
  BOOST_FOREACH(const MessagesBuffer::value_type entry, messages_) {
    const OpId& id = entry.second->op_id();
    OperationPB* operation = entry.second->operation();
    if (operation->has_replicate()) {
      out << Substitute("<tr><th>$0</th><th>$1.$2</th><td>REPLICATE $3</td>"
                        "<td>$4</td><td>$5</td></tr>",
                        counter++, id.term(), id.index(),
                        OperationType_Name(operation->replicate().op_type()),
                        operation->ByteSize(), entry.second->ToString()) << endl;
    } else {
      const OpId& committed_op_id = operation->commit().commited_op_id();
      out << Substitute("<tr><th>$0</th><th>$1.$2</th><td>COMMIT $5 $3.$4</td>"
                        "<td>$6</td><td>$7</td></tr>",
                        counter++, id.term(), id.index(), committed_op_id.index(),
                        committed_op_id.term(),
                        OperationType_Name(operation->commit().op_type()),
                        operation->ByteSize(), entry.second->ToString()) << endl;
    }
  }
  out << "</table>";
}

void PeerMessageQueue::Close() {
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  state_ = kQueueClosed;
  STLDeleteValues(&watermarks_);
}

int64_t PeerMessageQueue::GetQueuedOperationsSizeBytesForTests() const {
  return tracker_->consumption();
}

string PeerMessageQueue::ToString() const {
  // Even though metrics are thread-safe obtain the lock so that we get
  // a "consistent" snapshot of the metrics.
  boost::lock_guard<simple_spinlock> lock(queue_lock_);
  return ToStringUnlocked();
}

string PeerMessageQueue::ToStringUnlocked() const {
  return Substitute("Consensus queue metrics: Total Ops: $0, All Done Ops: $1, "
      "Only Majority Done Ops: $2, In Progress Ops: $3, Queue Size (bytes): $4/$5",
      metrics_.total_num_ops->value(), metrics_.num_all_done_ops->value(),
      metrics_.num_majority_done_ops->value(), metrics_.num_in_progress_ops->value(),
      metrics_.queue_size_bytes->value(), max_ops_size_bytes_hard_);
}

PeerMessageQueue::~PeerMessageQueue() {
  Close();
}

}  // namespace consensus
}  // namespace kudu