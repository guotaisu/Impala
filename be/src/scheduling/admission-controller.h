// Copyright 2014 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef SCHEDULING_ADMISSION_CONTROLLER_H
#define SCHEDULING_ADMISSION_CONTROLLER_H

#include <vector>
#include <string>
#include <list>

#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#include <boost/thread/mutex.hpp>

#include "common/status.h"
#include "statestore/statestore-subscriber.h"
#include "util/internal-queue.h"
#include "util/thread.h"

namespace impala {

class QuerySchedule;

// The AdmissionController is used to make local admission decisions based on
// cluster state disseminated by the statestore. Requests are submitted for
// execution to a given pool via AdmitQuery(). A request will either be
// admitted immediately, queued, or rejected. This decision is based on
// per-pool estimates of the total number of concurrently executing queries
// across the entire cluster and the total number of queued requests across the
// entire cluster. When the number of concurrently executing queries goes above
// a configurable per-pool threshold, requests will be queued. When the total
// number of queued requests in a particular pool goes above a configurable
// threshold, incoming requests to that pool will be rejected.
// TODO: When we resolve users->pools, explain the model and configuration story.
//       (There is one hard-coded pool right now, configurable via gflags.)
//
// The pool statistics are updated by the statestore using the
// IMPALA_REQUEST_QUEUE_TOPIC topic. Every <impalad, pool> pair is sent as a
// topic update when pool statistics change, and the topic updates from other
// impalads are used to re-compute the total per-pool stats. When there are
// queued requests and the number of concurrently executing queries drops below
// the configured maximum, a number of queued requests will be
// admitted according to the following formula:
//   N = (#local_pool_queued / #global_pool_queued) * (pool_limit - #global_pool_running)
// However, because the pool statistics are only updated on statestore
// heartbeats and all decisions are made locally, the total pool statistics are
// estimates.  As a result, more requests may be admitted or queued than the
// configured thresholds, which are really soft limits.
class AdmissionController {
 public:
  AdmissionController(Metrics* metrics, const std::string& backend_id);
  ~AdmissionController();

  // Submits the request for admission. Returns immediately if rejected, but
  // otherwise blocks until the request is admitted. When this method returns,
  // schedule->is_admitted() is true if and only if the request was admitted.
  // For all calls to AdmitQuery(), ReleaseQuery() should also be called after
  // the query completes to ensure that the pool statistics are updated.
  Status AdmitQuery(QuerySchedule* schedule);

  // Updates the pool statistics when a query completes (either successfully,
  // is cancelled or failed). This should be called for all requests that have
  // been submitted via AdmitQuery(). (If the request was not admitted, this is
  // a no-op.)
  // This does not block.
  Status ReleaseQuery(QuerySchedule* schedule);

  // Registers with the subscription manager.
  Status Init(StatestoreSubscriber* subscriber);

 private:
  static const std::string IMPALA_REQUEST_QUEUE_TOPIC;

  // Structure stored in a QueryQueue representing a request. This struct lives only
  // during the call to AdmitQuery().
  struct QueueNode : public InternalQueue<QueueNode>::Node {
    QueueNode(const TUniqueId& id) : query_id(id) { }

    // Set when the request is admitted or rejected by the dequeuing thread. Used
    // by AdmitQuery() to wait for admission or until the timeout is reached.
    // The admission_ctrl_lock_ is not held while waiting on this promise, but
    // the lock should be held when checking the result because the dequeuing
    // thread holds it to Set().
    Promise<bool> is_admitted;

    // The query id of the queued request. This is only used for debug logging.
    const TUniqueId query_id;
  };

  // Metrics exposed for a pool.
  // Created by GetPoolMetrics() and stored in pool_metrics_map_.
  struct PoolMetrics {
    // Monotonically increasing counters since process start, i.e. counters:
    // The total number of requests that have been admitted locally. This includes
    // requests that are admitted immediately as well as requests that are admitted
    // after being queued.  Incremented when AdmitQuery() returns and the request is
    // admitted.
    Metrics::IntMetric* local_admitted;
    // The total number of requests that have been queued locally. Incremented
    // when a request is queued.
    Metrics::IntMetric* local_queued;
    // The total number of requests that have been rejected locally.  Incremented when
    // AdmitQuery() returns and the request is rejected because the queue is full.
    Metrics::IntMetric* local_rejected;
    // The total number of requests that timed out while waiting for admission locally.
    Metrics::IntMetric* local_timed_out;
    // The total number of requests that have completed locally. Incremented in
    // ReleaseQuery().
    Metrics::IntMetric* local_completed;
    // The total amount of time (in milliseconds) that locally queued requests have
    // spent waiting to be admitted.
    Metrics::IntMetric* local_time_in_queue_ms;

    // Instantaneous statistics, i.e. gauges:
    // The estimated total number of queries currently running across the cluster.
    Metrics::IntMetric* cluster_num_running;
    // The estimated total number of requests currently queued across the cluster.
    Metrics::IntMetric* cluster_in_queue;
    // The total number of queries currently running that were initiated locally.
    Metrics::IntMetric* local_num_running;
    // The total number of requests currently queued locally.
    Metrics::IntMetric* local_in_queue;
  };

  // Metrics subsystem access
  Metrics* metrics_;

  // Thread dequeuing and admitting queries.
  boost::scoped_ptr<Thread> dequeue_thread_;

  // Unique id for this impalad, used to construct topic keys.
  const std::string backend_id_;

  // Serializes/deserializes TPoolStats when sending and receiving topic updates.
  ThriftSerializer thrift_serializer_;

  // Protects all access to all variables below.
  // Coordinates access to the results of the promise QueueNode::is_admitted,
  // but the lock is not required to wait on the promise.
  boost::mutex admission_ctrl_lock_;

  // Map of pool names to pool statistics.
  typedef boost::unordered_map<std::string, TPoolStats> PoolStatsMap;

  // The local pool statistics. Updated when requests are executed, queued, and
  // completed.
  PoolStatsMap local_pool_stats_;

  // A set of pool names.
  typedef boost::unordered_set<std::string> PoolSet;

  // The set of local pools that have changed between topic updates that
  // need to be sent to the statestore.
  PoolSet pools_for_updates_;

  // The set of pools for which the total number of running queries has
  // decreased and thus may be eligible for admitting queued queries.
  PoolSet pools_to_dequeue_;

  // Mimics the statestore topic, i.e. stores a local copy of the logical data structure
  // that the statestore broadcasts. The local stats are not stored in this map because
  // we need to be able to clear the stats for all remote backends when a full topic
  // update is received. By storing the local pool stats in local_pool_stats_, we can
  // simply clear() the map.
  // Pool names -> full topic keys (i.e. "<topic>!<backend_id>") -> pool stats
  typedef boost::unordered_map<std::string, PoolStatsMap> PerBackendPoolStatsMap;
  PerBackendPoolStatsMap per_backend_pool_stats_map_;

  // The (estimated) total pool statistics for the entire cluster. Includes the current
  // local stats in local_pool_stats_. Updated when (a) IMPALA_REQUEST_QUEUE_TOPIC
  // updates are received by aggregating the stats in per_backend_pool_stats_map_ and (b)
  // when local stats change (i.e. AdmitQuery(), ReleaseQuery(), and when dequeuing in
  // DequeueLoop()).
  // Pool names -> estimated total pool stats
  PoolStatsMap cluster_pool_stats_;

  // Queue for the queries waiting to be admitted for execution. Once the
  // maximum number of concurrently executing queries has been reached,
  // incoming queries are queued and admitted FCFS.
  typedef InternalQueue<QueueNode> RequestQueue;

  // Map of pool names to request queues.
  typedef boost::unordered_map<std::string, RequestQueue> RequestQueueMap;
  RequestQueueMap request_queue_map_;

  // Map of pool names to pool metrics.
  typedef boost::unordered_map<std::string, PoolMetrics> PoolMetricsMap;
  PoolMetricsMap pool_metrics_map_;

  // Notifies the dequeuing thread that pool stats have changed and it may be
  // possible to dequeue and admit queries.
  boost::condition_variable dequeue_cv_;

  // If true, tear down the dequeuing thread. This only happens in unit tests.
  bool done_;

  // Statestore subscriber callback that updates the pool stats state.
  void UpdatePoolStats(
      const StatestoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
      std::vector<TTopicDelta>* subscriber_topic_updates);

  // Updates the per_backend_pool_stats_map_ with topic_updates.
  // Called by UpdatePoolStats(). Must hold admission_ctrl_lock_.
  void HandleTopicUpdates(const std::vector<TTopicItem>& topic_updates,
      PoolSet* updated_pools);

  // Removes stats from the per_backend_pool_stats_map_ from topic deletions.
  // Called by UpdatePoolStats(). Must hold admission_ctrl_lock_.
  void HandleTopicDeletions(const std::vector<std::string>& topic_deletions,
      PoolSet* updated_pools);

  // Re-computes the cluster_pool_stats_ aggregate stats for the updated pools.
  // Called by UpdatePoolStats() after handling updates and deletions.
  // Must hold admission_ctrl_lock_.
  void UpdateClusterAggregates(const PoolSet& updated_pools);

  // Adds updates for local pools that have changed to the subscriber topic updates.
  // Called by UpdatePoolStats() before handling updates. Takes admission_ctrl_lock_.
  void AddPoolUpdates(std::vector<TTopicDelta>* subscriber_topic_updates);

  // Dequeues and admits queued queries when notified by dequeue_cv_.
  void DequeueLoop();

  // Gets the metrics for a pool. The metrics are initialized if they don't already
  // exist. Returns NULL if there is no metrics system available.  Must hold
  // admission_ctrl_lock_.
  PoolMetrics* GetPoolMetrics(const std::string& pool_name);
};

}

#endif // SCHEDULING_ADMISSION_CONTROLLER_H
