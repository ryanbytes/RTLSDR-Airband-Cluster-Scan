/*
 * cluster_scan.cpp
 * Frequency clustering helpers for clustered scan mode.
 */

#include "cluster_scan.h"

#include <algorithm>
#include <stdexcept>

using namespace std;

static int usable_bandwidth(int sample_rate, int edge_guard_hz, int max_cluster_width_hz) {
    int usable = sample_rate - 2 * edge_guard_hz;
    if (usable <= 0) {
        throw invalid_argument("edge_guard_hz leaves no usable bandwidth");
    }
    if (max_cluster_width_hz > 0 && max_cluster_width_hz < usable) {
        usable = max_cluster_width_hz;
    }
    return usable;
}

ClusterScanPlan build_cluster_scan_plan(const vector<ClusterScanFrequency>& frequencies,
                                        int sample_rate,
                                        int edge_guard_hz,
                                        int max_cluster_width_hz,
                                        bool prefer_more_channels_per_cluster) {
    if (sample_rate <= 0) {
        throw invalid_argument("sample_rate must be positive");
    }
    if (edge_guard_hz < 0) {
        throw invalid_argument("edge_guard_hz must not be negative");
    }

    ClusterScanPlan plan;
    plan.sample_rate = sample_rate;
    plan.edge_guard_hz = edge_guard_hz;
    plan.max_cluster_width_hz = max_cluster_width_hz;
    plan.usable_bandwidth_hz = usable_bandwidth(sample_rate, edge_guard_hz, max_cluster_width_hz);

    vector<ClusterScanFrequency> sorted = frequencies;
    sort(sorted.begin(), sorted.end(), [](const ClusterScanFrequency& a, const ClusterScanFrequency& b) {
        if (a.frequency == b.frequency) {
            return a.channel_index < b.channel_index;
        }
        return a.frequency < b.frequency;
    });

    size_t start = 0;
    while (start < sorted.size()) {
        size_t end = start;
        if (prefer_more_channels_per_cluster) {
            while (end + 1 < sorted.size() && sorted[end + 1].frequency - sorted[start].frequency <= plan.usable_bandwidth_hz) {
                end++;
            }
        }

        ClusterScanCluster cluster;
        cluster.min_frequency = sorted[start].frequency;
        cluster.max_frequency = sorted[end].frequency;
        cluster.center_frequency = cluster.min_frequency + (cluster.max_frequency - cluster.min_frequency) / 2;
        for (size_t i = start; i <= end; i++) {
            cluster.channel_indices.push_back(sorted[i].channel_index);
            cluster.frequencies.push_back(sorted[i].frequency);
        }
        plan.clusters.push_back(cluster);

        start = end + 1;
    }

    return plan;
}

vector<vector<int> > assign_clusters_round_robin(int cluster_count, int device_count) {
    if (cluster_count < 0) {
        throw invalid_argument("cluster_count must not be negative");
    }
    if (device_count <= 0) {
        throw invalid_argument("device_count must be positive");
    }

    vector<vector<int> > assignments((size_t)device_count);
    for (int cluster = 0; cluster < cluster_count; cluster++) {
        assignments[(size_t)(cluster % device_count)].push_back(cluster);
    }
    return assignments;
}
