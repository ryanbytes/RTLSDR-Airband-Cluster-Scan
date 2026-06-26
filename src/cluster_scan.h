/*
 * cluster_scan.h
 * Frequency clustering helpers for clustered scan mode.
 */

#ifndef _CLUSTER_SCAN_H
#define _CLUSTER_SCAN_H 1

#include <vector>

struct ClusterScanFrequency {
    int frequency;
    int channel_index;
};

struct ClusterScanCluster {
    int center_frequency;
    int min_frequency;
    int max_frequency;
    std::vector<int> channel_indices;
    std::vector<int> frequencies;
};

struct ClusterScanPlan {
    int sample_rate;
    int edge_guard_hz;
    int max_cluster_width_hz;
    int usable_bandwidth_hz;
    std::vector<ClusterScanCluster> clusters;
};

ClusterScanPlan build_cluster_scan_plan(const std::vector<ClusterScanFrequency>& frequencies,
                                        int sample_rate,
                                        int edge_guard_hz,
                                        int max_cluster_width_hz,
                                        bool prefer_more_channels_per_cluster);

std::vector<std::vector<int> > assign_clusters_round_robin(int cluster_count, int device_count);

#endif /* _CLUSTER_SCAN_H */
