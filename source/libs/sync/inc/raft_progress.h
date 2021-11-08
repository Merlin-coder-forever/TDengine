/*
 * Copyright (c) 2019 TAOS Data, Inc. <cli@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TD_SYNC_RAFT_PROGRESS_H
#define TD_SYNC_RAFT_PROGRESS_H

#include "sync_type.h"

/** 
 * SSyncRaftInflights is a sliding window for the inflight messages.
 * Thus inflight effectively limits both the number of inflight messages
 * and the bandwidth each Progress can use.
 * When inflights is full, no more message should be sent.
 * When a leader sends out a message, the index of the last
 * entry should be added to inflights. The index MUST be added
 * into inflights in order.
 * When a leader receives a reply, the previous inflights should
 * be freed by calling syncRaftInflightFreeTo with the index of the last
 * received entry.
 **/ 
typedef struct SSyncRaftInflights {
  /* the starting index in the buffer */
  int start;

  /* number of inflights in the buffer */
  int count;

  /* the size of the buffer */
  int size;

	/** 
   * buffer contains the index of the last entry
	 * inside one message.
   **/
  SyncIndex* buffer;
} SSyncRaftInflights;

/** 
 * State defines how the leader should interact with the follower.
 *
 * When in PROGRESS_PROBE, leader sends at most one replication message
 * per heartbeat interval. It also probes actual progress of the follower.
 * 
 * When in PROGRESS_REPLICATE, leader optimistically increases next
 * to the latest entry sent after sending replication message. This is
 * an optimized state for fast replicating log entries to the follower.
 *
 * When in PROGRESS_SNAPSHOT, leader should have sent out snapshot
 * before and stops sending any replication message.
 * 
 * PROGRESS_PROBE is the initial state.
 **/
typedef enum RaftProgressState {
  PROGRESS_PROBE = 0,
  PROGRESS_REPLICATE,
  PROGRESS_SNAPSHOT,
} RaftProgressState;

/**
 * Progress represents a follower’s progress in the view of the leader. Leader maintains
 * progresses of all followers, and sends entries to the follower based on its progress.
 **/
struct SSyncRaftProgress {
  SyncNodeId id;

  SyncIndex nextIndex;

  SyncIndex matchIndex;

  RaftProgressState state;

	/**
   * paused is used in PROGRESS_PROBE.
	 * When paused is true, raft should pause sending replication message to this peer. 
   **/ 
  bool paused;

  // last send append message tick
  uint32_t lastSendTick;

	/** 
   * pendingSnapshotIndex is used in PROGRESS_SNAPSHOT.
	 * If there is a pending snapshot, the pendingSnapshotIndex will be set to the
	 * index of the snapshot. If pendingSnapshotIndex is set, the replication process of
	 * this Progress will be paused. raft will not resend snapshot until the pending one
	 * is reported to be failed.
   **/
  SyncIndex pendingSnapshotIndex;

	/** 
   * recentActive is true if the progress is recently active. Receiving any messages
   * from the corresponding follower indicates the progress is active.
	 * RecentActive can be reset to false after an election timeout.
   **/
  bool recentActive;

  /**
   * flow control sliding window
   **/
  SSyncRaftInflights inflights;

  // IsLearner is true if this progress is tracked for a learner.
  bool isLearner;
};

void syncRaftInitProgress(SSyncRaft* pRaft, SSyncRaftProgress* progress);

/**
 * syncRaftProgressMaybeUpdate returns false if the given lastIndex index comes from i-th node's log.
 * Otherwise it updates the progress and returns true.
 **/
bool syncRaftProgressMaybeUpdate(SSyncRaftProgress* progress, SyncIndex lastIndex);

static FORCE_INLINE void syncRaftProgressOptimisticNextIndex(SSyncRaftProgress* progress, SyncIndex nextIndex) {
  progress->nextIndex = nextIndex + 1;
}

/** 
 * syncRaftProgressMaybeDecrTo returns false if the given to index comes from an out of order message.
 * Otherwise it decreases the progress next index to min(rejected, last) and returns true.
 **/
bool syncRaftProgressMaybeDecrTo(SSyncRaftProgress* progress,
                                SyncIndex rejected, SyncIndex lastIndex);

/** 
 * syncRaftProgressIsPaused returns whether sending log entries to this node has been
 * paused. A node may be paused because it has rejected recent
 * MsgApps, is currently waiting for a snapshot, or has reached the
 * MaxInflightMsgs limit.
 **/
bool syncRaftProgressIsPaused(SSyncRaftProgress* progress);

static FORCE_INLINE void syncRaftProgressPause(SSyncRaftProgress* progress) {
  progress->paused = true;
}

static FORCE_INLINE SyncIndex syncRaftProgressNextIndex(SSyncRaftProgress* progress) {
  return progress->nextIndex;
}

static FORCE_INLINE RaftProgressState syncRaftProgressInReplicate(SSyncRaftProgress* progress) {
  return progress->state == PROGRESS_REPLICATE;
}

static FORCE_INLINE RaftProgressState syncRaftProgressInSnapshot(SSyncRaftProgress* progress) {
  return progress->state == PROGRESS_SNAPSHOT;
}

static FORCE_INLINE RaftProgressState syncRaftProgressInProbe(SSyncRaftProgress* progress) {
  return progress->state == PROGRESS_PROBE;
}

static FORCE_INLINE bool syncRaftProgressRecentActive(SSyncRaftProgress* progress) {
  return progress->recentActive;
}

static FORCE_INLINE bool syncRaftProgressUpdateSendTick(SSyncRaftProgress* progress, SyncTick current) {
  return progress->lastSendTick = current;
}

void syncRaftProgressFailure(SSyncRaftProgress* progress);

bool syncRaftProgressNeedAbortSnapshot(SSyncRaftProgress* progress);

/** 
 * return true if progress's log is up-todate
 **/
bool syncRaftProgressIsUptodate(SSyncRaft* pRaft, SSyncRaftProgress* progress);

void syncRaftProgressBecomeProbe(SSyncRaftProgress* progress);

void syncRaftProgressBecomeReplicate(SSyncRaftProgress* progress);

void syncRaftProgressBecomeSnapshot(SSyncRaftProgress* progress, SyncIndex snapshotIndex);

/* inflights APIs */
int syncRaftInflightReset(SSyncRaftInflights* inflights);
bool syncRaftInflightFull(SSyncRaftInflights* inflights);
void syncRaftInflightAdd(SSyncRaftInflights* inflights, SyncIndex inflightIndex);
void syncRaftInflightFreeTo(SSyncRaftInflights* inflights, SyncIndex toIndex);
void syncRaftInflightFreeFirstOne(SSyncRaftInflights* inflights);

#if 0

void syncRaftProgressAbortSnapshot(SSyncRaft* pRaft, int i);



SyncIndex syncRaftProgressMatchIndex(SSyncRaft* pRaft, int i);

void syncRaftProgressUpdateLastSend(SSyncRaft* pRaft, int i);

void syncRaftProgressUpdateSnapshotLastSend(SSyncRaft* pRaft, int i);

bool syncRaftProgressResetRecentRecv(SSyncRaft* pRaft, int i);

void syncRaftProgressMarkRecentRecv(SSyncRaft* pRaft, int i);



void syncRaftProgressAbortSnapshot(SSyncRaft* pRaft, int i);

#endif

#endif /* TD_SYNC_RAFT_PROGRESS_H */